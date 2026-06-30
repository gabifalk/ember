/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Ext2.c -- superblock, group descriptors, mount/init, path resolution. */
#include <stdint.h>
#include "ember/syscall.h"
#include "ember/ext2.h"
#include "ember/ext2_internal.h"
#include "ember/blkcache.h"
#include "ember/blkdev.h"
#include "ember/console.h"
#include "ember/heap.h"
#include "ember/proc.h"
#include "ember/spinlock.h"

/* ========== Shared state (externed via ext2_internal.h) ========== */
spinlock_t ext2_lock = SPINLOCK_INIT;
ext2_superblock_t ext2_sb;
ext2_group_desc_t *ext2_group_descs;
uint32_t ext2_block_size;
uint32_t ext2_groups_count;
uint16_t ext2_inode_size;
int ext2_ready;
int ext2_dev = -1;
uint32_t ext2_ptrs_per_block;
int ext2_has_filetype;

int
ext2_is_ready(void)
{
	return ext2_ready;
}

int
ext2_get_dev(void)
{
	return ext2_dev;
}

/* Fill a Linux struct statfs (120 bytes on x86_64) from ext2 superblock data. */
int
ext2_statfs(void *buf)
{
	if (!ext2_ready)
		return -ENODEV;
	spin_lock(&ext2_lock);
	uint8_t *p = (uint8_t *) buf;
	for (int i = 0; i < 120; i++)
		p[i] = 0;
	uint64_t *q = (uint64_t *) buf;
	q[0] = 0xEF53;		/* f_type. */
	q[1] = (uint64_t) ext2_block_size;	/* f_bsize. */
	q[2] = (uint64_t) ext2_sb.s_blocks_count;	/* f_blocks. */
	q[3] = (uint64_t) ext2_sb.s_free_blocks_count;	/* f_bfree. */
	q[4] = (uint64_t) ext2_sb.s_free_blocks_count;	/* f_bavail. */
	q[5] = (uint64_t) ext2_sb.s_inodes_count;	/* f_files. */
	q[6] = (uint64_t) ext2_sb.s_free_inodes_count;	/* f_ffree. */
	q[8] = 255;		/* f_namelen. */
	q[9] = (uint64_t) ext2_block_size;	/* f_frsize. */
	spin_unlock(&ext2_lock);
	return 0;
}

