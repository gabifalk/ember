/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

uint64_t
syscall_handle_file(syscall_frame_t * f)
{
	switch (f->rax) {

	case SYS_READ:
		return sys_read(f);
	case SYS_WRITE:
		return sys_write(f);
	case SYS_WRITEV:
		return sys_writev(f);
	case SYS_READV:
		return sys_readv(f);
	case SYS_PREAD64:
		return sys_pread64(f);
	case SYS_PWRITE64:
		return sys_pwrite64(f);
	case SYS_SENDFILE:
		return sys_sendfile(f);
	case SYS_LSEEK:
		return sys_lseek(f);
	case SYS_IOCTL:
		return sys_ioctl(f);
	case SYS_FCNTL:
		return sys_fcntl(f);
	case SYS_DUP:
		return sys_dup(f);

	case SYS_MEMFD_CREATE:{
			const char *user_name = (const char *)f->rdi;
			uint32_t flags = (uint32_t) f->rsi;

			if (flags != 0) {
				f->rax = SYSCALL_ERR(EINVAL);
				return f->rax;
			}

			/* Copy name from user (max 249 bytes to fit "memfd:" prefix in 256) */
			char namebuf[250];
			namebuf[0] = '\0';
			if (user_name) {
				for (int i = 0; i < 249; i++) {
					char c = ((const char *)user_name)[i];
					namebuf[i] = c;
					if (c == '\0')
						break;
					if (i == 248)
						namebuf[249] = '\0';
				}
			}

			/* Build "memfd:<name>" path string. */
			char prefix[] = "memfd:";
			int plen = 6;
			int nlen = 0;
			while (namebuf[nlen])
				nlen++;
			char *path = (char *)kmalloc(plen + nlen + 1);
			if (!path) {
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}
			for (int i = 0; i < plen; i++)
				path[i] = prefix[i];
			for (int i = 0; i <= nlen; i++)
				path[plen + i] = namebuf[i];

			/* Allocate a standalone vfs_node (NOT in VFS cache) */
			vfs_node_t *node =
			    (vfs_node_t *) kmalloc(sizeof(vfs_node_t));
			if (!node) {
				kfree(path);
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}
			/* Zero the entire struct. */
			uint8_t *p = (uint8_t *) node;
			for (unsigned i = 0; i < sizeof(vfs_node_t); i++)
				p[i] = 0;

			node->path = path;
			node->type = VFS_NODE_FILE;
			node->fs_type = VFS_FS_MEMFD;
			node->mode = 0600;
			node->refcount = 1;
			/* Data = NULL, size = 0 (empty file) */

			/* Allocate fd. */
			int nfd = fd_alloc();
			if (nfd < 0) {
				kfree(path);
				kfree(node);
				f->rax = SYSCALL_ERR(EMFILE);
				return f->rax;
			}
			file_desc_t *d = file_desc_alloc();
			if (!d) {
				kfree(path);
				kfree(node);
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}
			d->type = FD_TYPE_FILE;
			d->flags = O_RDWR;
			d->node = node;

			fd_entry_t *entry = fd_get_raw(nfd);
			entry->desc = d;
			entry->fd_flags = 0;

			f->rax = (uint64_t) nfd;
			return f->rax;
		}

	case SYS_CLOSE:{
			int fd = (int)f->rdi;
			fd_entry_t *entry = syscall_fd_get(fd, f);
			if (!entry)
				return f->rax;
			file_desc_unref(entry->desc);
			entry->desc = 0;
			entry->fd_flags = 0;
			f->rax = 0;
			return 0;
		}

	case SYS_OPEN:{
			const char *user_path = (const char *)f->rdi;
			uint32_t oflags = (uint32_t) f->rsi;
			uint16_t create_mode = (uint16_t) (f->rdx & S_ALLPERMS);
			if (current_proc)
				create_mode &= ~(uint16_t) current_proc->umask;

			char pathbuf[EMBER_PATH_MAX];
			char resolved[EMBER_PATH_MAX];
			copy_path_from_user(user_path, pathbuf,
					    sizeof(pathbuf));
			if (pathbuf[0] == '\0') {
				f->rax = SYSCALL_ERR(ENOENT);
				return f->rax;
			}
			resolve_path(pathbuf, resolved, sizeof(resolved));

			if (kstreq(resolved, "/dev/null")
			    || kstreq(resolved, "/dev/zero")) {
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type =
				    kstreq(resolved,
					   "/dev/null") ? FD_TYPE_DEVNULL :
				    FD_TYPE_DEVZERO;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			if (is_devrandom(resolved)) {
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_DEVRANDOM;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			if (is_proc_iomem(resolved)) {
				uint64_t iomem_len = 0;
				char *iomem_buf =
				    kexec_generate_iomem(&iomem_len);
				if (!iomem_buf) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				int nfd = fd_alloc();
				if (nfd < 0) {
					kfree(iomem_buf);
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					kfree(iomem_buf);
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_PROC_IOMEM;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				d->node = (vfs_node_t *) iomem_buf;
				d->offset = 0;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			if (kstreq(resolved, "/dev/tty")
			    || kstreq(resolved, "/dev/console")) {
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_CONSOLE;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			int lookup_err = 0;
			int nofollow = (oflags & O_NOFOLLOW) ? 1 : 0;
			vfs_node_t *node =
			    vfs_lookup_err(resolved, nofollow, &lookup_err);
			if (!node && lookup_err == -ELOOP) {
				f->rax = SYSCALL_ERR(ELOOP);
				return f->rax;
			}
			if (node && nofollow && node->type == VFS_NODE_SYMLINK) {
				f->rax = SYSCALL_ERR(ELOOP);
				return f->rax;
			}
			if (!node && (oflags & O_CREAT)) {
				int create_err = 0;
				node =
				    vfs_create(resolved, create_mode,
					       &create_err);
				if (!node) {
					f->rax =
					    (uint64_t) (create_err ? create_err
							: -EIO);
					return f->rax;
				}
			}
			if (!node) {
				f->rax = SYSCALL_ERR(ENOENT);
				return f->rax;
			}
			if (node->type == VFS_NODE_DIR) {
				/* Allow opening directories for reading (getdents) */
				uint32_t accmode = oflags & 3;
				if (accmode != O_RDONLY) {
					f->rax = SYSCALL_ERR(EISDIR);
					return f->rax;
				}
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_DIR;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				d->node = node;
				vfs_ref(node);
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}
			/* O_DIRECTORY on non-directory -> ENOTDIR (Linux behavior) */
			if (oflags & O_DIRECTORY) {
				f->rax = SYSCALL_ERR(ENOTDIR);
				return f->rax;
			}
			if ((oflags & O_TRUNC) && node->ext2_ino) {
				ext2_truncate(node->ext2_ino, 0);
				vfs_set_size(node, 0);
			}
			int nfd = fd_alloc();
			if (nfd < 0) {
				f->rax = SYSCALL_ERR(EMFILE);
				return f->rax;
			}
			file_desc_t *d = file_desc_alloc();
			if (!d) {
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}
			d->type = FD_TYPE_FILE;
			d->flags = oflags & ~(uint32_t) O_CLOEXEC;
			d->offset = (oflags & O_APPEND) ? node->size : 0;
			d->node = node;
			vfs_ref(node);
			fd_entry_t *entry = fd_get_raw(nfd);
			entry->desc = d;
			entry->fd_flags = (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
			f->rax = (uint64_t) nfd;
			return f->rax;
		}

	case SYS_OPENAT:{
			int dirfd = (int)f->rdi;
			const char *user_path = (const char *)f->rsi;
			uint32_t oflags = (uint32_t) f->rdx;
			uint16_t create_mode = (uint16_t) (f->r10 & S_ALLPERMS);
			if (current_proc)
				create_mode &= ~(uint16_t) current_proc->umask;

			char pathbuf[EMBER_PATH_MAX];
			char resolved[EMBER_PATH_MAX];
			copy_path_from_user(user_path, pathbuf,
					    sizeof(pathbuf));
			if (pathbuf[0] == '\0') {
				f->rax = SYSCALL_ERR(ENOENT);
				return f->rax;
			}
			if (pathbuf[0] == '/' || dirfd == AT_FDCWD) {
				resolve_path(pathbuf, resolved,
					     sizeof(resolved));
			} else {
				/* Real dirfd: use fd's node path as base. */
				fd_entry_t *de = fd_get(dirfd);
				if (!de || !de->desc->node
				    || !de->desc->node->path) {
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

			if (kstreq(resolved, "/dev/null")
			    || kstreq(resolved, "/dev/zero")) {
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type =
				    kstreq(resolved,
					   "/dev/null") ? FD_TYPE_DEVNULL :
				    FD_TYPE_DEVZERO;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			if (is_devrandom(resolved)) {
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_DEVRANDOM;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			if (is_proc_iomem(resolved)) {
				uint64_t iomem_len = 0;
				char *iomem_buf =
				    kexec_generate_iomem(&iomem_len);
				if (!iomem_buf) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				int nfd = fd_alloc();
				if (nfd < 0) {
					kfree(iomem_buf);
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					kfree(iomem_buf);
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_PROC_IOMEM;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				d->node = (vfs_node_t *) iomem_buf;
				d->offset = 0;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			if (kstreq(resolved, "/dev/tty")
			    || kstreq(resolved, "/dev/console")) {
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_CONSOLE;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}

			int lookup_err2 = 0;
			int nofollow2 = (oflags & O_NOFOLLOW) ? 1 : 0;
			vfs_node_t *node =
			    vfs_lookup_err(resolved, nofollow2, &lookup_err2);
			if (!node && lookup_err2 == -ELOOP) {
				f->rax = SYSCALL_ERR(ELOOP);
				return f->rax;
			}
			if (node && nofollow2 && node->type == VFS_NODE_SYMLINK) {
				f->rax = SYSCALL_ERR(ELOOP);
				return f->rax;
			}
			if (!node && (oflags & O_CREAT)) {
				int create_err = 0;
				node =
				    vfs_create(resolved, create_mode,
					       &create_err);
				if (!node) {
					f->rax =
					    (uint64_t) (create_err ? create_err
							: -EIO);
					return f->rax;
				}
			}
			if (!node) {
				f->rax = SYSCALL_ERR(ENOENT);
				return f->rax;
			}
			if (node->type == VFS_NODE_DIR) {
				uint32_t accmode = oflags & 3;
				if (accmode != O_RDONLY) {
					f->rax = SYSCALL_ERR(EISDIR);
					return f->rax;
				}
				int nfd = fd_alloc();
				if (nfd < 0) {
					f->rax = SYSCALL_ERR(EMFILE);
					return f->rax;
				}
				file_desc_t *d = file_desc_alloc();
				if (!d) {
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				d->type = FD_TYPE_DIR;
				d->flags = oflags & ~(uint32_t) O_CLOEXEC;
				d->node = node;
				vfs_ref(node);
				fd_entry_t *entry = fd_get_raw(nfd);
				entry->desc = d;
				entry->fd_flags =
				    (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
				f->rax = (uint64_t) nfd;
				return f->rax;
			}
			/* O_DIRECTORY on non-directory -> ENOTDIR (Linux behavior) */
			if (oflags & O_DIRECTORY) {
				f->rax = SYSCALL_ERR(ENOTDIR);
				return f->rax;
			}
			if ((oflags & O_TRUNC) && node->ext2_ino) {
				ext2_truncate(node->ext2_ino, 0);
				vfs_set_size(node, 0);
			}
			int nfd = fd_alloc();
			if (nfd < 0) {
				f->rax = SYSCALL_ERR(EMFILE);
				return f->rax;
			}
			file_desc_t *d = file_desc_alloc();
			if (!d) {
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}
			d->type = FD_TYPE_FILE;
			d->flags = oflags & ~(uint32_t) O_CLOEXEC;
			d->offset = (oflags & O_APPEND) ? node->size : 0;
			d->node = node;
			vfs_ref(node);
			fd_entry_t *entry = fd_get_raw(nfd);
			entry->desc = d;
			entry->fd_flags = (oflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
			f->rax = (uint64_t) nfd;
			return f->rax;
		}

	case SYS_FLOCK:
		/* Stub: accept silently -- no real locking needed for single-user kernel. */
		f->rax = 0;
		return 0;

	default:
		f->rax = SYSCALL_ERR(ENOSYS);
		return f->rax;
	}
}
