/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_VFS_H
#define EMBER_VFS_H

#include <stdint.h>
#include "../boot_info.h"

typedef enum {
	VFS_NODE_FILE,
	VFS_NODE_DIR,
	VFS_NODE_SYMLINK,
	VFS_NODE_CHRDEV,
	VFS_NODE_BLKDEV
} vfs_node_type_t;

/* Filesystem type for each VFS node. */
#define VFS_FS_CPIO  0
#define VFS_FS_EXT2  1
#define VFS_FS_FAT32 2
#define VFS_FS_MEMFD 3

typedef struct vfs_node {
	char *path;
	vfs_node_type_t type;
	uint32_t mode;
	uint64_t size;
	const uint8_t *data;
	uint32_t ext2_ino;
	uint64_t rdev;
	uint32_t atime;		/* Cached access time (epoch seconds) */
	uint32_t mtime;		/* Cached modification time. */
	uint32_t ctime;		/* Cached change time. */
	uint32_t refcount;	/* Open FD references. */
	uint8_t unlinked;	/* Directory entry removed, defer block free. */
	uint8_t evicted;	/* Removed from VFS list; kfree when refcount==0. */
	uint8_t sym_followed;	/* Entry created by follow-symlinks lookup. */
	uint64_t last_access;	/* LRU timestamp. */
	uint8_t fs_type;	/* VFS_FS_CPIO / VFS_FS_EXT2 / VFS_FS_FAT32. */
	uint32_t fat_cluster;	/* FAT32: first cluster of file/dir. */
	uint32_t fat_dir_cluster;	/* FAT32: first cluster of parent directory. */
	struct vfs_node *next;
	struct vfs_node *hash_next;
} vfs_node_t;

void vfs_init_from_cpio(uint64_t initrd_phys_base, uint64_t initrd_size);
vfs_node_t *vfs_lookup(const char *path);
vfs_node_t *vfs_lookup_err(const char *path, int nofollow, int *out_err);
uint64_t vfs_read(vfs_node_t * node, uint64_t offset, void *buf, uint64_t len);
uint64_t vfs_write(vfs_node_t * node, uint64_t offset, const void *buf,
		   uint64_t len);
vfs_node_t *vfs_create(const char *path, uint16_t mode, int *err_out);
int vfs_unlink(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
int vfs_truncate_path(const char *path, uint64_t new_size);

vfs_node_t *vfs_head_get(void);
void vfs_ref(vfs_node_t * node);
void vfs_unref(vfs_node_t * node);
void vfs_set_size(vfs_node_t * node, uint64_t size);

int vfs_mkdir(const char *path, uint32_t mode);
int vfs_rmdir(const char *path);
int64_t vfs_getdents(vfs_node_t * dir, uint64_t offset, void *buf,
		     uint64_t buflen, uint64_t * new_offset);

int vfs_chmod(const char *path, uint32_t mode);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, uint32_t bufsize);
int vfs_mknod(const char *path, uint32_t mode, uint64_t dev);

#endif
