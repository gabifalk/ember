/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Filesystem syscalls: path operations (access, getcwd, readlink, chdir, chroot, getdents64)
 */
#include "syscall_helpers.h"

uint64_t
sys_access(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	if (is_devnull(resolved) || is_devzero(resolved)
	    || is_devrandom(resolved) || is_proc_self_exe(resolved)
	    || is_devtty(resolved) || is_devconsole(resolved)) {
		f->rax = 0;
		return 0;
	}
	vfs_node_t *node = vfs_lookup(resolved);
	if (!node) {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_faccessat(syscall_frame_t * f)
{
	int afdirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '/' || afdirfd == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(afdirfd);
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
	if (is_devnull(resolved) || is_devzero(resolved)
	    || is_devrandom(resolved) || is_proc_self_exe(resolved)
	    || is_devtty(resolved) || is_devconsole(resolved)) {
		f->rax = 0;
		return 0;
	}
	vfs_node_t *node = vfs_lookup(resolved);
	if (!node) {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_getcwd(syscall_frame_t * f)
{
	char *user_buf = (char *)f->rdi;
	uint64_t size = f->rsi;
	const char *cwd = current_proc ? current_proc->cwd : "/";
	uint64_t cwdlen = 0;
	while (cwd[cwdlen])
		cwdlen++;
	if (size < cwdlen + 1) {
		f->rax = SYSCALL_ERR(ERANGE);
		return f->rax;
	}
	USER_ACCESS_BEGIN();
	for (uint64_t i = 0; i <= cwdlen; i++)
		user_buf[i] = cwd[i];
	USER_ACCESS_END();
	f->rax = cwdlen + 1;	/* Return length including NUL, like Linux. */
	return f->rax;
}

uint64_t
sys_readlink(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	char *user_buf = (char *)f->rsi;
	uint64_t bufsiz = f->rdx;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	if (is_proc_self_exe(resolved)) {
		const char *ep = current_proc ? current_proc->exe_path : "";
		uint64_t elen = 0;
		while (ep[elen])
			elen++;
		if (elen == 0) {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
		uint64_t copylen = elen;
		if (copylen > bufsiz)
			copylen = bufsiz;
		USER_ACCESS_BEGIN();
		for (uint64_t i = 0; i < copylen; i++)
			user_buf[i] = ep[i];
		USER_ACCESS_END();
		f->rax = copylen;
		return f->rax;
	}
	char linkbuf[EMBER_PATH_MAX];
	int r = vfs_readlink(resolved, linkbuf, sizeof(linkbuf));
	if (r < 0) {
		f->rax = (uint64_t) r;
		return f->rax;
	}
	uint64_t copylen = (uint64_t) r;
	if (copylen > bufsiz)
		copylen = bufsiz;
	USER_ACCESS_BEGIN();
	for (uint64_t i = 0; i < copylen; i++)
		user_buf[i] = linkbuf[i];
	USER_ACCESS_END();
	f->rax = copylen;
	return f->rax;
}

uint64_t
sys_readlinkat(syscall_frame_t * f)
{
	int rdirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	char *user_buf = (char *)f->rdx;
	uint64_t bufsiz = f->r10;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '/' || rdirfd == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(rdirfd);
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
	if (is_proc_self_exe(resolved)) {
		const char *ep = current_proc ? current_proc->exe_path : "";
		uint64_t elen = 0;
		while (ep[elen])
			elen++;
		if (elen == 0) {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
		uint64_t copylen = elen;
		if (copylen > bufsiz)
			copylen = bufsiz;
		USER_ACCESS_BEGIN();
		for (uint64_t i = 0; i < copylen; i++)
			user_buf[i] = ep[i];
		USER_ACCESS_END();
		f->rax = copylen;
		return f->rax;
	}
	char linkbuf[EMBER_PATH_MAX];
	int r = vfs_readlink(resolved, linkbuf, sizeof(linkbuf));
	if (r < 0) {
		f->rax = (uint64_t) r;
		return f->rax;
	}
	uint64_t copylen = (uint64_t) r;
	if (copylen > bufsiz)
		copylen = bufsiz;
	USER_ACCESS_BEGIN();
	for (uint64_t i = 0; i < copylen; i++)
		user_buf[i] = linkbuf[i];
	USER_ACCESS_END();
	f->rax = copylen;
	return f->rax;
}

uint64_t
sys_chdir(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0') {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	resolve_path(pathbuf, resolved, sizeof(resolved));
	vfs_node_t *node = vfs_lookup(resolved);
	if (!node) {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	if (node->type != VFS_NODE_DIR) {
		f->rax = SYSCALL_ERR(ENOTDIR);
		return f->rax;
	}
	if (current_proc) {
		int ci;
		for (ci = 0; ci < EMBER_PATH_MAX - 1 && resolved[ci]; ci++)
			current_proc->cwd[ci] = resolved[ci];
		current_proc->cwd[ci] = '\0';
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_fchdir(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	fd_entry_t *entry = fd_get(fd);
	if (!entry || entry->desc->type != FD_TYPE_DIR) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	if (entry->desc->node && entry->desc->node->path && current_proc) {
		const char *p = entry->desc->node->path;
		int ci;
		for (ci = 0; ci < EMBER_PATH_MAX - 1 && p[ci]; ci++)
			current_proc->cwd[ci] = p[ci];
		current_proc->cwd[ci] = '\0';
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_chroot(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	/* Verify directory exists. */
	vfs_node_t *node = vfs_lookup(resolved);
	if (!node) {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	if (node->type != VFS_NODE_DIR) {
		f->rax = SYSCALL_ERR(ENOTDIR);
		return f->rax;
	}
	if (current_proc) {
		int ci;
		for (ci = 0; ci < EMBER_PATH_MAX - 1 && resolved[ci]; ci++)
			current_proc->root_path[ci] = resolved[ci];
		current_proc->root_path[ci] = '\0';
		current_proc->cwd[0] = '/';
		current_proc->cwd[1] = '\0';
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_getdents64(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	void *user_buf = (void *)f->rsi;
	uint64_t count = f->rdx;
	fd_entry_t *entry = fd_get(fd);
	if (!entry || entry->desc->type != FD_TYPE_DIR) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	USER_ACCESS_BEGIN();
	spin_lock(&entry->desc->lock);
	uint64_t new_offset = entry->desc->offset;
	int64_t ret =
	    vfs_getdents(entry->desc->node, entry->desc->offset, user_buf,
			 count, &new_offset);
	entry->desc->offset = new_offset;
	spin_unlock(&entry->desc->lock);
	USER_ACCESS_END();
	if (ret == -ENOTDIR) {
		vfs_node_t *dn = entry->desc->node;
		console_write("getdents ENOTDIR: fd=");
		console_hex64((uint64_t) fd);
		console_write(" node_type=");
		console_hex64((uint64_t) dn->type);
		console_write(" ext2_ino=");
		console_hex64((uint64_t) dn->ext2_ino);
		console_write(" refcount=");
		console_hex64((uint64_t) dn->refcount);
		console_write(" unlinked=");
		console_hex64((uint64_t) dn->unlinked);
		console_write("\n");
		if (dn->path) {
			console_write("  path=");
			console_write(dn->path);
			console_write("\n");
			/* Re-resolve the path to see what inode it maps to NOW. */
			uint32_t cur_ino = ext2_path_resolve(dn->path);
			console_write("  resolve_now=");
			console_hex64((uint64_t) cur_ino);
			console_write("\n");
		}
	}
	if (ret < 0) {
		f->rax = (uint64_t) ret;
		return f->rax;
	}
	f->rax = (uint64_t) ret;
	return f->rax;
}
