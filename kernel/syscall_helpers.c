/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

volatile int console_echo = 1;	/* Default: echo on. */
volatile int console_icanon = 1;	/* Default: canonical mode. */
volatile int console_fg_pgid = 0;	/* Foreground process group (set by TIOCSPGRP) */

/*
 * Send a signal to all processes in the foreground process group.
 * Uses console_fg_pgid (set by tcsetpgrp/TIOCSPGRP) if available,
 * otherwise falls back to current_proc->pgid.
 */
void
console_signal_fg(int sig)
{
	int pgid = console_fg_pgid;
	if (pgid <= 0) {
		proc_t *cur = current_proc;
		pgid = cur ? cur->pgid : 0;
	}
	if (pgid <= 0)
		return;
	uint64_t sf = spin_lock_irqsave(&sched_lock);
	for (int i = 0; i < MAX_PROCS; i++) {
		if (procs[i].state != PROC_UNUSED && procs[i].pgid == pgid) {
			procs[i].sig_pending |= (1u << sig);
			if (procs[i].state == PROC_SLEEPING)
				procs[i].state = PROC_READY;
		}
	}
	spin_unlock_irqrestore(&sched_lock, sf);
}

void
write_user_buf(const char *buf, uint64_t len)
{
	uint64_t remaining = len;
	const char *p = buf;
	while (remaining > 0) {
		uint64_t chunk = remaining > 255 ? 255 : remaining;
		char tmp[256];
		kmemcpy(tmp, p, chunk);
		tmp[chunk] = '\0';
		console_write(tmp);
		p += chunk;
		remaining -= chunk;
	}
}

int
copy_path_from_user(const char *user_ptr, char *kbuf, uint64_t kbuf_size)
{
	if (!user_ptr) {
		kbuf[0] = '\0';
		return -1;
	}
	uint64_t old = read_cr3();
	uint64_t ucr3 =
	    current_proc ? current_proc->pml4_phys : cpu0_user_cr3();
	if (ucr3)
		write_cr3(ucr3);

	uint64_t i;
	for (i = 0; i < kbuf_size - 1; i++) {
		kbuf[i] = user_ptr[i];
		if (kbuf[i] == '\0')
			break;
	}
	kbuf[i] = '\0';

	if (ucr3)
		write_cr3(old);
	return (int)i;
}

uint64_t
get_user_cr3(void)
{
	if (current_proc)
		return current_proc->pml4_phys;
	return cpu0_user_cr3();
}

void
fill_stat(struct linux_stat *st, file_desc_t * entry)
{
	kmemzero(st, sizeof(struct linux_stat));

	if (entry->type == FD_TYPE_CONSOLE) {
		st->st_dev = 5;	/* /Dev/tty major=5. */
		st->st_mode = S_IFCHR | 0620;
		st->st_rdev = 0x8800;
		st->st_ino = 2;
		st->st_nlink = 1;
		st->st_blksize = 4096;
	} else if (entry->type == FD_TYPE_FILE && entry->node) {
		vfs_node_t *n = entry->node;
		st->st_dev = n->fs_type == VFS_FS_EXT2 ? 0x0801 : 0x0001;
		st->st_mode = n->mode ? n->mode : (S_IFREG | 0644);
		st->st_size = (int64_t) n->size;
		st->st_ino =
		    n->ext2_ino ? n->ext2_ino : (uint64_t) (uintptr_t) n;
		st->st_nlink = 1;
		st->st_blksize = 4096;
		st->st_blocks = (int64_t) ((n->size + 511) / 512);
		st->st_atime_sec = n->atime;
		st->st_mtime_sec = n->mtime;
		st->st_ctime_sec = n->ctime;
	} else if (entry->type == FD_TYPE_DIR && entry->node) {
		vfs_node_t *n = entry->node;
		st->st_dev = n->fs_type == VFS_FS_EXT2 ? 0x0801 : 0x0001;
		st->st_mode = n->mode ? n->mode : (S_IFDIR | 0755);
		st->st_size = (int64_t) n->size;
		st->st_ino =
		    n->ext2_ino ? n->ext2_ino : (uint64_t) (uintptr_t) n;
		st->st_nlink = 2;
		st->st_blksize = 4096;
		st->st_blocks = (int64_t) ((n->size + 511) / 512);
		st->st_atime_sec = n->atime;
		st->st_mtime_sec = n->mtime;
		st->st_ctime_sec = n->ctime;
	} else if (entry->type == FD_TYPE_PIPE_READ
		   || entry->type == FD_TYPE_PIPE_WRITE) {
		st->st_dev = 0;
		st->st_mode = S_IFIFO | 0600;
		st->st_ino = (uint64_t) (uintptr_t) entry;	/* Unique per pipe. */
		st->st_nlink = 1;
		st->st_blksize = 4096;
	} else if (entry->type == FD_TYPE_DEVNULL) {
		st->st_dev = 5;
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = 0x0103;	/* Major 1, minor 3. */
		st->st_ino = 3;
		st->st_nlink = 1;
		st->st_blksize = 4096;
	} else if (entry->type == FD_TYPE_DEVZERO) {
		st->st_dev = 5;
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = 0x0105;	/* Major 1, minor 5. */
		st->st_ino = 5;
		st->st_nlink = 1;
		st->st_blksize = 4096;
	} else if (entry->type == FD_TYPE_DEVRANDOM) {
		st->st_dev = 5;
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = 0x0108;	/* Major 1, minor 8. */
		st->st_ino = 8;
		st->st_nlink = 1;
		st->st_blksize = 4096;
	} else if (entry->type == FD_TYPE_PROC_IOMEM) {
		st->st_dev = 0;
		st->st_mode = S_IFREG | 0444;
		st->st_ino = 100;
		st->st_nlink = 1;
		st->st_blksize = 4096;
	}
}