int
ext2_init(int dev)
{
	ext2_ready = 0;
	ext2_dev = dev;

	/* Superblock is always at byte offset 1024 = LBA 2, regardless of block size. */
	uint8_t sb_raw[1024];
	if (blkdev_read(dev, 2, 2, sb_raw) < 0) {
		console_write("ext2: failed to read superblock\n");
		return -EIO;
	}

	{
		const uint8_t *src = (const uint8_t *)sb_raw;
		uint8_t *dst = (uint8_t *) & ext2_sb;
		for (uint64_t i = 0; i < sizeof(ext2_sb); i++)
			dst[i] = src[i];
	}

	if (ext2_sb.s_magic != EXT2_MAGIC) {
		console_write("ext2: bad magic\n");
		return -EINVAL;
	}

	ext2_block_size = 1024U << ext2_sb.s_log_block_size;
	if (ext2_block_size != 1024 && ext2_block_size != 2048
	    && ext2_block_size != 4096) {
		console_write("ext2: unsupported block size\n");
		return -EINVAL;
	}

	blkcache_set_block_size(ext2_dev, ext2_block_size);

	ext2_inode_size =
	    (ext2_sb.s_rev_level >= 1) ? ext2_sb.s_inode_size : 128;
	ext2_ptrs_per_block = ext2_block_size / 4;
	ext2_has_filetype =
	    (ext2_sb.
	     s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? 1 : 0;

	ext2_groups_count =
	    (ext2_sb.s_blocks_count + ext2_sb.s_blocks_per_group -
	     1) / ext2_sb.s_blocks_per_group;

	uint32_t gdt_block = (ext2_block_size == 1024) ? 2 : 1;
	if (ext2_groups_count > EXT2_MAX_BLOCK_GROUPS)
		ext2_groups_count = EXT2_MAX_BLOCK_GROUPS;

	ext2_group_descs =
	    kmalloc(ext2_groups_count * sizeof(ext2_group_desc_t));
	if (!ext2_group_descs) {
		console_write("ext2: failed to alloc group descriptors\n");
		return -ENOMEM;
	}
	{
		uint32_t descs_per_block =
		    ext2_block_size / sizeof(ext2_group_desc_t);
		uint32_t gdt_blocks =
		    (ext2_groups_count + descs_per_block - 1) / descs_per_block;
		uint8_t *d = (uint8_t *) ext2_group_descs;
		uint32_t remaining = ext2_groups_count;
		for (uint32_t b = 0; b < gdt_blocks; b++) {
			uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
			if (blkcache_read(ext2_dev, gdt_block + b, blk) < 0) {
				console_write
				    ("ext2: failed to read GDT block\n");
				return -EIO;
			}
			uint32_t n =
			    (remaining <
			     descs_per_block) ? remaining : descs_per_block;
			for (uint32_t i = 0; i < n * sizeof(ext2_group_desc_t);
			     i++)
				d[i] = blk[i];
			d += n * sizeof(ext2_group_desc_t);
			remaining -= n;
		}
	}

	ext2_ready = 1;
	return 0;
}

/* ========== Superblock / group descriptor persistence ========== */

void
ext2_write_superblock(void)
{
	uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
	uint8_t *src = (uint8_t *) & ext2_sb;
	if (ext2_block_size == 1024) {
		if (blkcache_read(ext2_dev, 1, blk) < 0)
			return;
		for (uint64_t i = 0; i < sizeof(ext2_sb); i++)
			blk[i] = src[i];
		blkcache_write(ext2_dev, 1, blk);
	} else {
		if (blkcache_read(ext2_dev, 0, blk) < 0)
			return;
		for (uint64_t i = 0; i < sizeof(ext2_sb); i++)
			blk[1024 + i] = src[i];
		blkcache_write(ext2_dev, 0, blk);
	}
}

void
ext2_write_group_descs(void)
{
	uint32_t gdt_block = (ext2_block_size == 1024) ? 2 : 1;
	uint32_t descs_per_block = ext2_block_size / sizeof(ext2_group_desc_t);
	uint32_t gdt_blocks =
	    (ext2_groups_count + descs_per_block - 1) / descs_per_block;
	uint8_t *src = (uint8_t *) ext2_group_descs;
	uint32_t remaining = ext2_groups_count;
	for (uint32_t b = 0; b < gdt_blocks; b++) {
		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, gdt_block + b, blk) < 0)
			return;
		uint32_t n =
		    (remaining < descs_per_block) ? remaining : descs_per_block;
		for (uint32_t i = 0; i < n * sizeof(ext2_group_desc_t); i++)
			blk[i] = src[i];
		blkcache_write(ext2_dev, gdt_block + b, blk);
		src += n * sizeof(ext2_group_desc_t);
		remaining -= n;
	}
}

/*
 * ========== Path resolution ==========
 *
 * Public locking wrappers: ext2_path_resolve_impl is called internally by
 * ext2_readlink (via symlink resolution) where ext2_lock is already held.
 */

uint32_t
ext2_path_resolve(const char *path)
{
	spin_lock(&ext2_lock);
	uint32_t ret = ext2_path_resolve_impl(path, 0, 8);
	spin_unlock(&ext2_lock);
	return ret;
}

uint32_t
ext2_path_resolve_nofollow(const char *path)
{
	spin_lock(&ext2_lock);
	uint32_t ret = ext2_path_resolve_impl(path, 1, 8);
	spin_unlock(&ext2_lock);
	return ret;
}
