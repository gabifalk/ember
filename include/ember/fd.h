/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_FD_H
#define EMBER_FD_H

#include <stdint.h>
#include "ember/vfs.h"
#include "ember/spinlock.h"

#define MAX_FDS 256
#define MAX_FILE_DESCS 512
#define FD_CLOEXEC 1

#define FD_TYPE_NONE       0
#define FD_TYPE_CONSOLE    1
#define FD_TYPE_FILE       2
#define FD_TYPE_PIPE_READ  3
#define FD_TYPE_PIPE_WRITE 4
#define FD_TYPE_DIR        5
#define FD_TYPE_DEVNULL    6
#define FD_TYPE_DEVZERO    7
#define FD_TYPE_DEVRANDOM  8
#define FD_TYPE_EPOLL      9
#define FD_TYPE_PROC_IOMEM 10

/* Forward declaration. */
struct pipe;

/* Shared file description -- one per open(), shared across dup/fork. */
typedef struct file_desc {
	uint8_t type;		/* FD_TYPE_* */
	uint32_t flags;		/* O_RDONLY, O_APPEND, O_NONBLOCK (NOT O_CLOEXEC) */
	uint64_t offset;	/* Current file position. */
	vfs_node_t *node;	/* VFS node (NULL for console) */
	struct pipe *pipe;	/* Pipe (for FD_TYPE_PIPE_READ/WRITE) */
	uint32_t refcount;	/* 0 = Free slot. */
	spinlock_t lock;	/* Protects offset and flags across concurrent access. */
} file_desc_t;

/* Per-fd entry in process fd table. */
typedef struct {
	file_desc_t *desc;	/* NULL = free fd slot. */
	uint32_t fd_flags;	/* Per-fd: FD_CLOEXEC only. */
} fd_entry_t;

/* File description pool management. */
file_desc_t *file_desc_alloc(void);
void file_desc_ref(file_desc_t * d);
void file_desc_unref(file_desc_t * d);

void fd_init(void);
int fd_alloc(void);		/* Returns lowest free fd, or -1. */
void fd_free(int fd);
fd_entry_t *fd_get(int fd);	/* Returns NULL if invalid or free. */
fd_entry_t *fd_get_raw(int fd);	/* Returns entry even if free. */

#endif