void
fill_stat_from_node(struct linux_stat *st, vfs_node_t * node)
{
	kmemzero(st, sizeof(struct linux_stat));

	if (node->mode & S_IFMT) {
		st->st_mode = node->mode;
	} else if (node->type == VFS_NODE_DIR) {
		st->st_mode = S_IFDIR | (node->mode ? node->mode : 0755);
	} else {
		st->st_mode = S_IFREG | (node->mode ? node->mode : 0644);
	}
	st->st_dev = node->fs_type == VFS_FS_EXT2 ? 0x0801 : 0x0001;
	st->st_size = (int64_t) node->size;
	st->st_ino =
	    node->ext2_ino ? node->ext2_ino : (uint64_t) (uintptr_t) node;
	st->st_nlink = 1;
	st->st_blksize = 4096;
	st->st_blocks = (int64_t) ((node->size + 511) / 512);
	if (node->type == VFS_NODE_CHRDEV || node->type == VFS_NODE_BLKDEV)
		st->st_rdev = node->rdev;
	st->st_atime_sec = node->atime;
	st->st_mtime_sec = node->mtime;
	st->st_ctime_sec = node->ctime;
}

/*
 * Normalize a path in-place: collapse ".", "..", "//", remove trailing "/".
 * Input must be an absolute path starting with '/'.
 */
void
normalize_path(char *path)
{
	if (!path || path[0] != '/')
		return;

	/*
	 * Use a temporary buffer because in-place modification can overwrite
	 * the null terminator before the read pointer reaches it.
	 */
	char tmp[EMBER_PATH_MAX];
	char *out = tmp;
	char *in = path;

	*out++ = '/';

	while (*in) {
		while (*in == '/')
			in++;
		if (*in == '\0')
			break;

		char *comp = in;
		while (*in && *in != '/')
			in++;
		int len = (int)(in - comp);

		if (len == 1 && comp[0] == '.') {
			continue;
		}

		if (len == 2 && comp[0] == '.' && comp[1] == '.') {
			if (out > tmp + 1) {
				out--;
				while (out > tmp + 1 && *(out - 1) != '/')
					out--;
			}
			continue;
		}

		if ((out - tmp) + len + 1 >= EMBER_PATH_MAX)
			break;	/* Overflow guard. */
		for (int i = 0; i < len; i++)
			*out++ = comp[i];
		*out++ = '/';
	}

	/* Remove trailing slash unless path is just "/". */
	if (out > tmp + 1 && *(out - 1) == '/')
		out--;

	*out = '\0';

	/* Copy back. */
	char *s = tmp;
	char *d = path;
	while (*s)
		*d++ = *s++;
	*d = '\0';
}

/*
 * Resolve a relative path against the current process's cwd.
 * If chroot is active, prepend root_path.
 * Writes the resolved absolute path into kbuf.
 */
