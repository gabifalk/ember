/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* ext2_io.c -- data read/write using block mapping + block cache. */
#include <stdint.h>
#include "ember/syscall.h"
#include "ember/ext2.h"
#include "ember/ext2_internal.h"
#include "ember/blkcache.h"
#include "ember/time.h"
#include "ember/spinlock.h"

int
ext2_read_data(ext2_inode_t * inode, uint64_t offset, void *buf, uint64_t len)
{
	if (!ext2_ready)
		return 0;
	spin_lock(&ext2_lock);
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */
	uint32_t file_size = inode->i_size;
	if (offset >= file_size) {
		spin_unlock(&ext2_lock);
		return 0;
	}
	if (offset + len > file_size)
		len = file_size - offset;

	uint8_t *dst = (uint8_t *) buf;
	uint64_t done = 0;

	while (done < len) {
		uint32_t logical_block = (uint32_t) ((offset + done) / bs);
		uint32_t off_in_block = (uint32_t) ((offset + done) % bs);
		uint32_t chunk = bs - off_in_block;
		if (chunk > len - done)
			chunk = (uint32_t) (len - done);

		uint32_t phys_block = ext2_block_map(inode, logical_block);
		if (phys_block == 0) {
			/* Sparse block -- zero fill. */
			for (uint32_t i = 0; i < chunk; i++)
				dst[done + i] = 0;
		} else {
			uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
			if (blkcache_read(ext2_dev, phys_block, blk) < 0) {
				spin_unlock(&ext2_lock);
				return -EIO;
			}
			for (uint32_t i = 0; i < chunk; i++)
				dst[done + i] = blk[off_in_block + i];
		}

		done += chunk;
	}

	spin_unlock(&ext2_lock);
	return (int)done;
}

int
ext2_write_data(ext2_inode_t * inode, uint32_t ino, uint64_t offset,
		const void *buf, uint64_t len)
{
	if (!ext2_ready)
		return -ENODEV;
	spin_lock(&ext2_lock);
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	const uint8_t *src = (const uint8_t *)buf;
	uint64_t done = 0;
	uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];

	while (done < len) {
		uint32_t logical_block = (uint32_t) ((offset + done) / bs);
		uint32_t off_in_block = (uint32_t) ((offset + done) % bs);
		uint32_t chunk = bs - off_in_block;
		if (chunk > len - done)
			chunk = (uint32_t) (len - done);

		uint32_t phys_block = ext2_block_map(inode, logical_block);
		if (phys_block == 0) {
			phys_block = ext2_alloc_block_inner();
			if (phys_block == 0) {
				int ret = (done > 0) ? (int)done : -ENOSPC;
				spin_unlock(&ext2_lock);
				return ret;
			}
			int meta =
			    ext2_block_set(inode, logical_block, phys_block);
			if (meta < 0) {
				int ret = (done > 0) ? (int)done : -EIO;
				spin_unlock(&ext2_lock);
				return ret;
			}
			for (uint32_t i = 0; i < bs; i++)
				blk[i] = 0;
			blkcache_write(ext2_dev, phys_block, blk);
			inode->i_blocks += (1 + meta) * (bs / EXT2_SECTOR_SIZE);
		}

		if (blkcache_read(ext2_dev, phys_block, blk) < 0) {
			int ret = (done > 0) ? (int)done : -EIO;
			spin_unlock(&ext2_lock);
			return ret;
		}

		for (uint32_t i = 0; i < chunk; i++)
			blk[off_in_block + i] = src[done + i];

		blkcache_write(ext2_dev, phys_block, blk);
		done += chunk;
	}

	if (offset + done > inode->i_size)
		inode->i_size = (uint32_t) (offset + done);

	inode->i_mtime = (uint32_t) kernel_time_sec();
	inode->i_ctime = inode->i_mtime;

	ext2_write_inode_inner(ino, inode);

	spin_unlock(&ext2_lock);
	return (int)done;
}
