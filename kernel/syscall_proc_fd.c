/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

/* ---- Pipe2 ---- */
uint64_t
do_pipe2(syscall_frame_t * f)
{
	uint64_t user_pipefd = f->rdi;
	uint32_t flags = (uint32_t) f->rsi;

	pipe_t *p = pipe_create();
	if (!p) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	int rfd = fd_alloc();
	if (rfd < 0) {
		kfree(p);
		f->rax = SYSCALL_ERR(EMFILE);
		return f->rax;
	}
	file_desc_t *rd = file_desc_alloc();
	if (!rd) {
		kfree(p);
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}
	rd->type = FD_TYPE_PIPE_READ;
	rd->flags = (flags & ~(uint32_t) O_CLOEXEC) | O_RDONLY;
	rd->pipe = (struct pipe *)p;
	fd_entry_t *re = fd_get_raw(rfd);
	re->desc = rd;
	re->fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

	int wfd = fd_alloc();
	if (wfd < 0) {
		re->desc = 0;
		file_desc_unref(rd);
		kfree(p);
		f->rax = SYSCALL_ERR(EMFILE);
		return f->rax;
	}
	file_desc_t *wd = file_desc_alloc();
	if (!wd) {
		re->desc = 0;
		file_desc_unref(rd);
		kfree(p);
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}
	wd->type = FD_TYPE_PIPE_WRITE;
	wd->flags = (flags & ~(uint32_t) O_CLOEXEC) | O_WRONLY;
	wd->pipe = (struct pipe *)p;
	fd_entry_t *we = fd_get_raw(wfd);
	we->desc = wd;
	we->fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

	/* Write [rfd, wfd] to user space. */
	USER_ACCESS_BEGIN();
	int *upipe = (int *)(uintptr_t) user_pipefd;
	upipe[0] = rfd;
	upipe[1] = wfd;
	USER_ACCESS_END();

	f->rax = 0;
	return 0;
}

/* ---- Dup2 ---- */
uint64_t
do_dup2(syscall_frame_t * f)
{
	int oldfd = (int)f->rdi;
	int newfd = (int)f->rsi;

	fd_entry_t *old_entry = syscall_fd_get(oldfd, f);
	if (!old_entry)
		return f->rax;

	if (oldfd == newfd) {
		f->rax = (uint64_t) newfd;
		return f->rax;
	}

	if (newfd < 0 || newfd >= MAX_FDS) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}

	/* Close newfd if open. */
	fd_entry_t *new_entry = fd_get_raw(newfd);
	if (new_entry && new_entry->desc) {
		file_desc_unref(new_entry->desc);
	}

	/* Share the file description. */
	new_entry->desc = old_entry->desc;
	file_desc_ref(old_entry->desc);
	new_entry->fd_flags = 0;	/* Dup2 clears FD_CLOEXEC. */

	f->rax = (uint64_t) newfd;
	return f->rax;
}

/* ---- Dup3 ---- */
uint64_t
do_dup3(syscall_frame_t * f)
{
	int oldfd = (int)f->rdi;
	int newfd = (int)f->rsi;
	int flags = (int)f->rdx;

	if (oldfd == newfd) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}

	fd_entry_t *old_entry = syscall_fd_get(oldfd, f);
	if (!old_entry)
		return f->rax;

	if (newfd < 0 || newfd >= MAX_FDS) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}

	/* Close newfd if open. */
	fd_entry_t *new_entry = fd_get_raw(newfd);
	if (new_entry && new_entry->desc) {
		file_desc_unref(new_entry->desc);
	}

	/* Share the file description. */
	new_entry->desc = old_entry->desc;
	file_desc_ref(old_entry->desc);
	new_entry->fd_flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

	f->rax = (uint64_t) newfd;
	return f->rax;
}
