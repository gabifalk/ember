/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Filesystem syscalls: stat family (fstat, stat, lstat, newfstatat, statfs, fstatfs)
 */
#include "syscall_helpers.h"

uint64_t
sys_fstat(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	struct linux_stat *user_stat = (struct linux_stat *)f->rsi;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;

	struct linux_stat kst;
	fill_stat(&kst, entry->desc);

	USER_ACCESS_BEGIN();

	uint8_t *dst = (uint8_t *) user_stat;
	uint8_t *src = (uint8_t *) & kst;
	for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
		dst[i] = src[i];

	USER_ACCESS_END();
	f->rax = 0;
	return 0;
}

uint64_t
sys_stat(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	struct linux_stat *user_stat = (struct linux_stat *)f->rsi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0') {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	resolve_path(pathbuf, resolved, sizeof(resolved));
	struct linux_stat kst;
	if (is_devnull(resolved)) {
		fill_devnull_stat(&kst);
	} else if (is_devzero(resolved)) {
		fill_devzero_stat(&kst);
	} else if (is_devrandom(resolved)) {
		fill_devrandom_stat(&kst);
	} else if (is_devtty(resolved) || is_devconsole(resolved)) {
		fill_devtty_stat(&kst);
	} else if (is_proc_iomem(resolved)) {
		kmemzero(&kst, sizeof(kst));
		kst.st_mode = S_IFREG | 0444;
		kst.st_ino = 100;
		kst.st_nlink = 1;
		kst.st_blksize = 4096;
	} else if (ext2_is_ready()) {
		uint32_t ino = ext2_path_resolve(resolved);
		if (ino) {
			ext2_inode_t ei;
			if (ext2_read_inode(ino, &ei) < 0) {
				f->rax = SYSCALL_ERR(EIO);
				return f->rax;
			}
			uint8_t *p = (uint8_t *) & kst;
			for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
				p[i] = 0;
			kst.st_dev = 0x0801;
			kst.st_mode = ei.i_mode;
			kst.st_size = (int64_t) ei.i_size;
			kst.st_ino = ino;
			kst.st_nlink = ei.i_links_count;
			kst.st_uid = ei.i_uid;
			kst.st_gid = ei.i_gid;
			kst.st_blksize = 4096;
			kst.st_blocks = (int64_t) ((ei.i_size + 511) / 512);
			kst.st_atime_sec = ei.i_atime;
			kst.st_mtime_sec = ei.i_mtime;
			kst.st_ctime_sec = ei.i_ctime;
		} else {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
	} else {
		vfs_node_t *node = vfs_lookup(resolved);
		if (!node) {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
		fill_stat_from_node(&kst, node);
	}
	USER_ACCESS_BEGIN();
	uint8_t *dst = (uint8_t *) user_stat;
	uint8_t *src = (uint8_t *) & kst;
	for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
		dst[i] = src[i];
	USER_ACCESS_END();
	f->rax = 0;
	return 0;
}

uint64_t
sys_lstat(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	struct linux_stat *user_stat = (struct linux_stat *)f->rsi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0') {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	resolve_path(pathbuf, resolved, sizeof(resolved));
	struct linux_stat kst;
	if (is_devnull(resolved)) {
		fill_devnull_stat(&kst);
	} else if (is_devzero(resolved)) {
		fill_devzero_stat(&kst);
	} else if (is_devrandom(resolved)) {
		fill_devrandom_stat(&kst);
	} else if (is_devtty(resolved) || is_devconsole(resolved)) {
		fill_devtty_stat(&kst);
	} else if (is_proc_self_exe(resolved)) {
		uint8_t *p = (uint8_t *) & kst;
		for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
			p[i] = 0;
		kst.st_mode = S_IFLNK | S_ACCESSPERMS;
		kst.st_ino = 1;
		kst.st_nlink = 1;
		kst.st_blksize = 4096;
	} else if (ext2_is_ready()) {
		uint32_t ino = ext2_path_resolve_nofollow(resolved);
		if (ino) {
			ext2_inode_t ei;
			if (ext2_read_inode(ino, &ei) < 0) {
				f->rax = SYSCALL_ERR(EIO);
				return f->rax;
			}
			uint8_t *p = (uint8_t *) & kst;
			for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
				p[i] = 0;
			kst.st_dev = 0x0801;
			kst.st_mode = ei.i_mode;
			kst.st_size = (int64_t) ei.i_size;
			kst.st_ino = ino;
			kst.st_nlink = ei.i_links_count;
			kst.st_uid = ei.i_uid;
			kst.st_gid = ei.i_gid;
			kst.st_blksize = 4096;
			kst.st_blocks = (int64_t) ((ei.i_size + 511) / 512);
			kst.st_atime_sec = ei.i_atime;
			kst.st_mtime_sec = ei.i_mtime;
			kst.st_ctime_sec = ei.i_ctime;
		} else {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
	} else {
		vfs_node_t *node = vfs_lookup(resolved);
		if (!node) {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
		fill_stat_from_node(&kst, node);
	}
	USER_ACCESS_BEGIN();
	uint8_t *dst = (uint8_t *) user_stat;
	uint8_t *src = (uint8_t *) & kst;
	for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
		dst[i] = src[i];
	USER_ACCESS_END();
	f->rax = 0;
	return 0;
}

uint64_t
sys_newfstatat(syscall_frame_t * f)
{
	int dirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	struct linux_stat *user_stat = (struct linux_stat *)f->rdx;
	uint32_t flags = (uint32_t) f->r10;

	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0' && !(flags & AT_EMPTY_PATH)) {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	if (pathbuf[0] == '/' || dirfd == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(dirfd);
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
		normalize_path(resolved);
	}

	struct linux_stat kst;
	if (is_devnull(resolved)) {
		fill_devnull_stat(&kst);
	} else if (is_devzero(resolved)) {
		fill_devzero_stat(&kst);
	} else if (is_devrandom(resolved)) {
		fill_devrandom_stat(&kst);
	} else if (is_devtty(resolved) || is_devconsole(resolved)) {
		fill_devtty_stat(&kst);
	} else if (is_proc_self_exe(resolved)) {
		uint8_t *p = (uint8_t *) & kst;
		for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
			p[i] = 0;
		kst.st_mode = S_IFLNK | S_ACCESSPERMS;
		kst.st_ino = 1;
		kst.st_nlink = 1;
		kst.st_blksize = 4096;
	} else if (is_proc_iomem(resolved)) {
		kmemzero(&kst, sizeof(kst));
		kst.st_mode = S_IFREG | 0444;
		kst.st_ino = 100;
		kst.st_nlink = 1;
		kst.st_blksize = 4096;
	} else {
		vfs_node_t *node;
		if ((flags & AT_SYMLINK_NOFOLLOW) && ext2_is_ready()) {
			uint32_t ino = ext2_path_resolve_nofollow(resolved);
			if (ino) {
				ext2_inode_t ei;
				if (ext2_read_inode(ino, &ei) < 0) {
					f->rax = SYSCALL_ERR(EIO);
					return f->rax;
				}
				uint8_t *p = (uint8_t *) & kst;
				for (uint64_t i = 0;
				     i < sizeof(struct linux_stat); i++)
					p[i] = 0;
				kst.st_dev = 0x0801;
				kst.st_mode = ei.i_mode;
				kst.st_size = (int64_t) ei.i_size;
				kst.st_ino = ino;
				kst.st_nlink = ei.i_links_count;
				kst.st_uid = ei.i_uid;
				kst.st_gid = ei.i_gid;
				kst.st_blksize = 4096;
				kst.st_blocks =
				    (int64_t) ((ei.i_size + 511) / 512);
				kst.st_atime_sec = ei.i_atime;
				kst.st_mtime_sec = ei.i_mtime;
				kst.st_ctime_sec = ei.i_ctime;
				goto newfstatat_copyout;
			}
		}
		node = vfs_lookup(resolved);
		if (!node) {
			f->rax = SYSCALL_ERR(ENOENT);
			return f->rax;
		}
		fill_stat_from_node(&kst, node);
	}
 newfstatat_copyout:;
	USER_ACCESS_BEGIN();

	uint8_t *dst = (uint8_t *) user_stat;
	uint8_t *src = (uint8_t *) & kst;
	for (uint64_t i = 0; i < sizeof(struct linux_stat); i++)
		dst[i] = src[i];

	USER_ACCESS_END();
	f->rax = 0;
	return 0;
}

uint64_t
sys_statfs(syscall_frame_t * f)
{
	uint8_t kbuf[120];
	if (!ext2_is_ready() || ext2_statfs(kbuf) < 0) {
		/* No ext2: return a basic tmpfs-like statfs. */
		for (int i = 0; i < 120; i++)
			kbuf[i] = 0;
		uint64_t *q = (uint64_t *) kbuf;
		q[0] = 0x01021994;	/* TMPFS_MAGIC. */
		q[1] = 4096;	/* f_bsize. */
		q[8] = 255;	/* f_namelen. */
		q[9] = 4096;	/* f_frsize. */
	}
	uint64_t user_buf = f->rsi;
	USER_ACCESS_BEGIN();
	uint8_t *dst = (uint8_t *) (uintptr_t) user_buf;
	for (int i = 0; i < 120; i++)
		dst[i] = kbuf[i];
	USER_ACCESS_END();
	f->rax = 0;
	return 0;
}

uint64_t
sys_fstatfs(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	uint8_t kbuf[120];
	if (!ext2_is_ready() || ext2_statfs(kbuf) < 0) {
		for (int i = 0; i < 120; i++)
			kbuf[i] = 0;
		uint64_t *q = (uint64_t *) kbuf;
		q[0] = 0x01021994;
		q[1] = 4096;
		q[8] = 255;
		q[9] = 4096;
	}
	uint64_t user_buf = f->rsi;
	USER_ACCESS_BEGIN();
	uint8_t *dst = (uint8_t *) (uintptr_t) user_buf;
	for (int i = 0; i < 120; i++)
		dst[i] = kbuf[i];
	USER_ACCESS_END();
	f->rax = 0;
	return 0;
}
