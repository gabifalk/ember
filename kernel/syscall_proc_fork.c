/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/bug.h"
#include "ember/bkl.h"

/* ---- Shared helpers for fork/vfork/clone ---- */

/* Copy VMAs, paths, metadata, and signal state from parent to child. */
static void
copy_proc_state(proc_t * child, proc_t * parent)
{
	/* Copy VMAs. */
	for (int i = 0; i < MAX_VMAS; i++) {
		child->vmas[i].start = parent->vmas[i].start;
		child->vmas[i].length = parent->vmas[i].length;
		child->vmas[i].prot = parent->vmas[i].prot;
		child->vmas[i].used = parent->vmas[i].used;
	}

	/* Copy cwd, exe_path, root_path. */
	for (int ci = 0; ci < EMBER_PATH_MAX; ci++)
		child->cwd[ci] = parent->cwd[ci];
	for (int ci = 0; ci < EMBER_PATH_MAX; ci++)
		child->exe_path[ci] = parent->exe_path[ci];
	for (int ci = 0; ci < EMBER_PATH_MAX; ci++)
		child->root_path[ci] = parent->root_path[ci];
	child->umask = parent->umask;
	child->pgid = parent->pgid;
	child->sid = parent->sid;

	/* Copy signal state. */
	child->sig_mask = parent->sig_mask;
	child->sig_pending = 0;
	for (int s = 0; s < NSIG; s++) {
		child->sig_handlers[s] = parent->sig_handlers[s];
		child->sig_flags[s] = parent->sig_flags[s];
		child->sig_restorer[s] = parent->sig_restorer[s];
		child->sig_sa_mask[s] = parent->sig_sa_mask[s];
	}
	child->alarm_tick = 0;
	child->itimer_interval_ticks = 0;
	child->reported_stopped = 0;
	child->clear_child_tid = 0;
}

/* Copy fd table from parent to child, bumping refcounts. */
static void
copy_fd_table(proc_t * child, proc_t * parent)
{
	for (int i = 0; i < MAX_FDS; i++) {
		child->fds[i].desc = parent->fds[i].desc;
		child->fds[i].fd_flags = parent->fds[i].fd_flags;
		if (child->fds[i].desc)
			file_desc_ref(child->fds[i].desc);
	}
}

/*
 * Copy parent's syscall frame to child kstack and set up context_switch frame.
 * Returns the child's syscall frame pointer (child_frame->rax is set to 0).
 */
static syscall_frame_t *
setup_child_kstack(proc_t * child, syscall_frame_t * parent_frame)
{
	uint64_t child_kstack_top =
	    (uint64_t) (uintptr_t) (child->kstack + PROC_KSTACK_SIZE);
	syscall_frame_t *child_frame =
	    (syscall_frame_t *) (child_kstack_top - sizeof(syscall_frame_t));
	{
		uint8_t *dst = (uint8_t *) child_frame;
		uint8_t *src = (uint8_t *) parent_frame;
		for (uint64_t i = 0; i < sizeof(syscall_frame_t); i++)
			dst[i] = src[i];
	}
	child_frame->rax = 0;

	/*
	 * Fabricate a context_switch frame below the syscall frame.
	 * Layout (low to high): r15, r14, r13, r12, rbx, rbp, return_addr.
	 */
	uint64_t *sw = (uint64_t *) ((uint8_t *) child_frame - 7 * 8);
	sw[0] = 0;		/* R15. */
	sw[1] = 0;		/* R14. */
	sw[2] = 0;		/* R13. */
	sw[3] = 0;		/* R12. */
	sw[4] = 0;		/* Rbx. */
	sw[5] = 0;		/* Rbp. */
	sw[6] = (uint64_t) (uintptr_t) fork_child_return;

	child->saved_ksp = (uint64_t) (uintptr_t) sw;
	return child_frame;
}

/* ---- Fork (preemptive model) ---- */
uint64_t
do_fork(syscall_frame_t * f)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	proc_t *parent = current_proc;
	if (!parent) {
		console_write("fork: no parent\n");
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}
	/* Allocate child process. */
	proc_t *child = proc_alloc();
	if (!child) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	child->ppid = parent->pid;

	/* Deep-copy parent's address space (marks shared pages COW read-only) */
	child->pml4_phys = paging_clone_user_pml4(parent->pml4_phys);
	if (!child->pml4_phys) {
		child->state = PROC_UNUSED;
		child->pid = 0;
		proc_free_slot((int)(child - procs));
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}
	/*
	 * Flush parent's TLB: clone changed parent's PTEs to COW read-only,
	 * but CPU TLB still has writable entries. Without flush, parent can
	 * write to shared pages via stale TLB. Verified: models/tlb.pml.
	 */
	write_cr3(parent->pml4_phys);

	/* Copy brk/mmap state. */
	child->brk = parent->brk;
	child->brk_base = parent->brk_base;
	child->mmap_next = parent->mmap_next;
	child->fs_base = parent->fs_base;

	copy_proc_state(child, parent);
	child->tgid = child->pid;	/* Fork creates new thread group. */

	copy_fd_table(child, parent);

	setup_child_kstack(child, f);

	/* Copy parent's live FPU/SSE state to child (fpu.pml model).
	 * Use register operand: tcc miscompiles "=m"(struct->member) for
	 * fxsave, placing the address in a stack temp then saving to the
	 * temp's address instead of the member's address. */
	{
		uint8_t *pfx = fxsave_ptr(parent);
		uint8_t *cfx = fxsave_ptr(child);
		__asm__ __volatile__("fxsave (%0)" : : "r"(pfx) : "memory");
		for (int i = 0; i < 512; i++)
			cfx[i] = pfx[i];
	}

	{
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		child->state = PROC_READY;
		spin_unlock_irqrestore(&sched_lock, sf);
	}
	/* Parent continues; return child pid. */
	f->rax = (uint64_t) child->pid;
	return f->rax;
}

