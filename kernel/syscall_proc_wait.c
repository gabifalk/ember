/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/bug.h"
#include "ember/bkl.h"

/* ---- Wait4 ---- */
uint64_t
do_wait4(syscall_frame_t * f)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	int wait_pid = (int)(int64_t) f->rdi;
	uint64_t status_ptr = f->rsi;
	int options = (int)f->rdx;

	proc_t *cur = current_proc;
	if (!cur) {
		f->rax = SYSCALL_ERR(ECHILD);
		return f->rax;
	}

	for (;;) {
		uint64_t sf = spin_lock_irqsave(&sched_lock);

		/* Find a zombie or stopped child. */
		proc_t *zombie = 0;
		proc_t *stopped = 0;
		int has_children = 0;
		for (int i = 0; i < MAX_PROCS; i++) {
			if (procs[i].state != PROC_UNUSED
			    && procs[i].ppid == cur->pid) {
				/* For wait_pid < -1: match pgid; for -1: any; for 0: match caller pgid. */
				if (wait_pid > 0 && procs[i].pid != wait_pid)
					continue;
				if (wait_pid < -1 && procs[i].pgid != -wait_pid)
					continue;
				if (wait_pid == 0 && procs[i].pgid != cur->pgid)
					continue;
				has_children = 1;
				if (procs[i].state == PROC_ZOMBIE) {
					zombie = &procs[i];
					break;
				}
				if ((options & WUNTRACED)
				    && procs[i].state == PROC_STOPPED
				    && !procs[i].reported_stopped) {
					stopped = &procs[i];
				}
			}
		}

		if (zombie) {
			int child_pid = zombie->pid;
			int exit_code = zombie->exit_code;
			int exit_sig = zombie->exit_signal;
			uint64_t zpml4 = zombie->pml4_phys;
			int zombie_idx = (int)(zombie - procs);
			zombie->pml4_phys = 0;
			zombie->state = PROC_UNUSED;
			zombie->pid = 0;
			spin_unlock_irqrestore(&sched_lock, sf);
			proc_free_slot(zombie_idx);

			if (zpml4) {
				int shared = 0;
				for (int i = 0; i < MAX_PROCS; i++) {
					if (procs[i].state != PROC_UNUSED &&
					    procs[i].pml4_phys == zpml4) {
						shared = 1;
						break;
					}
				}
				if (!shared)
					paging_free_user_pml4(zpml4);
			}

			if (status_ptr) {
				uint32_t status_val;
				if (exit_sig > 0)
					status_val =
					    (uint32_t) (exit_sig & 0x7f);
				else
					status_val =
					    (uint32_t) ((exit_code & 0xff) <<
							8);
				USER_ACCESS_BEGIN();
				*(uint32_t *) (uintptr_t) status_ptr =
				    status_val;
				USER_ACCESS_END();
			}

			f->rax = (uint64_t) child_pid;
			return f->rax;
		}

		if (stopped) {
			stopped->reported_stopped = 1;
			int child_pid = stopped->pid;
			int stop_sig = stopped->exit_code;	/* Signal that caused stop. */
			spin_unlock_irqrestore(&sched_lock, sf);

			if (status_ptr) {
				uint32_t status_val =
				    (uint32_t) ((stop_sig << 8) | 0x7f);
				USER_ACCESS_BEGIN();
				*(uint32_t *) (uintptr_t) status_ptr =
				    status_val;
				USER_ACCESS_END();
			}

			f->rax = (uint64_t) child_pid;
			return f->rax;
		}

		if (!has_children) {
			spin_unlock_irqrestore(&sched_lock, sf);
			f->rax = SYSCALL_ERR(ECHILD);
			return f->rax;
		}

		if (options & WNOHANG) {
			spin_unlock_irqrestore(&sched_lock, sf);
			f->rax = 0;
			return 0;
		}

		/*
		 * Check for pending signals before blocking.
		 * SIGCHLD must NEVER interrupt wait -- if a user handler
		 * is installed, SA_RESTART would re-execute wait4 after
		 * the handler, but the handler's waitpid(WNOHANG) may
		 * have already reaped the zombie, causing ECHILD.
		 * Verified: models/wait_sigchld.pml.
		 */
		{
			uint32_t pend = cur->sig_pending & ~cur->sig_mask;
			if (pend & (1u << SIGCHLD)) {
				/*
				 * Don't let SIGCHLD cause EINTR.  Clear
				 * sig_pending only for SIG_DFL/SIG_IGN
				 * (no handler to run).  For user handlers,
				 * leave sig_pending so signal_deliver
				 * fires the handler after wait4 returns.
				 */
				if (cur->sig_handlers[SIGCHLD] <= SIG_IGN)
					cur->sig_pending &= ~(1u << SIGCHLD);
				pend &= ~(1u << SIGCHLD);
			}
			if (pend) {
				spin_unlock_irqrestore(&sched_lock, sf);
				f->rax = SYSCALL_ERR(EINTR);
				return f->rax;
			}
		}

		cur->state = PROC_SLEEPING;
		cur->wait_chan = 0;
		spin_unlock_irqrestore(&sched_lock, sf);
		schedule();
	}
}

