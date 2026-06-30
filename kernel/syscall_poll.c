/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

#define POLLIN     0x01
#define POLLPRI    0x02
#define POLLOUT    0x04
#define POLLERR    0x08
#define POLLHUP    0x10
#define POLLNVAL   0x20

#define FD_SET_MAX   1024	/* Max fds for select/pselect. */
#define FD_SET_BYTES 128	/* sizeof(fd_set) = FD_SET_MAX / 8. */

#define NSEC_PER_SEC 1000000000ULL

static uint64_t
poll_impl(syscall_frame_t * f)
{
	uint64_t user_fds = f->rdi;
	uint32_t nfds = (uint32_t) f->rsi;

	/* SYS_PPOLL uses timespec from rdx; SYS_POLL uses int timeout from rdx. */
	int is_ppoll = (f->rax == SYS_PPOLL);

	/* Handle nfds == 0 early. */
	if (nfds == 0) {
		if (is_ppoll) {
			uint64_t user_tmo = f->rdx;
			if (user_tmo) {
				USER_ACCESS_BEGIN();
				uint64_t *ts =
				    (uint64_t *) (uintptr_t) user_tmo;
				uint64_t sec = ts[0], nsec = ts[1];
				USER_ACCESS_END();
				if (sec > 0 || nsec > 0)
					sched_sleep(SCHED_TICK_CHAN);
			}
		} else {
			int timeout = (int)(int64_t) f->rdx;
			if (timeout > 0)
				sched_sleep(SCHED_TICK_CHAN);
		}
		f->rax = 0;
		return 1;
	}

	/* Compute deadline. */
	uint64_t deadline = 0;
	int timeout_mode = 0;	/* 0=Non-blocking, 1=timed, 2=infinite. */

	if (is_ppoll) {
		uint64_t user_tmo = f->rdx;
		if (!user_tmo) {
			deadline = (uint64_t) - 1;
			timeout_mode = 2;
		} else {
			USER_ACCESS_BEGIN();
			uint64_t *ts = (uint64_t *) (uintptr_t) user_tmo;
			uint64_t sec = ts[0], nsec = ts[1];
			USER_ACCESS_END();
			if (sec > 0 || nsec > 0) {
				deadline =
				    kernel_ticks + sec * KERNEL_HZ +
				    nsec / (NSEC_PER_SEC / KERNEL_HZ);
				timeout_mode = 1;
			}
		}
	} else {
		int timeout = (int)(int64_t) f->rdx;
		if (timeout > 0) {
			deadline =
			    kernel_ticks + (uint64_t) timeout *KERNEL_HZ / 1000;
			timeout_mode = 1;
		} else if (timeout < 0) {
			deadline = (uint64_t) - 1;
			timeout_mode = 2;
		}
	}

	for (;;) {
		USER_ACCESS_BEGIN();
		int ready = 0;
		for (uint32_t i = 0; i < nfds; i++) {
			uint8_t *pfd =
			    (uint8_t *) (uintptr_t) (user_fds + i * 8);
			int pfd_fd = *(int *)pfd;
			uint16_t events = *(uint16_t *) (pfd + 4);
			uint16_t revents = 0;
			fd_entry_t *entry = fd_get(pfd_fd);
			if (!entry) {
				revents = POLLNVAL;
			} else {
				if (entry->desc->type == FD_TYPE_CONSOLE) {
					if (events & POLLOUT)
						revents |= POLLOUT;
					if ((events & POLLIN)
					    && serial_data_ready())
						revents |= POLLIN;
				} else if (entry->desc->type == FD_TYPE_FILE ||
					   entry->desc->type == FD_TYPE_DEVNULL
					   || entry->desc->type ==
					   FD_TYPE_DEVZERO
					   || entry->desc->type ==
					   FD_TYPE_DEVRANDOM
					   || entry->desc->type ==
					   FD_TYPE_DIR) {
					revents = events & (POLLIN | POLLOUT);
				} else if (entry->desc->type ==
					   FD_TYPE_PIPE_READ
					   && entry->desc->pipe) {
					pipe_t *pp =
					    (pipe_t *) entry->desc->pipe;
					if (pp->writers == 0)
						revents |= POLLHUP;
					if (pp->count > 0)
						revents |= (events & POLLIN);
				} else if (entry->desc->type ==
					   FD_TYPE_PIPE_WRITE
					   && entry->desc->pipe) {
					pipe_t *pp =
					    (pipe_t *) entry->desc->pipe;
					if (pp->readers == 0)
						revents |= POLLHUP;
					if (pp->count < PIPE_BUF_SIZE)
						revents |= (events & POLLOUT);
				} else if (entry->desc->type == FD_TYPE_EPOLL) {
					revents = events & (POLLIN | POLLOUT);
				}
			}
			*(uint16_t *) (pfd + 6) = revents;
			if (revents)
				ready++;
		}
		USER_ACCESS_END();
		if (ready > 0 || timeout_mode == 0) {
			f->rax = (uint64_t) ready;
			return 1;
		}
		if (timeout_mode == 1 && kernel_ticks >= deadline) {
			f->rax = is_ppoll ? 0 : (uint64_t) ready;
			return 1;
		}
		/* Check for pending signals. */
		if (current_proc
		    && (current_proc->sig_pending & ~current_proc->sig_mask)) {
			f->rax = SYSCALL_ERR(EINTR);
			return 1;
		}
		sched_sleep(SCHED_TICK_CHAN);
	}
}

