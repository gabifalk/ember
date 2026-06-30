/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/isr.h"
#include "ember/bug.h"
#include "ember/bkl.h"

#define SI_KERNEL    0x80	/* si_code: sent by kernel. */
#define SIGINFO_SIZE 128	/* sizeof(siginfo_t) on x86_64. */

/*
 * Check if the first actionable pending signal has SA_RESTART set.
 * Signals whose default action is "ignore" (SIGCHLD with SIG_DFL/SIG_IGN)
 * are silently cleared and skipped -- they must not interrupt blocking syscalls.
 * Returns 1 if the syscall should restart, 0 if it should return -EINTR.
 */
int
pending_signal_restarts(proc_t * cur)
{
	uint32_t pending = cur->sig_pending & ~cur->sig_mask;
	for (int i = 1; i < NSIG; i++) {
		if (pending & (1u << i)) {
			/*
			 * SIGCHLD with default/ignore handler: default action is ignore.
			 * Clear it and skip to the next pending signal.
			 */
			if (i == SIGCHLD && cur->sig_handlers[i] <= SIG_IGN) {
				cur->sig_pending &= ~(1u << i);
				continue;
			}
			if (cur->sig_handlers[i] > SIG_IGN
			    && (cur->sig_flags[i] & SA_RESTART))
				return 1;
			return 0;
		}
	}
	return 1;		/* Only default-ignore signals were pending -- restart. */
}

/* Helper: send signal to a single process by pid. */
static int
do_kill_pid(int pid, int sig)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	if (sig < 0 || sig >= NSIG)
		return -EINVAL;
	proc_t *target = proc_find(pid);
	if (!target)
		return -ESRCH;
	if (sig != 0) {
		uint64_t sf = spin_lock_irqsave(&sched_lock);
		target->sig_pending |= (1u << sig);
		/* SIGCONT wakes stopped processes. */
		if (sig == SIGCONT && target->state == PROC_STOPPED) {
			target->state = PROC_READY;
			target->reported_stopped = 0;
		}
		if (target->state == PROC_SLEEPING)
			target->state = PROC_READY;
		spin_unlock_irqrestore(&sched_lock, sf);
	}
	return 0;
}

/* ---- Signal frame setup helper ---- */

/*
 * Common signal frame layout built on the user stack.
 * Returned to callers so they can write the saved frame.
 */
typedef struct {
	uint64_t sp;		/* Final user RSP (return-address slot) */
	uint64_t saved_frame_addr;
	uint64_t siginfo_addr;	/* 0 If !SA_SIGINFO. */
} sig_frame_info_t;

/*
 * Build the signal trampoline frame on the user stack.
 * Handles: stack alignment, siginfo, saved-mask, trampoline/restorer,
 * sa_mask application.  The caller is responsible for writing the
 * saved syscall_frame_t at info->saved_frame_addr (it differs between
 * the syscall and ISR paths).
 */
