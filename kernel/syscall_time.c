/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

#define NSEC_PER_SEC 1000000000ULL

int
syscall_handle_time(syscall_frame_t * f)
{
	switch (f->rax) {

	case SYS_CLOCK_GETTIME:{
			uint64_t user_tp = f->rsi;
			if (user_tp) {
				uint64_t ticks = kernel_ticks;
				/*
				 * Use TSC for sub-tick nanosecond resolution.
				 * Without TSC calibration we can't convert to real ns,
				 * but the low bits provide uniqueness for mkstemp etc.
				 */
				uint32_t tsc_lo, tsc_hi;
				__asm__ __volatile__("rdtsc":"=a"(tsc_lo),
						     "=d"(tsc_hi));
				USER_ACCESS_BEGIN();
				uint64_t *tp = (uint64_t *) (uintptr_t) user_tp;
				tp[0] = EPOCH_BASE + ticks / KERNEL_HZ;
				tp[1] =
				    (ticks % KERNEL_HZ) * (NSEC_PER_SEC /
							   KERNEL_HZ) +
				    (tsc_lo & 0xFFFF);
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 1;
		}

	case SYS_GETTIMEOFDAY:{
			uint64_t user_tv = f->rdi;
			if (user_tv) {
				uint64_t ticks = kernel_ticks;
				USER_ACCESS_BEGIN();
				uint64_t *tv = (uint64_t *) (uintptr_t) user_tv;
				tv[0] = EPOCH_BASE + ticks / KERNEL_HZ;
				tv[1] = (ticks % KERNEL_HZ) * 10000ULL;
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 1;
		}

	case SYS_TIME:{
			uint64_t now = kernel_time_sec();
			uint64_t user_tloc = f->rdi;
			if (user_tloc) {
				USER_ACCESS_BEGIN();
				*(uint64_t *) (uintptr_t) user_tloc = now;
				USER_ACCESS_END();
			}
			f->rax = now;
			return 1;
		}

	case SYS_NANOSLEEP:{
			uint64_t user_req = f->rdi;
			uint64_t user_rem = f->rsi;

			/* Read requested time from user struct timespec. */
			uint64_t req_sec = 0, req_nsec = 0;
			{
				USER_ACCESS_BEGIN();
				if (user_req) {
					uint64_t *tp =
					    (uint64_t *) (uintptr_t) user_req;
					req_sec = tp[0];
					req_nsec = tp[1];
				}
				USER_ACCESS_END();
			}

			/* Compute target tick. */
			uint64_t sleep_ticks =
			    req_sec * KERNEL_HZ +
			    req_nsec / (NSEC_PER_SEC / KERNEL_HZ);
			if (sleep_ticks == 0 && (req_sec > 0 || req_nsec > 0))
				sleep_ticks = 1;	/* Minimum 1 tick. */
			uint64_t target = kernel_ticks + sleep_ticks;

			/*
			 * Sleep loop with signal interruption.
			 * Uses sched_sleep (releases BKL via idle stack) so other CPUs
			 * can run. Woken by timer_handler's sched_wakeup(SCHED_TICK_CHAN)
			 * on each tick. Verified: models/nanosleep.pml.
			 */
			while (kernel_ticks < target) {
				if (current_proc) {
					uint32_t pend =
					    current_proc->
					    sig_pending & ~current_proc->
					    sig_mask;
					if (pend) {
						/* Compute remaining time. */
						if (user_rem) {
							uint64_t rem_ticks =
							    (kernel_ticks <
							     target) ? (target -
									kernel_ticks)
							    : 0;
							uint64_t rem_sec =
							    rem_ticks /
							    KERNEL_HZ;
							uint64_t rem_nsec =
							    (rem_ticks %
							     KERNEL_HZ) *
							    (NSEC_PER_SEC /
							     KERNEL_HZ);
							USER_ACCESS_BEGIN();
							uint64_t *rem =
							    (uint64_t
							     *) (uintptr_t)
							    user_rem;
							rem[0] = rem_sec;
							rem[1] = rem_nsec;
							USER_ACCESS_END();
						}
						f->rax = SYSCALL_ERR(EINTR);
						return 1;
					}
				}
				sched_sleep(SCHED_TICK_CHAN);
			}

			/* Write remaining time (always 0 on normal completion) */
			if (user_rem) {
				USER_ACCESS_BEGIN();
				uint64_t *rem =
				    (uint64_t *) (uintptr_t) user_rem;
				rem[0] = 0;
				rem[1] = 0;
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 1;
		}

	case SYS_CLOCK_GETRES:{
			/* clock_getres(clockid, res) -- return timer resolution. */
			uint64_t user_res = f->rsi;
			if (user_res) {
				USER_ACCESS_BEGIN();
				uint64_t *tp =
				    (uint64_t *) (uintptr_t) user_res;
				tp[0] = 0;	/* tv_sec. */
				tp[1] = 1000000000ULL / KERNEL_HZ;	/* tv_nsec = 10ms. */
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 1;
		}

	case SYS_CLOCK_NANOSLEEP:{
			/* clock_nanosleep(clockid, flags, request, remain) */
			int flags = (int)f->rsi;
			uint64_t user_req = f->rdx;
			uint64_t user_rem = f->r10;

			uint64_t req_sec = 0, req_nsec = 0;
			{
				USER_ACCESS_BEGIN();
				if (user_req) {
					uint64_t *tp =
					    (uint64_t *) (uintptr_t) user_req;
					req_sec = tp[0];
					req_nsec = tp[1];
				}
				USER_ACCESS_END();
			}

			uint64_t target;
			if (flags & 1) {
				/* TIMER_ABSTIME: sleep until absolute time. */
				target =
				    req_sec * KERNEL_HZ +
				    req_nsec / (NSEC_PER_SEC / KERNEL_HZ);
				/* Convert from wall time to kernel ticks (approximate) */
				uint64_t now_ticks =
				    kernel_time_sec() * KERNEL_HZ;
				if (target <= now_ticks) {
					f->rax = 0;
					return 1;
				}
				uint64_t sleep_ticks = target - now_ticks;
				target = kernel_ticks + sleep_ticks;
			} else {
				uint64_t sleep_ticks =
				    req_sec * KERNEL_HZ +
				    req_nsec / (NSEC_PER_SEC / KERNEL_HZ);
				if (sleep_ticks == 0
				    && (req_sec > 0 || req_nsec > 0))
					sleep_ticks = 1;
				target = kernel_ticks + sleep_ticks;
			}

			while (kernel_ticks < target) {
				if (current_proc) {
					uint32_t pend =
					    current_proc->
					    sig_pending & ~current_proc->
					    sig_mask;
					if (pend) {
						if (user_rem && !(flags & 1)) {
							uint64_t rem_ticks =
							    (target >
							     kernel_ticks)
							    ? (target -
							       kernel_ticks) :
							    0;
							USER_ACCESS_BEGIN();
							uint64_t *rem =
							    (uint64_t
							     *) (uintptr_t)
							    user_rem;
							rem[0] =
							    rem_ticks /
							    KERNEL_HZ;
							rem[1] =
							    (rem_ticks %
							     KERNEL_HZ) *
							    (NSEC_PER_SEC /
							     KERNEL_HZ);
							USER_ACCESS_END();
						}
						f->rax = SYSCALL_ERR(EINTR);
						return 1;
					}
				}
				sched_sleep(SCHED_TICK_CHAN);
			}
			f->rax = 0;
			return 1;
		}

	case SYS_ALARM:{
			uint32_t seconds = (uint32_t) f->rdi;
			proc_t *cur = current_proc;
			uint64_t old_remaining = 0;
			if (cur) {
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				if (cur->alarm_tick > kernel_ticks)
					old_remaining =
					    (cur->alarm_tick - kernel_ticks +
					     KERNEL_HZ - 1) / KERNEL_HZ;
				if (seconds == 0)
					cur->alarm_tick = 0;
				else
					cur->alarm_tick =
					    kernel_ticks +
					    (uint64_t) seconds *KERNEL_HZ;
				cur->itimer_interval_ticks = 0;
				spin_unlock_irqrestore(&sched_lock, sf);
			}
			f->rax = old_remaining;
			return 1;
		}

	case SYS_SETITIMER:{
			int which = (int)f->rdi;
			uint64_t user_new = f->rsi;
			uint64_t user_old = f->rdx;
			proc_t *cur = current_proc;
			if (which != 0) {	/* Only ITIMER_REAL (0) supported. */
				/* Return zeroed old value for unsupported timers. */
				if (user_old) {
					USER_ACCESS_BEGIN();
					uint64_t *p =
					    (uint64_t *) (uintptr_t) user_old;
					p[0] = 0;
					p[1] = 0;
					p[2] = 0;
					p[3] = 0;
					USER_ACCESS_END();
				}
				f->rax = 0;
				return 1;
			}
			/* Save old value. */
			if (user_old && cur) {
				uint64_t remaining_sec = 0, remaining_usec = 0;
				uint64_t int_sec = 0, int_usec = 0;
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				if (cur->alarm_tick > kernel_ticks) {
					uint64_t rem_ticks =
					    cur->alarm_tick - kernel_ticks;
					remaining_sec = rem_ticks / KERNEL_HZ;
					remaining_usec =
					    (rem_ticks % KERNEL_HZ) * (1000000 /
								       KERNEL_HZ);
				}
				if (cur->itimer_interval_ticks > 0) {
					int_sec =
					    cur->itimer_interval_ticks /
					    KERNEL_HZ;
					int_usec =
					    (cur->itimer_interval_ticks %
					     KERNEL_HZ) * (1000000 / KERNEL_HZ);
				}
				spin_unlock_irqrestore(&sched_lock, sf);
				USER_ACCESS_BEGIN();
				uint64_t *p = (uint64_t *) (uintptr_t) user_old;
				p[0] = int_sec;
				p[1] = int_usec;
				p[2] = remaining_sec;
				p[3] = remaining_usec;
				USER_ACCESS_END();
			}
			/* Set new value. */
			if (user_new && cur) {
				USER_ACCESS_BEGIN();
				uint64_t *p = (uint64_t *) (uintptr_t) user_new;
				uint64_t int_sec = p[0];
				uint64_t int_usec = p[1];
				uint64_t val_sec = p[2];
				uint64_t val_usec = p[3];
				USER_ACCESS_END();
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				if (int_sec == 0 && int_usec == 0)
					cur->itimer_interval_ticks = 0;
				else
					cur->itimer_interval_ticks =
					    int_sec * KERNEL_HZ +
					    int_usec * KERNEL_HZ / 1000000;
				if (val_sec == 0 && val_usec == 0)
					cur->alarm_tick = 0;
				else
					cur->alarm_tick =
					    kernel_ticks + val_sec * KERNEL_HZ +
					    val_usec * KERNEL_HZ / 1000000;
				spin_unlock_irqrestore(&sched_lock, sf);
			}
			f->rax = 0;
			return 1;
		}

	case SYS_GETITIMER:{
			int which = (int)f->rdi;
			uint64_t user_val = f->rsi;
			if (user_val) {
				uint64_t remaining_sec = 0, remaining_usec = 0;
				uint64_t int_sec = 0, int_usec = 0;
				proc_t *cur = current_proc;
				if (which == 0 && cur) {
					uint64_t sf =
					    spin_lock_irqsave(&sched_lock);
					if (cur->alarm_tick > kernel_ticks) {
						uint64_t rem_ticks =
						    cur->alarm_tick -
						    kernel_ticks;
						remaining_sec =
						    rem_ticks / KERNEL_HZ;
						remaining_usec =
						    (rem_ticks % KERNEL_HZ) *
						    (1000000 / KERNEL_HZ);
					}
					if (cur->itimer_interval_ticks > 0) {
						int_sec =
						    cur->itimer_interval_ticks /
						    KERNEL_HZ;
						int_usec =
						    (cur->
						     itimer_interval_ticks %
						     KERNEL_HZ) * (1000000 /
								   KERNEL_HZ);
					}
					spin_unlock_irqrestore(&sched_lock, sf);
				}
				USER_ACCESS_BEGIN();
				uint64_t *p = (uint64_t *) (uintptr_t) user_val;
				p[0] = int_sec;
				p[1] = int_usec;
				p[2] = remaining_sec;
				p[3] = remaining_usec;
				USER_ACCESS_END();
			}
			f->rax = 0;
			return 1;
		}

	case SYS_PAUSE:{
			for (;;) {
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				if (current_proc) {
					uint32_t pend =
					    current_proc->
					    sig_pending & ~current_proc->
					    sig_mask;
					if (pend) {
						spin_unlock_irqrestore
						    (&sched_lock, sf);
						f->rax = SYSCALL_ERR(EINTR);
						return 1;
					}
					current_proc->state = PROC_SLEEPING;
					current_proc->wait_chan = 0;
				}
				spin_unlock_irqrestore(&sched_lock, sf);
				schedule();
			}
		}

	case SYS_SCHED_YIELD:
		sched_yield();
		f->rax = 0;
		return 1;

	case SYS_TIMES:{
			uint64_t user_buf = f->rdi;
			if (user_buf) {
				USER_ACCESS_BEGIN();
				uint8_t *p = (uint8_t *) (uintptr_t) user_buf;
				kmemzero(p, 32);
				USER_ACCESS_END();
			}
			f->rax = (uint64_t) kernel_ticks;
			return 1;
		}

	case SYS_SETTIMEOFDAY:
		/* Accept silently -- we don't adjust kernel clock. */
		f->rax = 0;
		return 1;

	default:
		return 0;
	}
}