static uint64_t
select_impl(syscall_frame_t * f)
{
	int nfds = (int)f->rdi;
	uint64_t user_readfds = f->rsi;
	uint64_t user_writefds = f->rdx;
	uint64_t user_exceptfds = f->r10;
	uint64_t user_tmo = f->r8;
	int is_pselect = (f->rax == SYS_PSELECT6);

	if (nfds < 0)
		nfds = 0;
	if (nfds > FD_SET_MAX)
		nfds = FD_SET_MAX;

	/* fd_set is FD_SET_BYTES bytes = FD_SET_MAX bits. */
	uint8_t readfds[FD_SET_BYTES], writefds[FD_SET_BYTES],
	    exceptfds[FD_SET_BYTES];
	int fdset_bytes = (nfds + 7) / 8;

	/* Zero input sets. */
	for (int i = 0; i < FD_SET_BYTES; i++)
		readfds[i] = writefds[i] = exceptfds[i] = 0;

	/* Copy fd_sets from user space. */
	{
		USER_ACCESS_BEGIN();
		if (user_readfds) {
			kmemcpy(readfds, (void *)(uintptr_t) user_readfds,
				fdset_bytes);
		}
		if (user_writefds) {
			kmemcpy(writefds, (void *)(uintptr_t) user_writefds,
				fdset_bytes);
		}
		if (user_exceptfds) {
			kmemcpy(exceptfds, (void *)(uintptr_t) user_exceptfds,
				fdset_bytes);
		}
		USER_ACCESS_END();
	}

	/* Compute deadline from timeout/timespec. */
	uint64_t deadline = 0;
	int is_blocking = 0;
	if (!user_tmo) {
		/* NULL timeout = block indefinitely. */
		deadline = (uint64_t) - 1;
		is_blocking = 1;
	} else {
		USER_ACCESS_BEGIN();
		uint64_t *tv = (uint64_t *) (uintptr_t) user_tmo;
		uint64_t tv_0 = tv[0];
		uint64_t tv_1 = tv[1];
		USER_ACCESS_END();
		if (tv_0 > 0 || tv_1 > 0) {
			if (is_pselect)
				deadline =
				    kernel_ticks + tv_0 * KERNEL_HZ +
				    tv_1 / (NSEC_PER_SEC / KERNEL_HZ);
			else
				deadline =
				    kernel_ticks + tv_0 * KERNEL_HZ +
				    tv_1 * KERNEL_HZ / 1000000;
			is_blocking = 1;
		}
	}

	for (;;) {
		uint8_t res_read[FD_SET_BYTES], res_write[FD_SET_BYTES],
		    res_except[FD_SET_BYTES];
		for (int i = 0; i < FD_SET_BYTES; i++)
			res_read[i] = res_write[i] = res_except[i] = 0;

		int ready = 0;
		for (int fd = 0; fd < nfds; fd++) {
			int byte = fd / 8;
			uint8_t bit = (uint8_t) (1 << (fd % 8));
			int in_read = (readfds[byte] & bit) != 0;
			int in_write = (writefds[byte] & bit) != 0;
			if (!in_read && !in_write)
				continue;
			fd_entry_t *entry = fd_get(fd);
			if (!entry)
				continue;
			if (in_read) {
				int is_ready = 0;
				if (entry->desc->type == FD_TYPE_CONSOLE) {
					if (serial_data_ready())
						is_ready = 1;
				} else if (entry->desc->type == FD_TYPE_FILE ||
					   entry->desc->type == FD_TYPE_DEVNULL
					   || entry->desc->type ==
					   FD_TYPE_DEVZERO
					   || entry->desc->type ==
					   FD_TYPE_DEVRANDOM
					   || entry->desc->type ==
					   FD_TYPE_DIR) {
					is_ready = 1;
				} else if (entry->desc->type ==
					   FD_TYPE_PIPE_READ
					   && entry->desc->pipe) {
					pipe_t *pp =
					    (pipe_t *) entry->desc->pipe;
					if (pp->count > 0 || pp->writers == 0)
						is_ready = 1;
				}
				if (is_ready) {
					res_read[byte] |= bit;
					ready++;
				}
			}
			if (in_write) {
				int is_ready = 0;
				if (entry->desc->type == FD_TYPE_FILE
				    || entry->desc->type == FD_TYPE_CONSOLE
				    || entry->desc->type == FD_TYPE_DEVNULL
				    || entry->desc->type == FD_TYPE_DEVZERO
				    || entry->desc->type == FD_TYPE_DEVRANDOM
				    || entry->desc->type == FD_TYPE_DIR) {
					is_ready = 1;
				} else if (entry->desc->type ==
					   FD_TYPE_PIPE_WRITE
					   && entry->desc->pipe) {
					pipe_t *pp =
					    (pipe_t *) entry->desc->pipe;
					if (pp->count < PIPE_BUF_SIZE
					    || pp->readers == 0)
						is_ready = 1;
				}
				if (is_ready) {
					res_write[byte] |= bit;
					ready++;
				}
			}
		}

		if (ready > 0 || !is_blocking) {
			/* Copy result sets back to user. */
			USER_ACCESS_BEGIN();
			if (user_readfds) {
				kmemcpy((void *)(uintptr_t) user_readfds,
					res_read, fdset_bytes);
			}
			if (user_writefds) {
				kmemcpy((void *)(uintptr_t) user_writefds,
					res_write, fdset_bytes);
			}
			if (user_exceptfds) {
				kmemcpy((void *)(uintptr_t) user_exceptfds,
					res_except, fdset_bytes);
			}
			USER_ACCESS_END();
			f->rax = (uint64_t) ready;
			return 1;
		}

		if (deadline != (uint64_t) - 1 && kernel_ticks >= deadline) {
			/* Timeout expired, write zeroed result sets. */
			USER_ACCESS_BEGIN();
			if (user_readfds) {
				kmemzero((void *)(uintptr_t) user_readfds,
					 fdset_bytes);
			}
			if (user_writefds) {
				kmemzero((void *)(uintptr_t) user_writefds,
					 fdset_bytes);
			}
			if (user_exceptfds) {
				kmemzero((void *)(uintptr_t) user_exceptfds,
					 fdset_bytes);
			}
			USER_ACCESS_END();
			f->rax = 0;
			return 1;
		}

		/* Check for pending signals. */
		if (current_proc
		    && (current_proc->sig_pending & ~current_proc->sig_mask)) {
			f->rax = SYSCALL_ERR(EINTR);
			return 1;
		}
		sched_sleep(SCHED_TICK_CHAN);
	}
}

