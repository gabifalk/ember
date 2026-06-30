/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/*
 * ext2_ops.c -- high-level VFS operations: create, mkdir, unlink, rename,
 * symlink, readlink, link, chmod, truncate, mknod, path_resolve_impl.
 */
#include <stdint.h>
#include "ember/syscall.h"
#include "ember/ext2.h"
#include "ember/ext2_internal.h"
#include "ember/blkcache.h"
#include "ember/time.h"
#include "ember/proc.h"
#include "ember/spinlock.h"

uint32_t
ext2_create(uint32_t dir_ino, const char *name, uint32_t name_len,
	    uint16_t mode)
{
	if (!ext2_ready)
		return 0;
	spin_lock(&ext2_lock);

	uint8_t ft;
	uint16_t type_bits = mode & EXT2_S_IFMT;
	if (type_bits == EXT2_S_IFDIR)
		ft = EXT2_FT_DIR;
	else if (type_bits == EXT2_S_IFCHR)
		ft = EXT2_FT_CHRDEV;
	else if (type_bits == EXT2_S_IFBLK)
		ft = EXT2_FT_BLKDEV;
	else if (type_bits == EXT2_S_IFLNK)
		ft = EXT2_FT_SYMLINK;
	else
		ft = EXT2_FT_REG_FILE;

	uint32_t new_ino = ext2_alloc_inode_inner();
	if (new_ino == 0) {
		spin_unlock(&ext2_lock);
		return 0;
	}

	ext2_inode_t new_inode;
	{
		uint8_t *p = (uint8_t *) & new_inode;
		for (uint64_t i = 0; i < sizeof(new_inode); i++)
			p[i] = 0;
	}
	new_inode.i_mode = mode;
	new_inode.i_links_count = 1;
	new_inode.i_atime = (uint32_t) kernel_time_sec();
	new_inode.i_mtime = new_inode.i_atime;
	new_inode.i_ctime = new_inode.i_atime;

	if (ext2_write_inode_inner(new_ino, &new_inode) < 0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return 0;
	}

	/* Add directory entry to parent. */
	if (ext2_add_dir_entry_existing(dir_ino, name, name_len, new_ino, ft) <
	    0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return 0;
	}

	spin_unlock(&ext2_lock);
	return new_ino;
}

int
ext2_truncate(uint32_t ino, uint32_t new_size)
{
	if (!ext2_ready || ino == 0)
		return -EINVAL;
	spin_lock(&ext2_lock);

	ext2_inode_t inode;
	if (ext2_read_inode_inner(ino, &inode) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}

	if (new_size == 0) {
		ext2_free_inode_blocks(&inode);
		inode.i_size = 0;
		inode.i_blocks = 0;
	} else if (new_size < inode.i_size) {
		uint32_t bs = ext2_block_size;
		uint32_t keep_blocks = (new_size + bs - 1) / bs;
		uint32_t sectors_per_block = bs / EXT2_SECTOR_SIZE;

		for (uint32_t i = keep_blocks; i < EXT2_NDIR_BLOCKS; i++) {
			if (inode.i_block[i]) {
				ext2_free_block_inner(inode.i_block[i]);
				inode.i_block[i] = 0;
				if (inode.i_blocks >= sectors_per_block)
					inode.i_blocks -= sectors_per_block;
			}
		}

		if (inode.i_block[EXT2_IND_BLOCK]
		    && keep_blocks <= EXT2_NDIR_BLOCKS + ext2_ptrs_per_block) {
			uint8_t ind_buf[BLKCACHE_MAX_BLOCK_SIZE];
			if (blkcache_read
			    (ext2_dev, inode.i_block[EXT2_IND_BLOCK],
			     ind_buf) == 0) {
				uint32_t *ind = (uint32_t *) ind_buf;
				uint32_t start =
				    keep_blocks >
				    EXT2_NDIR_BLOCKS ? keep_blocks -
				    EXT2_NDIR_BLOCKS : 0;
				for (uint32_t i = start;
				     i < ext2_ptrs_per_block; i++) {
					if (ind[i]) {
						ext2_free_block_inner(ind[i]);
						ind[i] = 0;
						if (inode.i_blocks >=
						    sectors_per_block)
							inode.i_blocks -=
							    sectors_per_block;
					}
				}
				if (start == 0) {
					ext2_free_block_inner(inode.
							      i_block
							      [EXT2_IND_BLOCK]);
					inode.i_block[EXT2_IND_BLOCK] = 0;
					if (inode.i_blocks >= sectors_per_block)
						inode.i_blocks -=
						    sectors_per_block;
				} else {
					blkcache_write(ext2_dev,
						       inode.
						       i_block[EXT2_IND_BLOCK],
						       ind_buf);
				}
			}
		}

		inode.i_size = new_size;
	} else {
		inode.i_size = new_size;
	}

	int ret = ext2_write_inode_inner(ino, &inode);
	spin_unlock(&ext2_lock);
	return ret;
}