sig_frame_info_t
setup_signal_frame(proc_t * cur, int sig, uint64_t user_sp,
		   uint64_t ucr3, uint64_t old_cr3)
{
	int is_siginfo = (cur->sig_flags[sig] & SA_SIGINFO) ? 1 : 0;

	uint64_t sp = user_sp;
	sp -= 128;		/* Skip x86_64 red zone (128 bytes below RSP reserved by ABI) */
	sp &= ~15ULL;		/* 16-Byte align. */

	/* Optionally allocate siginfo_t (SIGINFO_SIZE bytes) */
	uint64_t siginfo_addr = 0;
	if (is_siginfo) {
		sp -= SIGINFO_SIZE;
		siginfo_addr = sp;
	}

	/* Allocate space for saved frame + 8-byte checksum. */
	sp -= sizeof(syscall_frame_t) + 8;
	uint64_t saved_frame_addr = sp;

	/* Allocate space for saved_sig_mask (8 bytes) */
	sp -= 8;
	uint64_t saved_mask_addr = sp;

	/* Allocate space for trampoline code (16 bytes) */
	sp -= 16;
	uint64_t trampoline_addr = sp;

	/* Allocate return address (8 bytes), making sp 8 mod 16 for ABI. */
	sp -= 8;

	/* Write to user stack. */
	if (ucr3)
		write_cr3(ucr3);

	/* Write siginfo if SA_SIGINFO. */
	if (is_siginfo) {
		uint8_t *si = (uint8_t *) (uintptr_t) siginfo_addr;
		kmemzero(si, SIGINFO_SIZE);
		/* si_signo at offset 0 (int32) */
		*(int *)(uintptr_t) siginfo_addr = sig;
		/* si_code at offset 8 (int32) = SI_KERNEL. */
		*(int *)(uintptr_t) (siginfo_addr + 8) = SI_KERNEL;
	}

	/* NOTE: caller writes the saved frame at saved_frame_addr. */

	/* Write saved signal mask. */
	*(uint64_t *) (uintptr_t) saved_mask_addr = (uint64_t) cur->sig_mask;

	/*
	 * Set return address: use sa_restorer if SA_RESTORER is set,
	 * otherwise write a kernel trampoline.
	 */
	if (cur->sig_flags[sig] & SA_RESTORER) {
		/* Use user-provided restorer as return address. */
		*(uint64_t *) (uintptr_t) sp = cur->sig_restorer[sig];
	} else {
		/* Write trampoline: mov $15, %rax; syscall; nop padding. */
		uint8_t *tramp = (uint8_t *) (uintptr_t) trampoline_addr;
		tramp[0] = 0x48;
		tramp[1] = 0xc7;
		tramp[2] = 0xc0;	/* Mov $imm32, %rax. */
		tramp[3] = 0x0f;
		tramp[4] = 0x00;
		tramp[5] = 0x00;
		tramp[6] = 0x00;	/* Imm32 = 15. */
		tramp[7] = 0x0f;
		tramp[8] = 0x05;	/* Syscall. */
		for (int i = 9; i < 16; i++)
			tramp[i] = 0x90;	/* Nop padding. */
		*(uint64_t *) (uintptr_t) sp = trampoline_addr;
	}

	if (ucr3)
		write_cr3(old_cr3);

	/* Apply sa_mask: block signals in sa_mask | self during handler. */
	cur->sig_mask |= cur->sig_sa_mask[sig] | (1u << sig);
	cur->sig_mask &= ~(1u << SIGKILL);	/* Never mask SIGKILL. */

	sig_frame_info_t info;
	info.sp = sp;
	info.saved_frame_addr = saved_frame_addr;
	info.siginfo_addr = siginfo_addr;
	return info;
}

