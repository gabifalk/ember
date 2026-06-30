/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* ext2_inode.c -- inode read/write/alloc/free. */
#include <stdint.h>
#include "ember/syscall.h"
#include "ember/ext2.h"
#include "ember/ext2_internal.h"
#include "ember/blkcache.h"
#include "ember/time.h"
#include "ember/spinlock.h"

int
ext2_read_inode_inner(uint32_t ino, ext2_inode_t * out)
{
	if (!ext2_ready || ino == 0)
		return -EINVAL;

	uint32_t group = (ino - 1) / ext2_sb.s_inodes_per_group;
	uint32_t index = (ino - 1) % ext2_sb.s_inodes_per_group;

	if (group >= ext2_groups_count)
		return -EINVAL;

	uint32_t inode_table_block = ext2_group_descs[group].bg_inode_table;
	uint32_t byte_off = index * ext2_inode_size;
	uint32_t blk_in_table = byte_off / ext2_block_size;
	uint32_t off_in_block = byte_off % ext2_block_size;

	uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
	if (blkcache_read(ext2_dev, inode_table_block + blk_in_table, blk) < 0)
		return -EIO;

	const uint8_t *src = blk + off_in_block;
	uint8_t *dst = (uint8_t *) out;
	for (uint64_t i = 0; i < sizeof(ext2_inode_t); i++)
		dst[i] = src[i];

	return 0;
}

/*
 * Public locking wrappers: the _inner variants are called by other ext2 code
 * that already holds ext2_lock, so we need separate locked entry points for
 * callers outside the ext2 subsystem (VFS, syscall layer, etc.).
 */
int
ext2_read_inode(uint32_t ino, ext2_inode_t * out)
{
	if (!ext2_ready || ino == 0)
		return -EINVAL;
	spin_lock(&ext2_lock);
	int ret = ext2_read_inode_inner(ino, out);
	spin_unlock(&ext2_lock);
	return ret;
}

int
ext2_write_inode_inner(uint32_t ino, ext2_inode_t * inode)
{
	if (!ext2_ready || ino == 0)
		return -EINVAL;

	uint32_t group = (ino - 1) / ext2_sb.s_inodes_per_group;
	uint32_t index = (ino - 1) % ext2_sb.s_inodes_per_group;

	if (group >= ext2_groups_count)
		return -EINVAL;

	uint32_t inode_table_block = ext2_group_descs[group].bg_inode_table;
	uint32_t byte_off = index * ext2_inode_size;
	uint32_t blk_in_table = byte_off / ext2_block_size;
	uint32_t off_in_block = byte_off % ext2_block_size;

	uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
	if (blkcache_read(ext2_dev, inode_table_block + blk_in_table, blk) < 0)
		return -EIO;

	uint8_t *src = (uint8_t *) inode;
	uint8_t *dst = blk + off_in_block;
	for (uint64_t i = 0; i < sizeof(ext2_inode_t); i++)
		dst[i] = src[i];

	blkcache_write(ext2_dev, inode_table_block + blk_in_table, blk);
	return 0;
}

int
ext2_write_inode(uint32_t ino, ext2_inode_t * inode)
{
	spin_lock(&ext2_lock);
	int ret = ext2_write_inode_inner(ino, inode);
	spin_unlock(&ext2_lock);
	return ret;
}

uint32_t
ext2_alloc_inode_inner(void)
{
	if (!ext2_ready)
		return 0;

	for (uint32_t g = 0; g < ext2_groups_count; g++) {
		if (ext2_group_descs[g].bg_free_inodes_count == 0)
			continue;

		uint32_t bitmap_block = ext2_group_descs[g].bg_inode_bitmap;
		uint8_t bmp_buf[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, bitmap_block, bmp_buf) < 0)
			return 0;

		uint32_t inodes_in_group = ext2_sb.s_inodes_per_group;

		for (uint32_t bit = 0; bit < inodes_in_group; bit++) {
			uint32_t byte_idx = bit / 8;
			uint8_t bit_mask = (uint8_t) (1 << (bit % 8));
			if (!(bmp_buf[byte_idx] & bit_mask)) {
				bmp_buf[byte_idx] |= bit_mask;
				blkcache_write(ext2_dev, bitmap_block, bmp_buf);

				ext2_group_descs[g].bg_free_inodes_count--;
				ext2_write_group_descs();

				ext2_sb.s_free_inodes_count--;
				ext2_write_superblock();

				uint32_t ino =
				    g * ext2_sb.s_inodes_per_group + bit + 1;
				return ino;
			}
		}
	}
	return 0;
}

uint32_t
ext2_alloc_inode(void)
{
	spin_lock(&ext2_lock);
	uint32_t ret = ext2_alloc_inode_inner();
	spin_unlock(&ext2_lock);
	return ret;
}

void
ext2_free_inode_inner(uint32_t ino)
{
	if (!ext2_ready || ino == 0)
		return;

	/* Set deletion time on the inode. */
	ext2_inode_t di;
	if (ext2_read_inode_inner(ino, &di) == 0) {
		di.i_dtime = (uint32_t) kernel_time_sec();
		ext2_write_inode_inner(ino, &di);
	}

	uint32_t g = (ino - 1) / ext2_sb.s_inodes_per_group;
	uint32_t bit = (ino - 1) % ext2_sb.s_inodes_per_group;

	if (g >= ext2_groups_count)
		return;

	uint32_t bitmap_block = ext2_group_descs[g].bg_inode_bitmap;
	uint8_t bmp_buf[BLKCACHE_MAX_BLOCK_SIZE];
	if (blkcache_read(ext2_dev, bitmap_block, bmp_buf) < 0)
		return;

	uint32_t byte_idx = bit / 8;
	uint8_t bit_mask = (uint8_t) (1 << (bit % 8));
	bmp_buf[byte_idx] &= ~bit_mask;
	blkcache_write(ext2_dev, bitmap_block, bmp_buf);

	ext2_group_descs[g].bg_free_inodes_count++;
	ext2_write_group_descs();

	ext2_sb.s_free_inodes_count++;
	ext2_write_superblock();
}

void
ext2_free_inode(uint32_t ino)
{
	spin_lock(&ext2_lock);
	ext2_free_inode_inner(ino);
	spin_unlock(&ext2_lock);
}
