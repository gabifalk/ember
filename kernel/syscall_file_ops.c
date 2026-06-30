/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

uint64_t
sys_lseek(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	int64_t offset = (int64_t) f->rsi;
	int whence = (int)f->rdx;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	/* Pipes and consoles are not seekable. */
	if (entry->desc->type == FD_TYPE_PIPE_READ ||
	    entry->desc->type == FD_TYPE_PIPE_WRITE ||
	    entry->desc->type == FD_TYPE_CONSOLE ||
	    entry->desc->type == FD_TYPE_DEVRANDOM) {
		f->rax = SYSCALL_ERR(ESPIPE);
		return f->rax;
	}
	if (entry->desc->type == FD_TYPE_PROC_IOMEM) {
		f->rax = SYSCALL_ERR(ESPIPE);
		return f->rax;
	}
	spin_lock(&entry->desc->lock);
	int64_t new_off;
	uint64_t file_size = entry->desc->node ? entry->desc->node->size : 0;
	switch (whence) {
	case SEEK_SET:
		new_off = offset;
		break;
	case SEEK_CUR:
		new_off = (int64_t) entry->desc->offset + offset;
		break;
	case SEEK_END:
		new_off = (int64_t) file_size + offset;
		break;
	default:
		spin_unlock(&entry->desc->lock);
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}
	if (new_off < 0)
		new_off = 0;
	entry->desc->offset = (uint64_t) new_off;
	spin_unlock(&entry->desc->lock);
	f->rax = (uint64_t) new_off;
	return f->rax;
}

uint64_t
sys_ioctl(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	uint64_t request = f->rsi;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	if (entry->desc->type == FD_TYPE_CONSOLE) {
		if (request == TCGETS) {
			USER_ACCESS_BEGIN();
			uint32_t *tp = (uint32_t *) (uintptr_t) f->rdx;
			/*
			 * Kernel termios: c_iflag, c_oflag, c_cflag, c_lflag (4x uint32_t)
			 * + c_line(1) + c_cc[19] + ispeed(4) + ospeed(4) = 44 bytes.
			 */
			uint8_t *bp = (uint8_t *) tp;
			kmemzero(bp, 44);
			uint32_t lflag = 0;
			if (console_echo)
				lflag |= TERMIOS_ECHO;
			if (console_icanon)
				lflag |= TERMIOS_ICANON;
			tp[3] = lflag;
			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}
		if (request == TIOCGWINSZ) {
			USER_ACCESS_BEGIN();
			uint16_t *ws = (uint16_t *) (uintptr_t) f->rdx;
			ws[0] = TERM_ROWS;
			ws[1] = TERM_COLS;
			ws[2] = 0;	/* ws_xpixel. */
			ws[3] = 0;	/* ws_ypixel. */
			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}
		if (request == TIOCGPGRP) {
			USER_ACCESS_BEGIN();
			int fg = console_fg_pgid;
			*(int *)(uintptr_t) f->rdx =
			    fg >
			    0 ? fg : (current_proc ? current_proc->pgid : 1);
			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}
		if (request == TIOCSPGRP) {
			USER_ACCESS_BEGIN();
			int pgid = *(int *)(uintptr_t) f->rdx;
			USER_ACCESS_END();
			console_fg_pgid = pgid;
			f->rax = 0;
			return 0;
		}
		if (request == TCSETS || request == TCSETSW
		    || request == TCSETSF) {
			USER_ACCESS_BEGIN();
			uint32_t *tp = (uint32_t *) (uintptr_t) f->rdx;
			uint32_t lflag = tp[3];
			console_echo = (lflag & TERMIOS_ECHO) ? 1 : 0;
			console_icanon = (lflag & TERMIOS_ICANON) ? 1 : 0;
			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}
		if (request == FIONREAD) {
			USER_ACCESS_BEGIN();
			*(int *)(uintptr_t) f->rdx = serial_data_ready()? 1 : 0;
			USER_ACCESS_END();
			f->rax = 0;
			return 0;
		}
	}
	if (entry->desc->type == FD_TYPE_PIPE_READ && request == FIONREAD) {
		int avail = 0;
		if (entry->desc->pipe) {
			pipe_t *pp = (pipe_t *) entry->desc->pipe;
			avail = (int)pp->count;
		}
		USER_ACCESS_BEGIN();
		*(int *)(uintptr_t) f->rdx = avail;
		USER_ACCESS_END();
		f->rax = 0;
		return 0;
	}
	f->rax = SYSCALL_ERR(ENOTTY);
	return f->rax;
}

uint64_t
sys_fcntl(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	int cmd = (int)f->rsi;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	switch (cmd) {
	case F_GETFD:
		f->rax = (entry->fd_flags & FD_CLOEXEC) ? 1 : 0;
		return f->rax;
	case F_SETFD:
		if (f->rdx & 1)
			entry->fd_flags |= FD_CLOEXEC;
		else
			entry->fd_flags &= ~(uint32_t) FD_CLOEXEC;
		f->rax = 0;
		return 0;
	case F_GETFL:
		spin_lock(&entry->desc->lock);
		f->rax =
		    (uint64_t) (entry->desc->
				flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND
					 | O_NONBLOCK));
		spin_unlock(&entry->desc->lock);
		return f->rax;
	case F_SETFL:
		/* Only allow setting O_APPEND and O_NONBLOCK. */
		spin_lock(&entry->desc->lock);
		entry->desc->flags =
		    (entry->desc->flags & ~(uint32_t) (O_APPEND | O_NONBLOCK))
		    | ((uint32_t) f->rdx & (O_APPEND | O_NONBLOCK));
		spin_unlock(&entry->desc->lock);
		f->rax = 0;
		return 0;
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:{
			int minfd = (int)f->rdx;
			int newfd = -1;
			for (int i = minfd; i < MAX_FDS; i++) {
				fd_entry_t *e = fd_get_raw(i);
				if (e && e->desc == 0) {
					newfd = i;
					break;
				}
			}
			if (newfd < 0) {
				f->rax = SYSCALL_ERR(EMFILE);
				return f->rax;
			}
			fd_entry_t *ne = fd_get_raw(newfd);
			ne->desc = entry->desc;
			file_desc_ref(entry->desc);
			ne->fd_flags =
			    (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
			f->rax = (uint64_t) newfd;
			return f->rax;
		}
	default:
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}
}

uint64_t
sys_dup(syscall_frame_t * f)
{
	int oldfd = (int)f->rdi;
	fd_entry_t *old_entry = syscall_fd_get(oldfd, f);
	if (!old_entry)
		return f->rax;
	int newfd = fd_alloc();
	if (newfd < 0) {
		f->rax = SYSCALL_ERR(EMFILE);
		return f->rax;
	}
	fd_entry_t *ne = fd_get_raw(newfd);
	ne->desc = old_entry->desc;
	file_desc_ref(old_entry->desc);
	ne->fd_flags = 0;
	f->rax = (uint64_t) newfd;
	return f->rax;
}
