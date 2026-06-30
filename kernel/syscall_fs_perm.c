/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Filesystem syscalls: permissions (chmod, fchmod, fchmodat)
 * Note: chown/fchown/lchown/fchownat are stubs handled inline in the dispatcher.
 */
#include "syscall_helpers.h"

uint64_t
sys_chmod(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	uint32_t mode = (uint32_t) f->rsi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	int r = vfs_chmod(resolved, mode);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_fchmod(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	uint32_t mode = (uint32_t) f->rsi;
	fd_entry_t *entry = fd_get(fd);
	if (!entry || !entry->desc->node) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	if (entry->desc->node->ext2_ino) {
		int r =
		    ext2_chmod(entry->desc->node->ext2_ino, (uint16_t) mode);
		if (r < 0) {
			f->rax = SYSCALL_ERR(EIO);
			return f->rax;
		}
		entry->desc->node->mode =
		    (entry->desc->node->mode & S_IFMT) | (mode & S_ALLPERMS);
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_fchmodat(syscall_frame_t * f)
{
	int cmfdirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	uint32_t mode = (uint32_t) f->rdx;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '/' || cmfdirfd == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(cmfdirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			resolved[bi] = bp[bi];
			bi++;
		}
		resolved[bi++] = '/';
		uint64_t pi = 0;
		while (pathbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			resolved[bi++] = pathbuf[pi++];
		}
		resolved[bi] = '\0';
	}
	int r = vfs_chmod(resolved, mode);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}
