/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/bug.h"
#include "ember/bkl.h"
#include "ember/paging.h"

/* ---- Exit (preemptive model) ---- */
void
do_exit(syscall_frame_t * f, int exit_code)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	proc_t *cur = current_proc;

	if (!cur || cur->pid <= 1) {
		/* PID 1 (init) exiting: poweroff. */
		console_write("PID 1 EXIT code=");
		print_num((uint64_t) exit_code);
		if (cur && cur->exit_signal) {
			console_write(" signal=");
			print_num((uint64_t) cur->exit_signal);
		}
		console_write(" rip=");
		console_hex64(f->rip);
		console_write(" rsp=");
		console_hex64(f->rsp);
		console_write("\n");
		serial_flush();
		qemu_poweroff();
		for (;;)
			__asm__ __volatile__("hlt");
	}

	/* Wake vfork parent if this is a vfork child exiting without exec. */
	if (cur->vfork_parent) {
		int wake_chan = cur->pid | 0x80000000;
		cur->vfork_parent = 0;
		sched_wakeup(wake_chan);
	}

	/* Close all fds via file_desc_unref. */
	for (int i = 0; i < MAX_FDS; i++) {
		if (cur->fds[i].desc) {
			file_desc_unref(cur->fds[i].desc);
			cur->fds[i].desc = 0;
			cur->fds[i].fd_flags = 0;
		}
	}

	/* Clear child tid and futex wake if set. */
	if (cur->clear_child_tid) {
		/*
		 * Write 0 to the user-space clear_child_tid address.
		 * The page may not be mapped (thread stack munmap'd,
		 * or stale pointer from pre-exec address space).
		 * Check PTE before writing — ember has no put_user().
		 */
		uint64_t *pte = paging_walk_pte(cur->pml4_phys,
			cur->clear_child_tid);
		if (pte && (*pte & 1)) {
			uint64_t old_cr3 = read_cr3();
			write_cr3(cur->pml4_phys);
			*(volatile uint32_t *)(uintptr_t) cur->clear_child_tid = 0;
			write_cr3(old_cr3);
		}
		int futex_chan =
		    (int)((cur->
			   clear_child_tid >> 2) & 0x7fffffff) | 0x40000000;
		sched_wakeup(futex_chan);
	}

	/* Reparent children to init (pid 1) and reap any that are already zombies. */
	{
		int freed[MAX_PROCS];
		int nfreed = 0;
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		for (int i = 0; i < MAX_PROCS; i++) {
			if (procs[i].state != PROC_UNUSED
			    && procs[i].ppid == cur->pid) {
				procs[i].ppid = 1;
				if (procs[i].state == PROC_ZOMBIE) {
					/* Orphaned zombie -- reap immediately. */
					if (procs[i].pml4_phys) {
						int shared = 0;
						for (int j = 0; j < MAX_PROCS;
						     j++) {
							if (j != i
							    && procs[j].state !=
							    PROC_UNUSED
							    && procs[j].
							    pml4_phys ==
							    procs[i].
							    pml4_phys) {
								shared = 1;
								break;
							}
						}
						if (!shared)
							paging_free_user_pml4
							    (procs[i].
							     pml4_phys);
					}
					procs[i].pml4_phys = 0;
					procs[i].state = PROC_UNUSED;
					procs[i].pid = 0;
					freed[nfreed++] = i;
				}
			}
		}
		spin_unlock_irqrestore(&sched_lock, sf);
		for (int f = 0; f < nfreed; f++)
			proc_free_slot(freed[f]);
	}

	/*
	 * Atomically: set ZOMBIE + wake parent. If parent has SIGCHLD=SIG_IGN,
	 * auto-reap instead of becoming a zombie.
	 */
	{
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		int auto_reap = 0;
		for (int i = 0; i < MAX_PROCS; i++) {
			if (procs[i].state != PROC_UNUSED
			    && procs[i].pid == cur->ppid) {
				if (procs[i].sig_handlers[SIGCHLD] == 1) {
					/* SIG_IGN: auto-reap this child. */
					auto_reap = 1;
				} else {
					procs[i].sig_pending |= (1u << SIGCHLD);
					if (procs[i].state == PROC_SLEEPING)
						procs[i].state = PROC_READY;
				}
				break;
			}
		}
		if (auto_reap) {
			int idx = (int)(cur - procs);
			uint64_t zpml4 = cur->pml4_phys;
			cur->pml4_phys = 0;
			cur->state = PROC_UNUSED;
			cur->pid = 0;
			spin_unlock_irqrestore(&sched_lock, sf);
			proc_free_slot(idx);
			if (zpml4) {
				int shared = 0;
				uint64_t sf2 = spin_lock_irqsave(&sched_lock);
				for (int j = 0; j < MAX_PROCS; j++) {
					if (procs[j].state != PROC_UNUSED
					    && procs[j].pml4_phys == zpml4) {
						shared = 1;
						break;
					}
				}
				spin_unlock_irqrestore(&sched_lock, sf2);
				if (!shared) {
					/*
					 * Switch CR3 to boot PML4 BEFORE freeing -- the
					 * current CPU's CR3 still points to zpml4.
					 * Verified: models/idle_cr3.pml.
					 */
					extern uint64_t kernel_idle_cr3;
					if (kernel_idle_cr3)
						write_cr3(kernel_idle_cr3);
					paging_free_user_pml4(zpml4);
				}
			}
		} else {
			cur->state = PROC_ZOMBIE;
			cur->exit_code = exit_code;
			spin_unlock_irqrestore(&sched_lock, sf);
		}
	}

	schedule();
	for (;;)
		__asm__ __volatile__("hlt");
}

/* ---- Exit from ISR (hardware exception) ---- */
void
do_exit_from_isr(int sig)
{
	proc_t *cur = current_proc;
	if (!cur)
		return;
	cur->exit_signal = sig;
	syscall_frame_t dummy;
	kmemzero(&dummy, sizeof(dummy));
	do_exit(&dummy, 0);
}