/* ---- Signal delivery ---- */
void
signal_deliver(syscall_frame_t * f)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	proc_t *cur = current_proc;
	if (!cur)
		return;

	uint64_t sf = spin_lock_irqsave(&sched_lock);

	/* Check alarm expiry. */
	if (cur->alarm_tick && kernel_ticks >= cur->alarm_tick) {
		cur->sig_pending |= (1u << SIGALRM);
		/* Reload from interval if repeating, otherwise clear. */
		if (cur->itimer_interval_ticks > 0)
			cur->alarm_tick =
			    kernel_ticks + cur->itimer_interval_ticks;
		else
			cur->alarm_tick = 0;
	}

	uint32_t pending = cur->sig_pending & ~cur->sig_mask;
	if (!pending) {
		spin_unlock_irqrestore(&sched_lock, sf);
		return;
	}

	/* Find lowest pending signal. */
	int sig = 0;
	for (int i = 1; i < NSIG; i++) {
		if (pending & (1u << i)) {
			sig = i;
			break;
		}
	}
	if (sig == 0) {
		spin_unlock_irqrestore(&sched_lock, sf);
		return;
	}

	/* Clear pending bit. */
	cur->sig_pending &= ~(1u << sig);
	spin_unlock_irqrestore(&sched_lock, sf);

	uint64_t handler = cur->sig_handlers[sig];

	if (handler == SIG_IGN) {
		/* SIGCONT: even if ignored, still continue stopped processes. */
		if (sig == SIGCONT) {
			/* Wake all stopped processes in same pgid. */
			uint64_t sf = spin_lock_irqsave(&sched_lock);
			for (int i = 0; i < MAX_PROCS; i++) {
				if (procs[i].state == PROC_STOPPED
				    && procs[i].pgid == cur->pgid) {
					procs[i].state = PROC_READY;
					procs[i].reported_stopped = 0;
				}
			}
			spin_unlock_irqrestore(&sched_lock, sf);
		}
		return;
	}

	if (handler == SIG_DFL) {
		/* Default actions. */
		if (sig == SIGCHLD) {
			return;
		}
		if (sig == SIGCONT) {
			/* Wake all stopped processes in same pgid. */
			uint64_t sf = spin_lock_irqsave(&sched_lock);
			for (int i = 0; i < MAX_PROCS; i++) {
				if (procs[i].state == PROC_STOPPED
				    && procs[i].pgid == cur->pgid) {
					procs[i].state = PROC_READY;
					procs[i].reported_stopped = 0;
				}
			}
			spin_unlock_irqrestore(&sched_lock, sf);
			return;
		}
		if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN
		    || sig == SIGTTOU) {
			/* Job control stop. */
			uint64_t sf = spin_lock_irqsave(&sched_lock);
			cur->state = PROC_STOPPED;
			cur->exit_code = sig;
			cur->reported_stopped = 0;
			/* Send SIGCHLD to parent. */
			for (int i = 0; i < MAX_PROCS; i++) {
				if (procs[i].state != PROC_UNUSED
				    && procs[i].pid == cur->ppid) {
					procs[i].sig_pending |= (1u << SIGCHLD);
					if (procs[i].state == PROC_SLEEPING)
						procs[i].state = PROC_READY;
					break;
				}
			}
			spin_unlock_irqrestore(&sched_lock, sf);
			schedule();
			return;
		}
		/* SIGPIPE, SIGTERM, SIGKILL, etc: kill process. */
		cur->exit_signal = sig;
		do_exit(f, 0);
		return;
	}

	/*
	 * SA_RESTART: if the syscall returned -EINTR and the signal has
	 * SA_RESTART, rewind the saved frame so the syscall re-executes
	 * after the signal handler returns via rt_sigreturn.
	 */
	if (f->rax == SYSCALL_ERR(EINTR) && (cur->sig_flags[sig] & SA_RESTART)) {
		f->rax = f->orig_rax;	/* Restore original syscall number. */
		f->rip -= 2;	/* Rewind to `syscall` instruction (2 bytes) */
	}

	/* User handler: set up signal trampoline frame on user stack. */
	uint64_t old_cr3 = read_cr3();
	uint64_t ucr3 = get_user_cr3();

	sig_frame_info_t info =
	    setup_signal_frame(cur, sig, f->rsp, ucr3, old_cr3);

	/* Write saved syscall frame (under user CR3) */
	if (ucr3)
		write_cr3(ucr3);
	kmemcpy((void *)(uintptr_t) info.saved_frame_addr, f,
		sizeof(syscall_frame_t));
	/* Write checksum after the frame so rt_sigreturn can verify integrity. */
	{
		uint64_t *qw = (uint64_t *) (uintptr_t) info.saved_frame_addr;
		uint64_t cksum = 0xDEAD5164DEAD5164ULL;
		for (int _qi = 0; _qi < (int)(sizeof(syscall_frame_t) / 8);
		     _qi++)
			cksum ^= qw[_qi];
		*(uint64_t *) (uintptr_t) (info.saved_frame_addr +
					   sizeof(syscall_frame_t)) = cksum;
	}
	if (ucr3)
		write_cr3(old_cr3);

	/* Redirect execution to signal handler. */
	f->rip = handler;
	f->rdi = (uint64_t) sig;
	f->rsp = info.sp;
	if (info.siginfo_addr) {
		f->rsi = info.siginfo_addr;
		f->rdx = 0;	/* No ucontext. */
	} else {
		f->rsi = 0;
		f->rdx = 0;
	}
}

/*
 * Deliver a signal from an ISR (hardware exception) context.
 * Synthesizes a syscall_frame_t on the user stack so rt_sigreturn works.
 */
