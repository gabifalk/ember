/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* ext2_block.c -- block mapping, allocation/free, free_inode_blocks. */
#include <stdint.h>
#include "ember/syscall.h"
#include "ember/ext2.h"
#include "ember/ext2_internal.h"
#include "ember/blkcache.h"
#include "ember/spinlock.h"

/* Resolve a logical block number to a physical block number. */
uint32_t
ext2_block_map(ext2_inode_t * inode, uint32_t logical)
{
	/* Direct blocks: 0..EXT2_NDIR_BLOCKS-1. */
	if (logical < EXT2_NDIR_BLOCKS)
		return inode->i_block[logical];

	uint8_t buf[BLKCACHE_MAX_BLOCK_SIZE];
	uint32_t blkno;

	logical -= EXT2_NDIR_BLOCKS;

	/* Single indirect. */
	if (logical < ext2_ptrs_per_block) {
		blkno = inode->i_block[EXT2_IND_BLOCK];
		if (blkno == 0)
			return 0;
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			return 0;
		return ((const uint32_t *)buf)[logical];
	}

	logical -= ext2_ptrs_per_block;

	/* Double indirect. */
	if (logical < ext2_ptrs_per_block * ext2_ptrs_per_block) {
		blkno = inode->i_block[EXT2_DIND_BLOCK];
		if (blkno == 0)
			return 0;
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			return 0;

		uint32_t idx1 = logical / ext2_ptrs_per_block;
		uint32_t idx2 = logical % ext2_ptrs_per_block;

		blkno = ((const uint32_t *)buf)[idx1];
		if (blkno == 0)
			return 0;
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			return 0;
		return ((const uint32_t *)buf)[idx2];
	}

	logical -= ext2_ptrs_per_block * ext2_ptrs_per_block;

	/* Triple indirect. */
	{
		blkno = inode->i_block[EXT2_TIND_BLOCK];
		if (blkno == 0)
			return 0;
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			return 0;

		uint32_t idx1 =
		    logical / (ext2_ptrs_per_block * ext2_ptrs_per_block);
		uint32_t rem =
		    logical % (ext2_ptrs_per_block * ext2_ptrs_per_block);
		uint32_t idx2 = rem / ext2_ptrs_per_block;
		uint32_t idx3 = rem % ext2_ptrs_per_block;

		blkno = ((const uint32_t *)buf)[idx1];
		if (blkno == 0)
			return 0;
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			return 0;

		blkno = ((const uint32_t *)buf)[idx2];
		if (blkno == 0)
			return 0;
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			return 0;
		return ((const uint32_t *)buf)[idx3];
	}
}

/* Zero-init a newly allocated indirect block via the block cache. */
static void
ext2_zero_block(uint32_t blkno, uint8_t * buf, uint32_t bs)
{
	for (uint32_t i = 0; i < bs; i++)
		buf[i] = 0;
	blkcache_write(ext2_dev, blkno, buf);
}

/*
 * Set a block mapping for a logical block in an inode. Allocates indirect blocks as needed.
 * Returns the number of newly allocated metadata (indirect) blocks on success (>= 0),
 * or a negative errno on failure. The caller must add (retval * bs/512) to i_blocks.
 */
