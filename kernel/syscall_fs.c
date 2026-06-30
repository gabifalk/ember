/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Filesystem syscall dispatcher. Implementations are in:
 *   syscall_fs_stat.c  -- stat family
 *   syscall_fs_path.c  -- path operations (access, getcwd, readlink, chdir, getdents)
 *   syscall_fs_mod.c   -- creation/modification (unlink, rename, mkdir, link, symlink, etc.)
 *   syscall_fs_perm.c  -- permissions (chmod, fchmod, fchmodat)
 */
#include "syscall_helpers.h"

uint64_t
syscall_handle_fs(syscall_frame_t * f)
{
	switch (f->rax) {
	case SYS_FSTAT:
		return sys_fstat(f);
	case SYS_STAT:
		return sys_stat(f);
	case SYS_LSTAT:
		return sys_lstat(f);
	case SYS_NEWFSTATAT:
		return sys_newfstatat(f);
	case SYS_ACCESS:
		return sys_access(f);
	case SYS_FACCESSAT2:
	case SYS_FACCESSAT:
		return sys_faccessat(f);
	case SYS_GETCWD:
		return sys_getcwd(f);
	case SYS_READLINK:
		return sys_readlink(f);
	case SYS_READLINKAT:
		return sys_readlinkat(f);
	case SYS_UNLINK:
		return sys_unlink(f);
	case SYS_RENAME:
		return sys_rename(f);
	case SYS_TRUNCATE:
		return sys_truncate(f);
	case SYS_FTRUNCATE:
		return sys_ftruncate(f);
	case SYS_MKDIR:
		return sys_mkdir(f);
	case SYS_MKDIRAT:
		return sys_mkdirat(f);
	case SYS_RMDIR:
		return sys_rmdir(f);
	case SYS_CHDIR:
		return sys_chdir(f);
	case SYS_FCHDIR:
		return sys_fchdir(f);
	case SYS_CHROOT:
		return sys_chroot(f);
	case SYS_LINK:
		return sys_link(f);
	case SYS_LINKAT:
		return sys_linkat(f);
	case SYS_SYMLINK:
		return sys_symlink(f);
	case SYS_SYMLINKAT:
		return sys_symlinkat(f);
	case SYS_MKNOD:
		return sys_mknod(f);
	case SYS_MKNODAT:
		return sys_mknodat(f);
	case SYS_CHMOD:
		return sys_chmod(f);
	case SYS_FCHMOD:
		return sys_fchmod(f);
	case SYS_FCHMODAT:
		return sys_fchmodat(f);
	case SYS_CHOWN:
	case SYS_FCHOWN:
	case SYS_LCHOWN:
	case SYS_FCHOWNAT:
		f->rax = 0;
		return 0;
	case SYS_GETDENTS64:
		return sys_getdents64(f);
	case SYS_FSYNC:
		return sys_fsync(f);
	case SYS_MOUNT:
	case SYS_UMOUNT2:
		f->rax = 0;
		return 0;
	case SYS_STATFS:
		return sys_statfs(f);
	case SYS_FSTATFS:
		return sys_fstatfs(f);
	case SYS_UTIMENSAT:
		return sys_utimensat(f);
	case SYS_UNLINKAT:
		return sys_unlinkat(f);
	case SYS_RENAMEAT:
	case SYS_RENAMEAT2:
		return sys_renameat(f);
	default:
		f->rax = SYSCALL_ERR(ENOSYS);
		return f->rax;
	}
}