void
signal_deliver_isr(isr_frame_t * frame, int sig, proc_t * cur)
{
	uint64_t handler = cur->sig_handlers[sig];
	if (handler <= SIG_IGN)
		return;

	uint64_t old_cr3 = read_cr3();
	uint64_t ucr3 = cur->pml4_phys;

	sig_frame_info_t info =
	    setup_signal_frame(cur, sig, frame->rsp, ucr3, old_cr3);

	/* Synthesize a syscall_frame_t from isr_frame_t (under user CR3) */
	if (ucr3)
		write_cr3(ucr3);
	syscall_frame_t *saved =
	    (syscall_frame_t *) (uintptr_t) info.saved_frame_addr;
	saved->r15 = frame->r15;
	saved->r14 = frame->r14;
	saved->r13 = frame->r13;
	saved->r12 = frame->r12;
	saved->r11 = frame->r11;
	saved->r10 = frame->r10;
	saved->r9 = frame->r9;
	saved->r8 = frame->r8;
	saved->rsi = frame->rsi;
	saved->rdi = frame->rdi;
	saved->rbp = frame->rbp;
	saved->rdx = frame->rdx;
	saved->rcx = frame->rcx;
	saved->rbx = frame->rbx;
	saved->rax = frame->rax;
	saved->rip = frame->rip;
	saved->rflags = frame->rflags;
	saved->rsp = frame->rsp;
	if (ucr3)
		write_cr3(old_cr3);

	/* Clear pending bit. */
	uint64_t sf = spin_lock_irqsave(&sched_lock);
	cur->sig_pending &= ~(1u << sig);
	spin_unlock_irqrestore(&sched_lock, sf);

	/* Redirect ISR frame to signal handler. */
	frame->rip = handler;
	frame->rdi = (uint64_t) sig;
	frame->rsp = info.sp;
	if (info.siginfo_addr) {
		frame->rsi = info.siginfo_addr;
		frame->rdx = 0;
	} else {
		frame->rsi = 0;
		frame->rdx = 0;
	}
}