int
ext2_block_set(ext2_inode_t * inode, uint32_t logical, uint32_t phys_block)
{
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */
	int meta_blocks = 0;

	if (logical < EXT2_NDIR_BLOCKS) {
		inode->i_block[logical] = phys_block;
		return 0;
	}

	uint8_t buf[BLKCACHE_MAX_BLOCK_SIZE];

	logical -= EXT2_NDIR_BLOCKS;

	/* Single indirect. */
	if (logical < ext2_ptrs_per_block) {
		if (inode->i_block[EXT2_IND_BLOCK] == 0) {
			uint32_t ind = ext2_alloc_block_inner();
			if (ind == 0)
				return -ENOSPC;
			inode->i_block[EXT2_IND_BLOCK] = ind;
			ext2_zero_block(ind, buf, bs);
			meta_blocks++;
		}
		if (blkcache_read(ext2_dev, inode->i_block[EXT2_IND_BLOCK], buf)
		    < 0)
			return -EIO;
		((uint32_t *) buf)[logical] = phys_block;
		blkcache_write(ext2_dev, inode->i_block[EXT2_IND_BLOCK], buf);
		return meta_blocks;
	}

	logical -= ext2_ptrs_per_block;

	/* Double indirect. */
	if (logical < ext2_ptrs_per_block * ext2_ptrs_per_block) {
		if (inode->i_block[EXT2_DIND_BLOCK] == 0) {
			uint32_t dind = ext2_alloc_block_inner();
			if (dind == 0)
				return -ENOSPC;
			inode->i_block[EXT2_DIND_BLOCK] = dind;
			ext2_zero_block(dind, buf, bs);
			meta_blocks++;
		}

		uint32_t idx1 = logical / ext2_ptrs_per_block;
		uint32_t idx2 = logical % ext2_ptrs_per_block;

		if (blkcache_read
		    (ext2_dev, inode->i_block[EXT2_DIND_BLOCK], buf) < 0)
			return -EIO;
		uint32_t ind_block = ((uint32_t *) buf)[idx1];

		if (ind_block == 0) {
			ind_block = ext2_alloc_block_inner();
			if (ind_block == 0)
				return -ENOSPC;
			((uint32_t *) buf)[idx1] = ind_block;
			blkcache_write(ext2_dev,
				       inode->i_block[EXT2_DIND_BLOCK], buf);
			ext2_zero_block(ind_block, buf, bs);
			meta_blocks++;
		}

		if (blkcache_read(ext2_dev, ind_block, buf) < 0)
			return -EIO;
		((uint32_t *) buf)[idx2] = phys_block;
		blkcache_write(ext2_dev, ind_block, buf);
		return meta_blocks;
	}

	logical -= ext2_ptrs_per_block * ext2_ptrs_per_block;

	/* Triple indirect. */
	{
		if (inode->i_block[EXT2_TIND_BLOCK] == 0) {
			uint32_t tind = ext2_alloc_block_inner();
			if (tind == 0)
				return -ENOSPC;
			inode->i_block[EXT2_TIND_BLOCK] = tind;
			ext2_zero_block(tind, buf, bs);
			meta_blocks++;
		}

		uint32_t idx1 =
		    logical / (ext2_ptrs_per_block * ext2_ptrs_per_block);
		uint32_t rem =
		    logical % (ext2_ptrs_per_block * ext2_ptrs_per_block);
		uint32_t idx2 = rem / ext2_ptrs_per_block;
		uint32_t idx3 = rem % ext2_ptrs_per_block;

		if (blkcache_read
		    (ext2_dev, inode->i_block[EXT2_TIND_BLOCK], buf) < 0)
			return -EIO;
		uint32_t dind_block = ((uint32_t *) buf)[idx1];

		if (dind_block == 0) {
			dind_block = ext2_alloc_block_inner();
			if (dind_block == 0)
				return -ENOSPC;
			/* Re-read tind to update it. */
			if (blkcache_read
			    (ext2_dev, inode->i_block[EXT2_TIND_BLOCK],
			     buf) < 0)
				return -EIO;
			((uint32_t *) buf)[idx1] = dind_block;
			blkcache_write(ext2_dev,
				       inode->i_block[EXT2_TIND_BLOCK], buf);
			ext2_zero_block(dind_block, buf, bs);
			meta_blocks++;
		}

		if (blkcache_read(ext2_dev, dind_block, buf) < 0)
			return -EIO;
		uint32_t ind_block = ((uint32_t *) buf)[idx2];

		if (ind_block == 0) {
			ind_block = ext2_alloc_block_inner();
			if (ind_block == 0)
				return -ENOSPC;
			/* Re-read dind to update it. */
			if (blkcache_read(ext2_dev, dind_block, buf) < 0)
				return -EIO;
			((uint32_t *) buf)[idx2] = ind_block;
			blkcache_write(ext2_dev, dind_block, buf);
			ext2_zero_block(ind_block, buf, bs);
			meta_blocks++;
		}

		if (blkcache_read(ext2_dev, ind_block, buf) < 0)
			return -EIO;
		((uint32_t *) buf)[idx3] = phys_block;
		blkcache_write(ext2_dev, ind_block, buf);
		return meta_blocks;
	}
}

uint32_t
ext2_alloc_block_inner(void)
{
	if (!ext2_ready)
		return 0;

	for (uint32_t g = 0; g < ext2_groups_count; g++) {
		if (ext2_group_descs[g].bg_free_blocks_count == 0)
			continue;

		uint32_t bitmap_block = ext2_group_descs[g].bg_block_bitmap;
		uint8_t bmp_buf[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, bitmap_block, bmp_buf) < 0)
			return 0;

		uint32_t blocks_in_group = ext2_sb.s_blocks_per_group;
		if (g == ext2_groups_count - 1) {
			uint32_t remaining =
			    ext2_sb.s_blocks_count -
			    g * ext2_sb.s_blocks_per_group;
			if (remaining < blocks_in_group)
				blocks_in_group = remaining;
		}

		for (uint32_t bit = 0; bit < blocks_in_group; bit++) {
			uint32_t byte_idx = bit / 8;
			uint8_t bit_mask = (uint8_t) (1 << (bit % 8));
			if (!(bmp_buf[byte_idx] & bit_mask)) {
				bmp_buf[byte_idx] |= bit_mask;
				blkcache_write(ext2_dev, bitmap_block, bmp_buf);

				ext2_group_descs[g].bg_free_blocks_count--;
				ext2_write_group_descs();

				ext2_sb.s_free_blocks_count--;
				ext2_write_superblock();

				uint32_t block_num =
				    g * ext2_sb.s_blocks_per_group + bit +
				    ext2_sb.s_first_data_block;
				return block_num;
			}
		}
	}
	return 0;
}

/*
 * Public locking wrappers: the _inner variants are called by other ext2 code
 * that already holds ext2_lock (e.g. ext2_block_set, ext2_free_inode_blocks,
 * ext2_create, ext2_mkdir).
 */
