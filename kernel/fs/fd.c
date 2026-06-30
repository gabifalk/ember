/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "ember/fd.h"
#include "ember/vfs.h"
#include "ember/heap.h"
#include "ember/proc.h"
#include "ember/pipe.h"
#include "ember/spinlock.h"
#include "ember/sched.h"
#include "ember/bug.h"

/* Global pool of shared file descriptions. */
static file_desc_t file_descs[MAX_FILE_DESCS];
static spinlock_t file_desc_lock = SPINLOCK_INIT;

file_desc_t *
file_desc_alloc(void)
{
	uint64_t flags = spin_lock_irqsave(&file_desc_lock);
	for (int i = 0; i < MAX_FILE_DESCS; i++) {
		if (file_descs[i].refcount == 0) {
			file_descs[i].refcount = 1;
			file_descs[i].type = FD_TYPE_NONE;
			file_descs[i].flags = 0;
			file_descs[i].offset = 0;
			file_descs[i].node = 0;
			file_descs[i].pipe = 0;
			spin_init(&file_descs[i].lock);
			spin_unlock_irqrestore(&file_desc_lock, flags);
			return &file_descs[i];
		}
	}
	spin_unlock_irqrestore(&file_desc_lock, flags);
	return 0;
}

void
file_desc_ref(file_desc_t * d)
{
	if (!d)
		return;
	uint64_t flags = spin_lock_irqsave(&file_desc_lock);
	d->refcount++;
	spin_unlock_irqrestore(&file_desc_lock, flags);
}

void
file_desc_unref(file_desc_t * d)
{
	if (!d)
		return;
	/* Decrement refcount under lock; capture cleanup info. */
	uint8_t do_cleanup = 0;
	uint8_t old_type = 0;
	vfs_node_t *old_node = 0;
	struct pipe *old_pipe = 0;

	uint64_t flags = spin_lock_irqsave(&file_desc_lock);
	if (d->refcount > 0)
		d->refcount--;
	if (d->refcount == 0) {
		do_cleanup = 1;
		old_type = d->type;
		old_node = d->node;
		old_pipe = d->pipe;
		d->type = FD_TYPE_NONE;
		d->flags = 0;
		d->offset = 0;
		d->node = 0;
		d->pipe = 0;
	}
	spin_unlock_irqrestore(&file_desc_lock, flags);

	/* Do cleanup outside of lock to avoid deadlock with pipe/sched locks. */
	if (do_cleanup) {
		if (old_type == FD_TYPE_PROC_IOMEM) {
			if (old_node)
				kfree((void *)old_node);
		} else if (old_node && old_node->fs_type == VFS_FS_MEMFD) {
			/* Memfd: free data buffer, path, and node (not in VFS cache) */
			if (old_node->refcount > 0)
				old_node->refcount--;
			if (old_node->refcount == 0) {
				if (old_node->data)
					kfree((void *)old_node->data);
				if (old_node->path)
					kfree(old_node->path);
				kfree(old_node);
			}
		} else {
			vfs_unref(old_node);
		}
		if (old_type == FD_TYPE_PIPE_READ && old_pipe)
			pipe_close_read((pipe_t *) old_pipe);
		if (old_type == FD_TYPE_PIPE_WRITE && old_pipe)
			pipe_close_write((pipe_t *) old_pipe);
	}
}

/* Early boot fd table, used before proc_init sets up current_proc. */
static fd_entry_t boot_fd_table[MAX_FDS];

static fd_entry_t *
fd_table_ptr(void)
{
	if (current_proc) {
		BUG_ON((uint64_t) (uintptr_t) current_proc <
		       0xffffffff80000000ULL);
		return current_proc->fds;
	}
	return boot_fd_table;
}

void
fd_init(void)
{
	fd_entry_t *tbl = fd_table_ptr();
	for (int i = 0; i < MAX_FDS; i++) {
		tbl[i].desc = 0;
		tbl[i].fd_flags = 0;
	}
	/* Pre-open stdin/stdout/stderr as console. */
	for (int i = 0; i < 3; i++) {
		file_desc_t *d = file_desc_alloc();
		if (d) {
			d->type = FD_TYPE_CONSOLE;
			tbl[i].desc = d;
			tbl[i].fd_flags = 0;
		}
	}
}

int
fd_alloc(void)
{
	fd_entry_t *tbl = fd_table_ptr();
	for (int i = 0; i < MAX_FDS; i++) {
		if (tbl[i].desc == 0)
			return i;
	}
	return -1;
}

void
fd_free(int fd)
{
	if (fd < 0 || fd >= MAX_FDS)
		return;
	fd_entry_t *tbl = fd_table_ptr();
	tbl[fd].desc = 0;
	tbl[fd].fd_flags = 0;
}

fd_entry_t *
fd_get(int fd)
{
	if (fd < 0 || fd >= MAX_FDS)
		return 0;
	fd_entry_t *tbl = fd_table_ptr();
	if (tbl[fd].desc == 0)
		return 0;
	return &tbl[fd];
}

fd_entry_t *
fd_get_raw(int fd)
{
	if (fd < 0 || fd >= MAX_FDS)
		return 0;
	fd_entry_t *tbl = fd_table_ptr();
	return &tbl[fd];
}