uint64_t
syscall_handle_sig(syscall_frame_t * f)
{
	switch (f->rax) {

	case SYS_RT_SIGPROCMASK:{
			int how = (int)f->rdi;
			uint64_t user_set = f->rsi;
			uint64_t user_old_set = f->rdx;

			proc_t *cur = current_proc;
			if (!cur) {
				f->rax = 0;
				return 0;
			}

			USER_ACCESS_BEGIN();

			if (user_old_set) {
				uint64_t *oset =
				    (uint64_t *) (uintptr_t) user_old_set;
				*oset = (uint64_t) cur->sig_mask;
			}

			if (user_set) {
				uint64_t *nset =
				    (uint64_t *) (uintptr_t) user_set;
				uint32_t mask = (uint32_t) * nset;
				switch (how) {
				case SIG_BLOCK:
					cur->sig_mask |= mask;
					break;
				case SIG_UNBLOCK:
					cur->sig_mask &= ~mask;
					break;
				case SIG_SETMASK:
					cur->sig_mask = mask;
					break;
				default:
					USER_ACCESS_END();
					f->rax = SYSCALL_ERR(EINVAL);
					return f->rax;
				}
				cur->sig_mask &= ~(1u << SIGKILL);
			}

			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}

	case SYS_SIGACTION:{
			int signum = (int)f->rdi;
			uint64_t user_new_act = f->rsi;
			uint64_t user_old_act = f->rdx;

			if (signum < 1 || signum >= NSIG || signum == SIGKILL
			    || signum == SIGSTOP) {
				f->rax = SYSCALL_ERR(EINVAL);
				return f->rax;
			}

			proc_t *cur = current_proc;
			if (!cur) {
				f->rax = 0;
				return 0;
			}

			USER_ACCESS_BEGIN();

			if (user_old_act) {
				/* Struct sigaction: sa_handler(8), sa_flags(8), sa_restorer(8), sa_mask(8) */
				uint64_t *old_act =
				    (uint64_t *) (uintptr_t) user_old_act;
				old_act[0] = cur->sig_handlers[signum];
				old_act[1] = cur->sig_flags[signum];
				old_act[2] = cur->sig_restorer[signum];
				old_act[3] =
				    (uint64_t) cur->sig_sa_mask[signum];
			}

			if (user_new_act) {
				uint64_t *new_act =
				    (uint64_t *) (uintptr_t) user_new_act;
				cur->sig_handlers[signum] = new_act[0];
				cur->sig_flags[signum] = new_act[1];
				cur->sig_restorer[signum] = new_act[2];
				cur->sig_sa_mask[signum] =
				    (uint32_t) new_act[3];
			}

			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}

	case SYS_RT_SIGRETURN:{
			/*
			 * Restore saved frame from user stack.
			 * At sigreturn syscall entry, user RSP points to trampoline code.
			 * Layout above trampoline: [saved_sig_mask(8)][saved_frame(144)][siginfo(SIGINFO_SIZE)?]
			 * So saved_mask is at user_rsp + 16, saved_frame at user_rsp + 24.
			 */
			uint64_t saved_mask_addr = f->rsp + 16;
			uint64_t saved_frame_addr = f->rsp + 24;

			USER_ACCESS_BEGIN();

			/* Restore signal mask. */
			uint32_t saved_mask =
			    (uint32_t) (*(uint64_t *) (uintptr_t)
					saved_mask_addr);
			if (current_proc) {
				current_proc->sig_mask = saved_mask;
				current_proc->sig_mask &= ~(1u << SIGKILL);
			}

			/*
			 * Verify checksum before restoring -- catches corruption
			 * between signal_deliver and rt_sigreturn.
			 */
			{
				uint64_t *qw =
				    (uint64_t *) (uintptr_t) saved_frame_addr;
				uint64_t cksum = 0xDEAD5164DEAD5164ULL;
				for (int _qi = 0;
				     _qi < (int)(sizeof(syscall_frame_t) / 8);
				     _qi++)
					cksum ^= qw[_qi];
				uint64_t stored =
				    *(uint64_t *) (uintptr_t) (saved_frame_addr
							       +
							       sizeof
							       (syscall_frame_t));
				if (cksum != stored) {
					USER_ACCESS_END();
					console_write
					    ("\n!!! SIGRETURN FRAME CORRUPT !!!\n");
					console_write("  pid=");
					print_num(current_proc ? (uint64_t)
						  current_proc->pid : 0);
					console_write(" frame_addr=");
					console_hex64(saved_frame_addr);
					console_write
					    ("\n  frame dump (19 qwords):\n");
					for (int _di = 0; _di < 19; _di++) {
						console_write("    [");
						print_num((uint64_t) _di);
						console_write("] ");
						console_hex64(qw[_di]);
						console_write("\n");
					}
					console_write("  expected_cksum=");
					console_hex64(cksum);
					console_write(" stored_cksum=");
					console_hex64(stored);
					console_write("\n");
					/* Also dump the PTE for the frame page. */
					{
						uint64_t ucr3 =
						    current_proc ?
						    current_proc->pml4_phys : 0;
						if (ucr3) {
							uint64_t *fpte =
							    paging_walk_pte
							    (ucr3,
							     saved_frame_addr);
							console_write
							    ("  PTE(frame)=");
							if (fpte)
								console_hex64
								    (*fpte);
							else
								console_write
								    ("NO-PTE");
							uint16_t frc =
							    fpte ?
							    pmm_page_refcount
							    (*fpte &
							     0x000FFFFFFFFFF000ULL)
							    : 0;
							console_write(" rc=");
							console_hex64((uint64_t)
								      frc);
						}
					}
					console_write("\n");
					serial_flush();
					f->rax = SYSCALL_ERR(EINVAL);
					return f->rax;
				}
			}

			/* Restore frame. */
			kmemcpy(f, (void *)(uintptr_t) saved_frame_addr,
				sizeof(syscall_frame_t));

			USER_ACCESS_END();

			/*
			 * The saved frame may come from a timer ISR where
			 * R11/RCX had real user values.  sysretq would
			 * clobber them (RCX←RIP, R11←RFLAGS).  Force the
			 * return path through iretq which restores all regs.
			 */
			{
				extern void syscall_force_iretq(void);
				syscall_force_iretq();
			}

			return f->rax;
		}

	case SYS_KILL:{
			int kill_pid = (int)(int64_t) f->rdi;
			int sig = (int)f->rsi;
			if (sig < 0 || sig >= NSIG) {
				f->rax = SYSCALL_ERR(EINVAL);
				return f->rax;
			}
			if (kill_pid > 0) {
				int ret = do_kill_pid(kill_pid, sig);
				if (ret < 0) {
					f->rax = (uint64_t) ret;
					return f->rax;
				}
			} else if (kill_pid == 0) {
				int mypgid =
				    current_proc ? current_proc->pgid : 0;
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				for (int i = 0; i < MAX_PROCS; i++) {
					if (procs[i].state != PROC_UNUSED
					    && procs[i].pgid == mypgid
					    && sig != 0) {
						procs[i].sig_pending |=
						    (1u << sig);
						if (sig == SIGCONT
						    && procs[i].state ==
						    PROC_STOPPED) {
							procs[i].state =
							    PROC_READY;
							procs[i].
							    reported_stopped =
							    0;
						}
						if (procs[i].state ==
						    PROC_SLEEPING)
							procs[i].state =
							    PROC_READY;
					}
				}
				spin_unlock_irqrestore(&sched_lock, sf);
			} else if (kill_pid == -1) {
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				for (int i = 0; i < MAX_PROCS; i++) {
					if (procs[i].state != PROC_UNUSED
					    && procs[i].pid > 1 && sig != 0) {
						procs[i].sig_pending |=
						    (1u << sig);
						if (sig == SIGCONT
						    && procs[i].state ==
						    PROC_STOPPED) {
							procs[i].state =
							    PROC_READY;
							procs[i].
							    reported_stopped =
							    0;
						}
						if (procs[i].state ==
						    PROC_SLEEPING)
							procs[i].state =
							    PROC_READY;
					}
				}
				spin_unlock_irqrestore(&sched_lock, sf);
			} else {
				int tgt_pgid = -kill_pid;
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				for (int i = 0; i < MAX_PROCS; i++) {
					if (procs[i].state != PROC_UNUSED
					    && procs[i].pgid == tgt_pgid
					    && sig != 0) {
						procs[i].sig_pending |=
						    (1u << sig);
						if (sig == SIGCONT
						    && procs[i].state ==
						    PROC_STOPPED) {
							procs[i].state =
							    PROC_READY;
							procs[i].
							    reported_stopped =
							    0;
						}
						if (procs[i].state ==
						    PROC_SLEEPING)
							procs[i].state =
							    PROC_READY;
					}
				}
				spin_unlock_irqrestore(&sched_lock, sf);
			}
			f->rax = 0;
			return 0;
		}

	case SYS_TKILL:{
			int tid = (int)f->rdi;
			int sig = (int)f->rsi;
			int ret = do_kill_pid(tid, sig);
			f->rax = (ret < 0) ? (uint64_t) ret : 0;
			return f->rax;
		}

	case SYS_TGKILL:{
			int tid = (int)f->rsi;
			int sig = (int)f->rdx;
			int ret = do_kill_pid(tid, sig);
			f->rax = (ret < 0) ? (uint64_t) ret : 0;
			return f->rax;
		}

	case SYS_RT_SIGSUSPEND:{
			uint64_t user_mask = f->rdi;
			proc_t *cur = current_proc;
			if (!cur) {
				f->rax = SYSCALL_ERR(EINTR);
				return f->rax;
			}
			uint32_t old_mask = cur->sig_mask;
			USER_ACCESS_BEGIN();
			uint32_t tmp_mask =
			    user_mask ? (uint32_t) (*(uint64_t *) (uintptr_t)
						    user_mask) : 0;
			USER_ACCESS_END();
			tmp_mask &= ~(1u << SIGKILL);
			cur->sig_mask = tmp_mask;
			for (;;) {
				if (cur->alarm_tick
				    && kernel_ticks >= cur->alarm_tick) {
					cur->sig_pending |= (1u << SIGALRM);
					if (cur->itimer_interval_ticks > 0)
						cur->alarm_tick =
						    kernel_ticks +
						    cur->itimer_interval_ticks;
					else
						cur->alarm_tick = 0;
				}
				uint32_t pend =
				    cur->sig_pending & ~cur->sig_mask;
				if (pend)
					break;
				sched_sleep(0);
			}
			cur->sig_mask = old_mask;
			f->rax = SYSCALL_ERR(EINTR);
			return f->rax;
		}

	case SYS_SIGALTSTACK:
		f->rax = 0;
		return 0;

	case SYS_RT_SIGPENDING:{
			uint64_t user_set = f->rdi;
			if (user_set && current_proc) {
				USER_ACCESS_BEGIN();
				*(uint64_t *) (uintptr_t) user_set =
				    (uint64_t) (current_proc->
						sig_pending & current_proc->
						sig_mask);
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 0;
		}

	default:
		f->rax = SYSCALL_ERR(ENOSYS);
		return f->rax;
	}
}