void
ext2_free_inode_deferred(uint32_t ino)
{
	if (!ino)
		return;
	spin_lock(&ext2_lock);
	ext2_inode_t inode;
	if (ext2_read_inode_inner(ino, &inode) == 0 && inode.i_links_count == 0) {
		ext2_free_inode_blocks(&inode);
		inode.i_size = 0;
		inode.i_blocks = 0;
		ext2_write_inode_inner(ino, &inode);
		ext2_free_inode_inner(ino);
	}
	spin_unlock(&ext2_lock);
}

int
ext2_rename(uint32_t old_dir_ino, const char *old_name, uint32_t old_name_len,
	    uint32_t new_dir_ino, const char *new_name, uint32_t new_name_len)
{
	if (!ext2_ready)
		return -ENODEV;
	if (!old_name || !new_name)
		return -EINVAL;
	if (old_name_len == 0 || new_name_len == 0)
		return -EINVAL;
	if (old_name_len > 255 || new_name_len > 255)
		return -ENAMETOOLONG;

	spin_lock(&ext2_lock);

	uint32_t src_ino =
	    ext2_lookup_inner(old_dir_ino, old_name, old_name_len);
	if (src_ino == 0) {
		spin_unlock(&ext2_lock);
		return -ENOENT;
	}

	if (old_dir_ino == new_dir_ino && old_name_len == new_name_len) {
		int same = 1;
		for (uint32_t i = 0; i < old_name_len; i++) {
			if (old_name[i] != new_name[i]) {
				same = 0;
				break;
			}
		}
		if (same) {
			spin_unlock(&ext2_lock);
			return 0;
		}
	}

	ext2_inode_t src_inode;
	if (ext2_read_inode_inner(src_ino, &src_inode) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	int src_is_dir = (src_inode.i_mode & EXT2_S_IFDIR) != 0;

	/* Prevent cycles. */
	if (src_is_dir) {
		uint32_t check = new_dir_ino;
		for (int depth = 0; depth < 64; depth++) {
			if (check == src_ino) {
				spin_unlock(&ext2_lock);
				return -EINVAL;
			}
			if (check == EXT2_ROOT_INO)
				break;
			uint32_t parent = ext2_lookup_inner(check, "..", 2);
			if (parent == 0 || parent == check)
				break;
			check = parent;
		}
	}

	uint32_t dst_ino =
	    ext2_lookup_inner(new_dir_ino, new_name, new_name_len);
	if (dst_ino == src_ino && dst_ino != 0) {
		spin_unlock(&ext2_lock);
		return 0;
	}
	if (dst_ino != 0) {
		ext2_inode_t dst_inode;
		if (ext2_read_inode_inner(dst_ino, &dst_inode) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
		int dst_is_dir = (dst_inode.i_mode & EXT2_S_IFDIR) != 0;

		if (src_is_dir && !dst_is_dir) {
			spin_unlock(&ext2_lock);
			return -ENOTDIR;
		}
		if (!src_is_dir && dst_is_dir) {
			spin_unlock(&ext2_lock);
			return -EISDIR;
		}
		if (src_is_dir) {
			spin_unlock(&ext2_lock);
			return -EISDIR;
		}

		if (ext2_unlink_inner(new_dir_ino, new_name, new_name_len) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
	}

	uint8_t ftype = src_is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

	if (ext2_add_dir_entry_existing
	    (new_dir_ino, new_name, new_name_len, src_ino, ftype) < 0) {
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}

	if (src_is_dir && old_dir_ino != new_dir_ino) {
		if (ext2_set_dir_entry_inode(src_ino, "..", 2, new_dir_ino) < 0) {
			ext2_remove_dir_entry_only(new_dir_ino, new_name,
						   new_name_len);
			spin_unlock(&ext2_lock);
			return -EIO;
		}
	}

	if (ext2_remove_dir_entry_only(old_dir_ino, old_name, old_name_len) < 0) {
		ext2_unlink_inner(new_dir_ino, new_name, new_name_len);
		spin_unlock(&ext2_lock);
		return -EIO;
	}

	if (src_is_dir && old_dir_ino != new_dir_ino) {
		ext2_inode_t old_parent, new_parent;
		if (ext2_read_inode_inner(old_dir_ino, &old_parent) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
		if (ext2_read_inode_inner(new_dir_ino, &new_parent) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}

		if (old_parent.i_links_count > 0)
			old_parent.i_links_count--;
		new_parent.i_links_count++;

		if (ext2_write_inode_inner(old_dir_ino, &old_parent) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
		if (ext2_write_inode_inner(new_dir_ino, &new_parent) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
	}

	spin_unlock(&ext2_lock);
	return 0;
}

int
ext2_mkdir(uint32_t parent_ino, const char *name, uint32_t name_len,
	   uint16_t mode)
{
	if (!ext2_ready)
		return -ENODEV;
	spin_lock(&ext2_lock);
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	uint8_t ft = EXT2_FT_DIR;

	uint32_t new_ino = ext2_alloc_inode_inner();
	if (new_ino == 0) {
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}

	ext2_inode_t new_inode;
	{
		uint8_t *p = (uint8_t *) & new_inode;
		for (uint64_t i = 0; i < sizeof(new_inode); i++)
			p[i] = 0;
	}
	new_inode.i_mode = EXT2_S_IFDIR | (mode & 0777);
	new_inode.i_links_count = 1;
	new_inode.i_atime = (uint32_t) kernel_time_sec();
	new_inode.i_mtime = new_inode.i_atime;
	new_inode.i_ctime = new_inode.i_atime;

	if (ext2_write_inode_inner(new_ino, &new_inode) < 0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return -EIO;
	}

	if (ext2_add_dir_entry_existing(parent_ino, name, name_len, new_ino, ft)
	    < 0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}

	uint32_t blk_num = ext2_alloc_block_inner();
	if (blk_num == 0) {
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}

	{
		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		for (uint32_t i = 0; i < bs; i++)
			blk[i] = 0;

		ext2_dir_entry_t *dot = (ext2_dir_entry_t *) blk;
		dot->inode = new_ino;
		dot->rec_len = 12;
		dot->name_len = 1;
		dot->file_type = ext2_has_filetype ? EXT2_FT_DIR : 0;
		dot->name[0] = '.';

		ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *) (blk + 12);
		dotdot->inode = parent_ino;
		dotdot->rec_len = (uint16_t) (bs - 12);
		dotdot->name_len = 2;
		dotdot->file_type = ext2_has_filetype ? EXT2_FT_DIR : 0;
		dotdot->name[0] = '.';
		dotdot->name[1] = '.';

		blkcache_write(ext2_dev, blk_num, blk);
	}

	ext2_inode_t inode;
	if (ext2_read_inode_inner(new_ino, &inode) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	inode.i_block[0] = blk_num;
	inode.i_size = bs;
	inode.i_blocks = bs / EXT2_SECTOR_SIZE;
	inode.i_links_count = 2;
	if (ext2_write_inode_inner(new_ino, &inode) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}

	ext2_inode_t parent;
	if (ext2_read_inode_inner(parent_ino, &parent) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	parent.i_links_count++;
	if (ext2_write_inode_inner(parent_ino, &parent) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}

	spin_unlock(&ext2_lock);
	return 0;
}

/* ========== Symlink, chmod, hard link operations ========== */

int
ext2_chmod(uint32_t ino, uint16_t mode)
{
	if (!ext2_ready || ino == 0)
		return -EINVAL;
	spin_lock(&ext2_lock);
	ext2_inode_t inode;
	if (ext2_read_inode_inner(ino, &inode) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	inode.i_mode = (inode.i_mode & EXT2_S_IFMT) | (mode & 07777);
	int ret = ext2_write_inode_inner(ino, &inode);
	spin_unlock(&ext2_lock);
	return ret;
}

int
ext2_link(uint32_t dir_ino, const char *name, uint32_t name_len,
	  uint32_t target_ino)
{
	if (!ext2_ready)
		return -ENODEV;
	spin_lock(&ext2_lock);
	ext2_inode_t target;
	if (ext2_read_inode_inner(target_ino, &target) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	if ((target.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
		spin_unlock(&ext2_lock);
		return -EPERM;
	}
	uint8_t ft = EXT2_FT_REG_FILE;
	if ((target.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK)
		ft = EXT2_FT_SYMLINK;
	if (ext2_add_dir_entry_existing(dir_ino, name, name_len, target_ino, ft)
	    < 0) {
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}
	target.i_links_count++;
	int ret = ext2_write_inode_inner(target_ino, &target);
	spin_unlock(&ext2_lock);
	return ret;
}

int
ext2_symlink(uint32_t dir_ino, const char *name, uint32_t name_len,
	     const char *target)
{
	if (!ext2_ready)
		return -ENODEV;
	spin_lock(&ext2_lock);
	uint32_t tlen = 0;
	while (target[tlen])
		tlen++;
	uint32_t new_ino = ext2_alloc_inode_inner();
	if (new_ino == 0) {
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}
	ext2_inode_t inode;
	{
		uint8_t *p = (uint8_t *) & inode;
		for (uint64_t i = 0; i < sizeof(inode); i++)
			p[i] = 0;
	}
	inode.i_mode = EXT2_S_IFLNK | 0777;
	inode.i_links_count = 1;
	inode.i_size = tlen;
	if (tlen <= EXT2_SYMLINK_INLINE_MAX) {
		uint8_t *dst = (uint8_t *) inode.i_block;
		for (uint32_t i = 0; i < tlen; i++)
			dst[i] = (uint8_t) target[i];
		inode.i_blocks = 0;
	} else {
		uint32_t blk_num = ext2_alloc_block_inner();
		if (blk_num == 0) {
			ext2_free_inode_inner(new_ino);
			spin_unlock(&ext2_lock);
			return -ENOSPC;
		}
		{
			uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
			for (uint32_t i = 0; i < ext2_block_size; i++)
				blk[i] = 0;
			for (uint32_t i = 0; i < tlen; i++)
				blk[i] = (uint8_t) target[i];
			blkcache_write(ext2_dev, blk_num, blk);
		}
		inode.i_block[0] = blk_num;
		inode.i_blocks = ext2_block_size / EXT2_SECTOR_SIZE;
	}
	if (ext2_write_inode_inner(new_ino, &inode) < 0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	if (ext2_add_dir_entry_existing
	    (dir_ino, name, name_len, new_ino, EXT2_FT_SYMLINK) < 0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return -ENOSPC;
	}
	spin_unlock(&ext2_lock);
	return 0;
}

int
ext2_readlink(uint32_t ino, char *buf, uint32_t bufsize)
{
	if (!ext2_ready || ino == 0)
		return -EINVAL;
	spin_lock(&ext2_lock);
	ext2_inode_t inode;
	if (ext2_read_inode_inner(ino, &inode) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) {
		spin_unlock(&ext2_lock);
		return -EINVAL;
	}
	uint32_t tlen = inode.i_size;
	if (tlen >= bufsize)
		tlen = bufsize - 1;
	if (inode.i_blocks == 0) {
		uint8_t *src = (uint8_t *) inode.i_block;
		for (uint32_t i = 0; i < tlen; i++)
			buf[i] = (char)src[i];
	} else {
		uint32_t blk_num = inode.i_block[0];
		if (blk_num == 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, blk_num, blk) < 0) {
			spin_unlock(&ext2_lock);
			return -EIO;
		}
		for (uint32_t i = 0; i < tlen; i++)
			buf[i] = (char)blk[i];
	}
	buf[tlen] = '\0';
	spin_unlock(&ext2_lock);
	return (int)tlen;
}

/* Internal path resolve with symlink following and depth limit. */
uint32_t
ext2_path_resolve_impl(const char *path, int nofollow, int max_links)
{
	if (!ext2_ready)
		return 0;
	if (!path || path[0] != '/')
		return 0;

	char buf[EMBER_PATH_MAX];
	uint32_t bi = 0;
	for (bi = 0; bi < EMBER_PATH_MAX - 1 && path[bi]; bi++)
		buf[bi] = path[bi];
	buf[bi] = '\0';

	int links_followed = 0;
	uint32_t ino;

 restart:
	ino = EXT2_ROOT_INO;
	const char *p = buf + 1;

	while (*p) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		const char *start = p;
		while (*p && *p != '/')
			p++;
		uint32_t comp_len = (uint32_t) (p - start);

		ino = ext2_lookup_inner(ino, start, comp_len);
		if (ino == 0)
			return 0;

		ext2_inode_t ei;
		if (ext2_read_inode_inner(ino, &ei) < 0)
			return 0;
		if ((ei.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
			int is_last = (*p == '\0'
				       || (*p == '/' && *(p + 1) == '\0'));
			if (is_last && nofollow)
				return ino;

			if (++links_followed > max_links)
				return UINT32_MAX;	/* ELOOP. */

			char target[EMBER_PATH_MAX];
			uint32_t tlen = ei.i_size;
			if (tlen >= sizeof(target))
				tlen = sizeof(target) - 1;
			if (ei.i_blocks == 0) {
				uint8_t *src = (uint8_t *) ei.i_block;
				for (uint32_t i = 0; i < tlen; i++)
					target[i] = (char)src[i];
			} else {
				uint32_t blk_num = ei.i_block[0];
				if (blk_num == 0)
					return 0;
				uint8_t data[BLKCACHE_MAX_BLOCK_SIZE];
				if (blkcache_read(ext2_dev, blk_num, data) < 0)
					return 0;
				for (uint32_t i = 0; i < tlen; i++)
					target[i] = (char)data[i];
			}
			target[tlen] = '\0';

			char newbuf[EMBER_PATH_MAX];
			uint32_t ni = 0;
			uint32_t np_max = EMBER_PATH_MAX - 2;
			if (target[0] == '/') {
				for (uint32_t i = 0; i < tlen && ni < np_max;
				     i++)
					newbuf[ni++] = target[i];
			} else {
				uint32_t parent_len = (uint32_t) (start - buf);
				while (parent_len > 1
				       && buf[parent_len - 1] == '/')
					parent_len--;
				if (parent_len == 0)
					parent_len = 1;
				for (uint32_t i = 0;
				     i < parent_len && ni < np_max; i++)
					newbuf[ni++] = buf[i];
				if (newbuf[ni - 1] != '/')
					newbuf[ni++] = '/';
				for (uint32_t i = 0; i < tlen && ni < np_max;
				     i++)
					newbuf[ni++] = target[i];
			}
			if (*p) {
				if (newbuf[ni - 1] != '/')
					newbuf[ni++] = '/';
				while (*p && ni < np_max)
					newbuf[ni++] = *p++;
			}
			newbuf[ni] = '\0';

			for (uint32_t i = 0; i <= ni; i++)
				buf[i] = newbuf[i];
			goto restart;
		}
	}

	return ino;
}

uint32_t
ext2_mknod(uint32_t dir_ino, const char *name, uint32_t name_len, uint16_t mode,
	   uint32_t dev)
{
	if (!ext2_ready)
		return 0;
	spin_lock(&ext2_lock);

	uint8_t ft;
	uint16_t type_bits = mode & EXT2_S_IFMT;
	if (type_bits == EXT2_S_IFDIR)
		ft = EXT2_FT_DIR;
	else if (type_bits == EXT2_S_IFCHR)
		ft = EXT2_FT_CHRDEV;
	else if (type_bits == EXT2_S_IFBLK)
		ft = EXT2_FT_BLKDEV;
	else if (type_bits == EXT2_S_IFLNK)
		ft = EXT2_FT_SYMLINK;
	else
		ft = EXT2_FT_REG_FILE;

	uint32_t new_ino = ext2_alloc_inode_inner();
	if (new_ino == 0) {
		spin_unlock(&ext2_lock);
		return 0;
	}

	ext2_inode_t new_inode;
	{
		uint8_t *p = (uint8_t *) & new_inode;
		for (uint64_t i = 0; i < sizeof(new_inode); i++)
			p[i] = 0;
	}
	new_inode.i_mode = mode;
	new_inode.i_links_count = 1;
	new_inode.i_atime = (uint32_t) kernel_time_sec();
	new_inode.i_mtime = new_inode.i_atime;
	new_inode.i_ctime = new_inode.i_atime;

	if (ext2_write_inode_inner(new_ino, &new_inode) < 0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return 0;
	}

	if (ext2_add_dir_entry_existing(dir_ino, name, name_len, new_ino, ft) <
	    0) {
		ext2_free_inode_inner(new_ino);
		spin_unlock(&ext2_lock);
		return 0;
	}

	ext2_inode_t inode;
	if (ext2_read_inode_inner(new_ino, &inode) < 0) {
		spin_unlock(&ext2_lock);
		return 0;
	}
	if (dev <= 0xFFFF) {
		inode.i_block[0] = dev;
		inode.i_block[1] = 0;
	} else {
		inode.i_block[0] = 0;
		inode.i_block[1] = dev;
	}
	ext2_write_inode_inner(new_ino, &inode);
	spin_unlock(&ext2_lock);
	return new_ino;
}