/* ---- Vfork (share address space, suspend parent) ---- */
uint64_t
do_vfork(syscall_frame_t * f)
{
	proc_t *parent = current_proc;
	if (!parent) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	proc_t *child = proc_alloc();
	if (!child) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	child->ppid = parent->pid;

	/* Share parent's address space -- NO clone. */
	child->pml4_phys = parent->pml4_phys;

	/* Copy brk/mmap state. */
	child->brk = parent->brk;
	child->brk_base = parent->brk_base;
	child->mmap_next = parent->mmap_next;
	child->fs_base = parent->fs_base;

	copy_proc_state(child, parent);
	child->tgid = child->pid;

	/* Mark child as vfork child. */
	child->vfork_parent = parent->pid;

	copy_fd_table(child, parent);

	setup_child_kstack(child, f);

	/* Mark child READY, suspend parent. */
	{
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		child->state = PROC_READY;
		parent->state = PROC_SLEEPING;
		parent->wait_chan = child->pid | 0x80000000;
		spin_unlock_irqrestore(&sched_lock, sf);
	}

	/*
	 * Parent returns child pid. Set it now before sleeping so that
	 * when the parent is woken, rax already has the right value.
	 */
	f->rax = (uint64_t) child->pid;

	/*
	 * Suspend parent until vfork child completes exec or _exit.
	 * SIGCHLD from other children can wake us spuriously -- re-check
	 * under sched_lock and go back to sleep if the vfork child hasn't
	 * finished yet.  Verified: models/vfork_sigchld.pml.
	 */
	{
		int child_idx = (int)(child - procs);
		for (;;) {
			schedule();
			uint64_t sf2 = spin_lock_irqsave(&sched_lock);
			if (procs[child_idx].vfork_parent == 0 ||
			    procs[child_idx].state == PROC_UNUSED) {
				spin_unlock_irqrestore(&sched_lock, sf2);
				break;
			}
			/* Spurious wake: go back to sleep. */
			parent->state = PROC_SLEEPING;
			parent->wait_chan = child->pid | 0x80000000;
			spin_unlock_irqrestore(&sched_lock, sf2);
		}
	}

	return f->rax;
}

/* ---- Clone with CLONE_VM (thread creation) ---- */
uint64_t
do_clone_thread(syscall_frame_t * f, uint64_t clone_flags)
{
	proc_t *parent = current_proc;
	if (!parent) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	uint64_t child_stack = f->rsi;
	uint64_t parent_tidptr = f->rdx;
	uint64_t child_tidptr = f->r10;
	uint64_t tls = f->r8;

	proc_t *child = proc_alloc();
	if (!child) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	/* CLONE_VM: share parent's page table. */
	child->pml4_phys = parent->pml4_phys;

	/* CLONE_THREAD: same thread group. */
	if (clone_flags & CLONE_THREAD) {
		child->tgid = parent->tgid;
		child->ppid = parent->ppid;
	} else {
		child->tgid = child->pid;
		child->ppid = parent->pid;
	}

	/* Copy brk/mmap state. */
	child->brk = parent->brk;
	child->brk_base = parent->brk_base;
	child->mmap_next = parent->mmap_next;
	child->fs_base = parent->fs_base;

	/* CLONE_SETTLS. */
	if (clone_flags & CLONE_SETTLS)
		child->fs_base = tls;

	copy_proc_state(child, parent);

	/* CLONE_CHILD_CLEARTID. */
	if (clone_flags & CLONE_CHILD_CLEARTID)
		child->clear_child_tid = child_tidptr;

	copy_fd_table(child, parent);

	/* CLONE_PARENT_SETTID: write child pid to parent_tidptr. */
	if (clone_flags & CLONE_PARENT_SETTID) {
		USER_ACCESS_BEGIN();
		*(uint32_t *) (uintptr_t) parent_tidptr = (uint32_t) child->pid;
		USER_ACCESS_END();
	}

	/* CLONE_CHILD_SETTID: write child pid to child_tidptr. */
	if (clone_flags & CLONE_CHILD_SETTID) {
		USER_ACCESS_BEGIN();
		*(uint32_t *) (uintptr_t) child_tidptr = (uint32_t) child->pid;
		USER_ACCESS_END();
	}

	syscall_frame_t *child_frame = setup_child_kstack(child, f);
	if (child_stack)
		child_frame->rsp = child_stack;

	{
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		child->state = PROC_READY;
		spin_unlock_irqrestore(&sched_lock, sf);
	}

	f->rax = (uint64_t) child->pid;
	return f->rax;
}