/* ---- Waitid ---- */
uint64_t
do_waitid(syscall_frame_t * f)
{
	/* Waitid(idtype, id, infop, options, rusage) */
	int idtype = (int)f->rdi;
	int id = (int)(int64_t) f->rsi;
	uint64_t user_infop = f->rdx;
	int options = (int)f->r10;

	proc_t *cur = current_proc;
	if (!cur) {
		f->rax = SYSCALL_ERR(ECHILD);
		return f->rax;
	}

	for (;;) {
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		proc_t *zombie = 0;
		int has_children = 0;
		for (int i = 0; i < MAX_PROCS; i++) {
			if (procs[i].state == PROC_UNUSED)
				continue;
			if (procs[i].ppid != cur->pid)
				continue;
			if (idtype == P_PID && procs[i].pid != id)
				continue;
			if (idtype == P_PGID && procs[i].pgid != id)
				continue;
			has_children = 1;
			if ((options & WEXITED)
			    && procs[i].state == PROC_ZOMBIE) {
				zombie = &procs[i];
				break;
			}
		}
		if (zombie) {
			int child_pid = zombie->pid;
			int exit_code = zombie->exit_code;
			int exit_sig = zombie->exit_signal;
			int zombie_idx = (int)(zombie - procs);
			int did_reap = 0;
			if (!(options & WNOWAIT)) {
				if (zombie->pml4_phys) {
					int shared = 0;
					for (int j = 0; j < MAX_PROCS; j++) {
						if (&procs[j] != zombie
						    && procs[j].state !=
						    PROC_UNUSED
						    && procs[j].pml4_phys ==
						    zombie->pml4_phys) {
							shared = 1;
							break;
						}
					}
					if (!shared)
						paging_free_user_pml4(zombie->
								      pml4_phys);
				}
				zombie->pml4_phys = 0;
				zombie->state = PROC_UNUSED;
				zombie->pid = 0;
				did_reap = 1;
			}
			spin_unlock_irqrestore(&sched_lock, sf);
			if (did_reap)
				proc_free_slot(zombie_idx);
			/* Fill siginfo_t at user_infop. */
			if (user_infop) {
				USER_ACCESS_BEGIN();
				/* siginfo_t is 128 bytes; zero it first. */
				uint8_t *si =
				    (uint8_t *) (uintptr_t) user_infop;
				for (int k = 0; k < 128; k++)
					si[k] = 0;
				/* si_signo at offset 0 (int) */
				*(int *)(si + 0) = 17;	/* SIGCHLD. */
				/* si_code at offset 8 (int) -- CLD_EXITED=1, CLD_KILLED=2. */
				*(int *)(si + 8) = (exit_sig > 0) ? 2 : 1;
				/* si_pid at offset 16 (int on x86_64 siginfo_t._sifields._sigchld) */
				/* Actually: _si_pid is at offset 16 in the _kill union. */
				*(int *)(si + 16) = child_pid;
				/* si_uid at offset 20. */
				*(int *)(si + 20) = 0;
				/* si_status at offset 24. */
				*(int *)(si + 24) =
				    (exit_sig > 0) ? exit_sig : exit_code;
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 0;
		}
		if (!has_children) {
			spin_unlock_irqrestore(&sched_lock, sf);
			f->rax = SYSCALL_ERR(ECHILD);
			return f->rax;
		}
		if (options & WNOHANG) {
			spin_unlock_irqrestore(&sched_lock, sf);
			/* Zero infop to indicate no state change. */
			if (user_infop) {
				USER_ACCESS_BEGIN();
				uint8_t *si =
				    (uint8_t *) (uintptr_t) user_infop;
				for (int k = 0; k < 128; k++)
					si[k] = 0;
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 0;
		}
		/* Check for pending signals (exclude SIGCHLD -- see wait4). */
		{
			uint32_t pend = cur->sig_pending & ~cur->sig_mask;
			if (pend & (1u << SIGCHLD)) {
				if (cur->sig_handlers[SIGCHLD] <= SIG_IGN)
					cur->sig_pending &= ~(1u << SIGCHLD);
				pend &= ~(1u << SIGCHLD);
			}
			if (pend) {
				spin_unlock_irqrestore(&sched_lock, sf);
				f->rax = SYSCALL_ERR(EINTR);
				return f->rax;
			}
		}
		cur->state = PROC_SLEEPING;
		cur->wait_chan = 0;
		spin_unlock_irqrestore(&sched_lock, sf);
		schedule();
	}
}
