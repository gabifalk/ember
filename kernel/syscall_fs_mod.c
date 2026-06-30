/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Filesystem syscalls: file/dir creation and modification
 * (unlink, rename, truncate, mkdir, rmdir, link, symlink, mknod, fsync,
 *  unlinkat, renameat, utimensat, mount/umount stubs)
 */
#include "syscall_helpers.h"

uint64_t
sys_unlink(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	int r = vfs_unlink(resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_rename(syscall_frame_t * f)
{
	const char *user_old = (const char *)f->rdi;
	const char *user_new = (const char *)f->rsi;
	char oldbuf[EMBER_PATH_MAX];
	char newbuf[EMBER_PATH_MAX];
	char old_resolved[EMBER_PATH_MAX];
	char new_resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_old, oldbuf, sizeof(oldbuf));
	copy_path_from_user(user_new, newbuf, sizeof(newbuf));
	resolve_path(oldbuf, old_resolved, sizeof(old_resolved));
	resolve_path(newbuf, new_resolved, sizeof(new_resolved));
	int r = vfs_rename(old_resolved, new_resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_truncate(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	uint64_t length = f->rsi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	int r = vfs_truncate_path(resolved, length);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_ftruncate(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	uint64_t length = f->rsi;
	fd_entry_t *entry = fd_get(fd);
	if (!entry || entry->desc->type != FD_TYPE_FILE) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	if (!(entry->desc->flags & (O_WRONLY | O_RDWR))) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}
	if (length > 0xffffffffULL) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}
	if (entry->desc->node && entry->desc->node->ext2_ino) {
		int r =
		    ext2_truncate(entry->desc->node->ext2_ino,
				  (uint32_t) length);
		if (r < 0) {
			f->rax = SYSCALL_ERR(EIO);
			return f->rax;
		}
		entry->desc->node->size = length;
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_mkdir(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	uint32_t mode = (uint32_t) f->rsi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0') {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	resolve_path(pathbuf, resolved, sizeof(resolved));
	int r = vfs_mkdir(resolved, mode);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_mkdirat(syscall_frame_t * f)
{
	int dirfd_mk = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	uint32_t mode = (uint32_t) f->rdx;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0') {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	if (pathbuf[0] == '/' || dirfd_mk == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(dirfd_mk);
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
	int r = vfs_mkdir(resolved, mode);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_rmdir(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	int r = vfs_rmdir(resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_link(syscall_frame_t * f)
{
	const char *user_old = (const char *)f->rdi;
	const char *user_new = (const char *)f->rsi;
	char oldbuf[EMBER_PATH_MAX];
	char newbuf[EMBER_PATH_MAX];
	char old_resolved[EMBER_PATH_MAX];
	char new_resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_old, oldbuf, sizeof(oldbuf));
	copy_path_from_user(user_new, newbuf, sizeof(newbuf));
	resolve_path(oldbuf, old_resolved, sizeof(old_resolved));
	resolve_path(newbuf, new_resolved, sizeof(new_resolved));
	int r = vfs_link(old_resolved, new_resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_linkat(syscall_frame_t * f)
{
	int olddirfd = (int)f->rdi;
	const char *user_old = (const char *)f->rsi;
	int newdirfd = (int)f->rdx;
	const char *user_new = (const char *)f->r10;
	char oldbuf[EMBER_PATH_MAX];
	char newbuf[EMBER_PATH_MAX];
	char old_resolved[EMBER_PATH_MAX];
	char new_resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_old, oldbuf, sizeof(oldbuf));
	copy_path_from_user(user_new, newbuf, sizeof(newbuf));
	if (oldbuf[0] == '/' || olddirfd == AT_FDCWD) {
		resolve_path(oldbuf, old_resolved, sizeof(old_resolved));
	} else {
		fd_entry_t *de = fd_get(olddirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			old_resolved[bi] = bp[bi];
			bi++;
		}
		old_resolved[bi++] = '/';
		uint64_t pi = 0;
		while (oldbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			old_resolved[bi++] = oldbuf[pi++];
		}
		old_resolved[bi] = '\0';
	}
	if (newbuf[0] == '/' || newdirfd == AT_FDCWD) {
		resolve_path(newbuf, new_resolved, sizeof(new_resolved));
	} else {
		fd_entry_t *de = fd_get(newdirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			new_resolved[bi] = bp[bi];
			bi++;
		}
		new_resolved[bi++] = '/';
		uint64_t pi = 0;
		while (newbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			new_resolved[bi++] = newbuf[pi++];
		}
		new_resolved[bi] = '\0';
	}
	int r = vfs_link(old_resolved, new_resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_symlink(syscall_frame_t * f)
{
	const char *user_target = (const char *)f->rdi;
	const char *user_linkpath = (const char *)f->rsi;
	char targetbuf[EMBER_PATH_MAX];
	char linkbuf[EMBER_PATH_MAX];
	char link_resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_target, targetbuf, sizeof(targetbuf));
	copy_path_from_user(user_linkpath, linkbuf, sizeof(linkbuf));
	resolve_path(linkbuf, link_resolved, sizeof(link_resolved));
	int r = vfs_symlink(targetbuf, link_resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_symlinkat(syscall_frame_t * f)
{
	const char *user_target = (const char *)f->rdi;
	int sdirfd = (int)f->rsi;
	const char *user_linkpath = (const char *)f->rdx;
	char targetbuf[EMBER_PATH_MAX];
	char linkbuf[EMBER_PATH_MAX];
	char link_resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_target, targetbuf, sizeof(targetbuf));
	copy_path_from_user(user_linkpath, linkbuf, sizeof(linkbuf));
	if (linkbuf[0] == '/' || sdirfd == AT_FDCWD) {
		resolve_path(linkbuf, link_resolved, sizeof(link_resolved));
	} else {
		fd_entry_t *de = fd_get(sdirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			link_resolved[bi] = bp[bi];
			bi++;
		}
		link_resolved[bi++] = '/';
		uint64_t pi = 0;
		while (linkbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			link_resolved[bi++] = linkbuf[pi++];
		}
		link_resolved[bi] = '\0';
	}
	int r = vfs_symlink(targetbuf, link_resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_mknod(syscall_frame_t * f)
{
	const char *user_path = (const char *)f->rdi;
	uint32_t mode = (uint32_t) f->rsi;
	uint64_t dev = f->rdx;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved, sizeof(resolved));
	int r = vfs_mknod(resolved, mode, dev);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_mknodat(syscall_frame_t * f)
{
	int mndirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	uint32_t mode = (uint32_t) f->rdx;
	uint64_t dev = f->r10;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '/' || mndirfd == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(mndirfd);
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
	int r = vfs_mknod(resolved, mode, dev);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_fsync(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	if (ext2_is_ready()) {
		blkcache_sync(ext2_get_dev());
		blkdev_flush(ext2_get_dev());
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_utimensat(syscall_frame_t * f)
{
	int dirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	uint64_t user_times = f->rdx;
	/* F->r10 = flags (unused for now) */

	/* Get current time. */
	uint64_t now = kernel_time_sec();

	/* Determine atime and mtime. */
	uint32_t new_atime = (uint32_t) now;
	uint32_t new_mtime = (uint32_t) now;
	int set_atime = 1, set_mtime = 1;

	if (user_times) {
		/* Read two struct timespec from user space: {int64_t sec, int64_t nsec} each. */
		USER_ACCESS_BEGIN();
		int64_t *ts = (int64_t *) (uintptr_t) user_times;
		int64_t atime_sec = ts[0];
		int64_t atime_nsec = ts[1];
		int64_t mtime_sec = ts[2];
		int64_t mtime_nsec = ts[3];
		USER_ACCESS_END();

		if (atime_nsec == UTIME_OMIT)
			set_atime = 0;
		else if (atime_nsec == UTIME_NOW)
			new_atime = (uint32_t) now;
		else
			new_atime = (uint32_t) atime_sec;

		if (mtime_nsec == UTIME_OMIT)
			set_mtime = 0;
		else if (mtime_nsec == UTIME_NOW)
			new_mtime = (uint32_t) now;
		else
			new_mtime = (uint32_t) mtime_sec;
	}

	if (!set_atime && !set_mtime) {
		f->rax = 0;
		return 0;
	}

	/* Resolve the target path (or use fd for futimens) */
	if (!user_path || (user_path && user_times == 0 && dirfd >= 0)) {
		/* Futimens: user_path is NULL, use dirfd. */
		if (!user_path) {
			fd_entry_t *entry = fd_get(dirfd);
			if (!entry || !entry->desc->node) {
				f->rax = SYSCALL_ERR(EBADF);
				return f->rax;
			}
			if (entry->desc->node->ext2_ino) {
				ext2_inode_t ei;
				if (ext2_read_inode
				    (entry->desc->node->ext2_ino, &ei) < 0) {
					f->rax = SYSCALL_ERR(EIO);
					return f->rax;
				}
				if (set_atime)
					ei.i_atime = new_atime;
				if (set_mtime)
					ei.i_mtime = new_mtime;
				ei.i_ctime = (uint32_t) now;
				if (ext2_write_inode
				    (entry->desc->node->ext2_ino, &ei) < 0) {
					f->rax = SYSCALL_ERR(EIO);
					return f->rax;
				}
				/* Update VFS cache node to match. */
				vfs_node_t *n = entry->desc->node;
				if (set_atime)
					n->atime = new_atime;
				if (set_mtime)
					n->mtime = new_mtime;
				n->ctime = (uint32_t) now;
			}
			/* Cpio/tmpfs: silently succeed. */
			f->rax = 0;
			return 0;
		}
	}

	/* Path-based: resolve via dirfd pattern. */
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
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
	}

	/* Look up in ext2 if available. */
	if (ext2_is_ready()) {
		uint32_t ino = ext2_path_resolve_nofollow(resolved);
		if (!ino) {
			/* Try via vfs_lookup to see if it exists at all. */
			vfs_node_t *node = vfs_lookup(resolved);
			if (!node) {
				f->rax = SYSCALL_ERR(ENOENT);
				return f->rax;
			}
			if (node->ext2_ino)
				ino = node->ext2_ino;
		}
		if (ino) {
			ext2_inode_t ei;
			if (ext2_read_inode(ino, &ei) < 0) {
				f->rax = SYSCALL_ERR(EIO);
				return f->rax;
			}
			if (set_atime)
				ei.i_atime = new_atime;
			if (set_mtime)
				ei.i_mtime = new_mtime;
			ei.i_ctime = (uint32_t) now;
			if (ext2_write_inode(ino, &ei) < 0) {
				f->rax = SYSCALL_ERR(EIO);
				return f->rax;
			}
			/* Update VFS cache node to match. */
			vfs_node_t *cn = vfs_lookup(resolved);
			if (cn) {
				if (set_atime)
					cn->atime = new_atime;
				if (set_mtime)
					cn->mtime = new_mtime;
				cn->ctime = (uint32_t) now;
			}
			f->rax = 0;
			return 0;
		}
	}

	/* Cpio-only or no ext2 inode: verify path exists, succeed silently. */
	vfs_node_t *node = vfs_lookup(resolved);
	if (!node) {
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	f->rax = 0;
	return 0;
}

uint64_t
sys_unlinkat(syscall_frame_t * f)
{
	int dirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	int flags = (int)f->rdx;
	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if (pathbuf[0] == '\0') {
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
	int r;
	if (flags & AT_REMOVEDIR)
		r = vfs_rmdir(resolved);
	else
		r = vfs_unlink(resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}

uint64_t
sys_renameat(syscall_frame_t * f)
{
	int olddirfd = (int)f->rdi;
	const char *user_old = (const char *)f->rsi;
	int newdirfd = (int)f->rdx;
	const char *user_new = (const char *)f->r10;
	char oldbuf[EMBER_PATH_MAX];
	char newbuf[EMBER_PATH_MAX];
	char old_resolved[EMBER_PATH_MAX];
	char new_resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_old, oldbuf, sizeof(oldbuf));
	copy_path_from_user(user_new, newbuf, sizeof(newbuf));
	if (oldbuf[0] == '/' || olddirfd == AT_FDCWD) {
		resolve_path(oldbuf, old_resolved, sizeof(old_resolved));
	} else {
		fd_entry_t *de = fd_get(olddirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			old_resolved[bi] = bp[bi];
			bi++;
		}
		old_resolved[bi++] = '/';
		uint64_t pi = 0;
		while (oldbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			old_resolved[bi++] = oldbuf[pi++];
		}
		old_resolved[bi] = '\0';
	}
	if (newbuf[0] == '/' || newdirfd == AT_FDCWD) {
		resolve_path(newbuf, new_resolved, sizeof(new_resolved));
	} else {
		fd_entry_t *de = fd_get(newdirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			new_resolved[bi] = bp[bi];
			bi++;
		}
		new_resolved[bi++] = '/';
		uint64_t pi = 0;
		while (newbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			new_resolved[bi++] = newbuf[pi++];
		}
		new_resolved[bi] = '\0';
	}
	int r = vfs_rename(old_resolved, new_resolved);
	f->rax = (r < 0) ? (uint64_t) r : 0;
	return f->rax;
}