static uint64_t
epoll_stub(syscall_frame_t * f)
{
	switch (f->rax) {
	case SYS_EPOLL_CREATE:
	case SYS_EPOLL_CREATE1:{
			int nfd = fd_alloc();
			if (nfd < 0) {
				f->rax = SYSCALL_ERR(EMFILE);
				return 1;
			}
			file_desc_t *d = file_desc_alloc();
			if (!d) {
				f->rax = SYSCALL_ERR(ENOMEM);
				return 1;
			}
			d->type = FD_TYPE_EPOLL;
			d->flags = 0;
			fd_entry_t *entry = fd_get_raw(nfd);
			entry->desc = d;
			entry->fd_flags = (f->rax == SYS_EPOLL_CREATE1
					   && (f->
					       rdi & O_CLOEXEC)) ? FD_CLOEXEC :
			    0;
			f->rax = (uint64_t) nfd;
			return 1;
		}

	case SYS_EPOLL_CTL:
		f->rax = 0;
		return 1;

	case SYS_EPOLL_WAIT:
	case SYS_EPOLL_PWAIT:{
			int timeout = (int)(int64_t) f->r10;
			if (timeout == 0) {
				f->rax = 0;
				return 1;
			}
			if (timeout > 0) {
				/* Sleep briefly then return 0. */
				sched_sleep(SCHED_TICK_CHAN);
			} else {
				/* Timeout == -1: yield once to avoid infinite hang. */
				sched_sleep(SCHED_TICK_CHAN);
			}
			f->rax = 0;
			return 1;
		}

	default:
		return 0;
	}
}

int
syscall_handle_poll(syscall_frame_t * f)
{
	switch (f->rax) {
	case SYS_POLL:
	case SYS_PPOLL:
		return poll_impl(f);

	case SYS_SELECT:
	case SYS_PSELECT6:
		return select_impl(f);

	case SYS_EPOLL_CREATE:
	case SYS_EPOLL_CREATE1:
	case SYS_EPOLL_CTL:
	case SYS_EPOLL_WAIT:
	case SYS_EPOLL_PWAIT:
		return epoll_stub(f);

	default:
		return 0;
	}
}