uint32_t
ext2_alloc_block(void)
{
	spin_lock(&ext2_lock);
	uint32_t ret = ext2_alloc_block_inner();
	spin_unlock(&ext2_lock);
	return ret;
}

void
ext2_free_block_inner(uint32_t block_num)
{
	if (!ext2_ready || block_num == 0)
		return;

	uint32_t adjusted = block_num - ext2_sb.s_first_data_block;
	uint32_t g = adjusted / ext2_sb.s_blocks_per_group;
	uint32_t bit = adjusted % ext2_sb.s_blocks_per_group;

	if (g >= ext2_groups_count)
		return;

	uint32_t bitmap_block = ext2_group_descs[g].bg_block_bitmap;
	uint8_t bmp_buf[BLKCACHE_MAX_BLOCK_SIZE];
	if (blkcache_read(ext2_dev, bitmap_block, bmp_buf) < 0)
		return;

	uint32_t byte_idx = bit / 8;
	uint8_t bit_mask = (uint8_t) (1 << (bit % 8));
	bmp_buf[byte_idx] &= ~bit_mask;
	blkcache_write(ext2_dev, bitmap_block, bmp_buf);

	blkcache_discard(ext2_dev, block_num);

	ext2_group_descs[g].bg_free_blocks_count++;
	ext2_write_group_descs();

	ext2_sb.s_free_blocks_count++;
	ext2_write_superblock();
}

void
ext2_free_block(uint32_t block_num)
{
	spin_lock(&ext2_lock);
	ext2_free_block_inner(block_num);
	spin_unlock(&ext2_lock);
}

/*
 * Free all leaf blocks reachable through a single-indirect block, then the block itself.
 * Uses caller-provided buf (one block size) as scratch.
 */
static void
ext2_free_indirect(uint32_t blkno, uint8_t * buf)
{
	if (blkno == 0)
		return;
	if (blkcache_read(ext2_dev, blkno, buf) < 0)
		goto free_self;
	const uint32_t *ptrs = (const uint32_t *)buf;
	for (uint32_t i = 0; i < ext2_ptrs_per_block; i++) {
		if (ptrs[i])
			ext2_free_block_inner(ptrs[i]);
	}
 free_self:
	ext2_free_block_inner(blkno);
}

/*
 * Free all single-indirect trees under a double-indirect block, then the block itself.
 * Re-reads the dind block from cache each iteration so buf can be reused by ext2_free_indirect.
 */
static void
ext2_free_dindirect(uint32_t blkno, uint8_t * buf)
{
	if (blkno == 0)
		return;
	for (uint32_t i = 0; i < ext2_ptrs_per_block; i++) {
		if (blkcache_read(ext2_dev, blkno, buf) < 0)
			break;
		uint32_t ind = ((const uint32_t *)buf)[i];
		if (ind)
			ext2_free_indirect(ind, buf);
	}
	ext2_free_block_inner(blkno);
}

/* Free all data blocks of an inode (direct + indirect) */
void
ext2_free_inode_blocks(ext2_inode_t * inode)
{
	/* Fast symlinks store the target string in i_block[] -- not block numbers. */
	if ((inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK
	    && inode->i_blocks == 0) {
		for (int i = 0; i < EXT2_N_BLOCKS; i++)
			inode->i_block[i] = 0;
		return;
	}

	uint8_t buf[BLKCACHE_MAX_BLOCK_SIZE];

	/* Free direct blocks. */
	for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
		if (inode->i_block[i]) {
			ext2_free_block_inner(inode->i_block[i]);
			inode->i_block[i] = 0;
		}
	}

	/* Free single indirect. */
	if (inode->i_block[EXT2_IND_BLOCK]) {
		ext2_free_indirect(inode->i_block[EXT2_IND_BLOCK], buf);
		inode->i_block[EXT2_IND_BLOCK] = 0;
	}

	/* Free double indirect. */
	if (inode->i_block[EXT2_DIND_BLOCK]) {
		ext2_free_dindirect(inode->i_block[EXT2_DIND_BLOCK], buf);
		inode->i_block[EXT2_DIND_BLOCK] = 0;
	}

	/*
	 * Free triple indirect -- iterate tind entries, re-reading each time since
	 * ext2_free_dindirect reuses buf/ptrs.
	 */
	if (inode->i_block[EXT2_TIND_BLOCK]) {
		uint32_t tind_blk = inode->i_block[EXT2_TIND_BLOCK];
		for (uint32_t i = 0; i < ext2_ptrs_per_block; i++) {
			if (blkcache_read(ext2_dev, tind_blk, buf) < 0)
				break;
			uint32_t dblk = ((const uint32_t *)buf)[i];
			if (dblk)
				ext2_free_dindirect(dblk, buf);
		}
		ext2_free_block_inner(tind_blk);
		inode->i_block[EXT2_TIND_BLOCK] = 0;
	}

	inode->i_blocks = 0;
	inode->i_size = 0;
}