void
resolve_path(const char *path, char *kbuf, uint64_t kbuf_size)
{
	uint64_t di = 0;

	/* Empty path is invalid -- Linux returns ENOENT. */
	if (path[0] == '\0') {
		kbuf[0] = '\0';
		return;
	}

	if (path[0] == '/') {
		/* Already absolute -- just copy. */
		for (di = 0; di < kbuf_size - 1 && path[di]; di++)
			kbuf[di] = path[di];
		kbuf[di] = '\0';
	} else {
		/* Prepend cwd. */
		const char *cwd = current_proc ? current_proc->cwd : "/";
		uint64_t ci = 0;
		while (cwd[ci] && di < kbuf_size - 2) {
			kbuf[di++] = cwd[ci++];
		}
		/* Add separator if cwd is not just "/". */
		if (di > 1 || (di == 1 && kbuf[0] != '/')) {
			kbuf[di++] = '/';
		} else if (di == 1 && kbuf[0] == '/') {
			/* Cwd is "/", don't double up. */
		}
		uint64_t pi = 0;
		while (path[pi] && di < kbuf_size - 1) {
			kbuf[di++] = path[pi++];
		}
		kbuf[di] = '\0';
	}

	/* Normalize: collapse ".", "..", "//", trailing "/". */
	normalize_path(kbuf);

	/* If chroot is active, prepend root_path. */
	if (current_proc && current_proc->root_path[0]) {
		uint64_t rlen = 0;
		while (current_proc->root_path[rlen])
			rlen++;
		/* Remove trailing slash from root_path for concatenation. */
		while (rlen > 1 && current_proc->root_path[rlen - 1] == '/')
			rlen--;
		uint64_t plen = 0;
		while (kbuf[plen])
			plen++;
		/* Shift kbuf right by rlen to make room for root_path prefix. */
		if (rlen + plen < kbuf_size - 1) {
			uint64_t i = plen;
			while (1) {
				kbuf[rlen + i] = kbuf[i];
				if (i == 0)
					break;
				i--;
			}
			for (i = 0; i < rlen; i++)
				kbuf[i] = current_proc->root_path[i];
			kbuf[rlen + plen] = '\0';
		}
	}
}

int
is_devnull(const char *path)
{
	return kstreq(path, "/dev/null");
}

void
fill_chardev_stat(struct linux_stat *st, uint64_t rdev, uint64_t ino,
		  uint32_t mode_bits)
{
	kmemzero(st, sizeof(struct linux_stat));
	st->st_dev = 5;
	st->st_mode = S_IFCHR | mode_bits;
	st->st_rdev = rdev;
	st->st_ino = ino;
	st->st_nlink = 1;
	st->st_blksize = 4096;
}

void
fill_devnull_stat(struct linux_stat *st)
{
	fill_chardev_stat(st, 0x0103, 3, 0666);
}

int
is_devzero(const char *path)
{
	return kstreq(path, "/dev/zero");
}

void
fill_devzero_stat(struct linux_stat *st)
{
	fill_chardev_stat(st, 0x0105, 5, 0666);
}

int
is_devrandom(const char *path)
{
	return kstreq(path, "/dev/random") || kstreq(path, "/dev/urandom");
}

void
fill_devrandom_stat(struct linux_stat *st)
{
	fill_chardev_stat(st, 0x0108, 8, 0666);	/* Major 1, minor 8. */
}

int
is_devtty(const char *path)
{
	return kstreq(path, "/dev/tty");
}

void
fill_devtty_stat(struct linux_stat *st)
{
	fill_chardev_stat(st, 0x0500, 2, 0620);
}

int
is_devconsole(const char *path)
{
	return kstreq(path, "/dev/console");
}

int
is_proc_self_exe(const char *path)
{
	return kstreq(path, "/proc/self/exe");
}

int
is_proc_iomem(const char *path)
{
	return kstreq(path, "/proc/iomem");
}

void
print_num(uint64_t n)
{
	char buf[24];
	int pos = 0;
	if (n == 0) {
		buf[pos++] = '0';
	} else {
		char tmp[20];
		int tpos = 0;
		while (n > 0) {
			tmp[tpos++] = '0' + (n % 10);
			n /= 10;
		}
		for (int i = tpos - 1; i >= 0; i--)
			buf[pos++] = tmp[i];
	}
	buf[pos++] = '\n';
	buf[pos] = '\0';
	console_write(buf);
}
