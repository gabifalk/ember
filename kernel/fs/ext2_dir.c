/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* ext2_dir.c -- directory operations: lookup, add/remove dirent, unlink, getdents, dentry cache. */
#include <stdint.h>
#include "ember/syscall.h"
#include "ember/ext2.h"
#include "ember/ext2_internal.h"
#include "ember/blkcache.h"
#include "ember/blkdev.h"
#include "ember/console.h"
#include "ember/spinlock.h"
#include "ember/heap.h"

/* Return 1 if the first @len bytes of @a and @b are identical, 0 otherwise. */
static int
ext2_name_eq(const char *a, const char *b, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* ========== Directory entry name cache (dentry cache) ========== */
#define DENTRY_CACHE_SIZE 512
#define DENTRY_NAME_MAX   64

typedef struct {
	uint32_t parent_ino;
	uint32_t child_ino;
	uint8_t valid;
	char name[DENTRY_NAME_MAX];
} dentry_cache_entry_t;

static dentry_cache_entry_t dentry_cache[DENTRY_CACHE_SIZE];

static uint32_t
dentry_hash(uint32_t parent_ino, const char *name, uint32_t name_len)
{
	uint32_t h = parent_ino * 2654435761u;
	for (uint32_t i = 0; i < name_len; i++)
		h = h * 31 + (uint8_t) name[i];
	return h % DENTRY_CACHE_SIZE;
}

static uint32_t
dentry_cache_lookup(uint32_t parent_ino, const char *name, uint32_t name_len)
{
	if (name_len == 0 || name_len >= DENTRY_NAME_MAX)
		return 0;
	uint32_t idx = dentry_hash(parent_ino, name, name_len);
	dentry_cache_entry_t *e = &dentry_cache[idx];
	if (!e->valid || e->parent_ino != parent_ino)
		return 0;
	if (!ext2_name_eq(e->name, name, name_len))
		return 0;
	if (e->name[name_len] != '\0')
		return 0;
	return e->child_ino;
}

static void
dentry_cache_insert(uint32_t parent_ino, const char *name, uint32_t name_len,
		    uint32_t child_ino)
{
	if (name_len == 0 || name_len >= DENTRY_NAME_MAX || child_ino == 0)
		return;
	uint32_t idx = dentry_hash(parent_ino, name, name_len);
	dentry_cache_entry_t *e = &dentry_cache[idx];
	e->parent_ino = parent_ino;
	e->child_ino = child_ino;
	for (uint32_t i = 0; i < name_len; i++)
		e->name[i] = name[i];
	e->name[name_len] = '\0';
	e->valid = 1;
}

static void
dentry_cache_invalidate(uint32_t parent_ino, const char *name,
			uint32_t name_len)
{
	if (name_len == 0 || name_len >= DENTRY_NAME_MAX)
		return;
	uint32_t idx = dentry_hash(parent_ino, name, name_len);
	dentry_cache_entry_t *e = &dentry_cache[idx];
	if (!e->valid || e->parent_ino != parent_ino)
		return;
	if (!ext2_name_eq(e->name, name, name_len))
		return;
	if (e->name[name_len] != '\0')
		return;
	e->valid = 0;
}

/* ========== Lookup ========== */

uint32_t
ext2_lookup_inner(uint32_t dir_ino, const char *name, uint32_t name_len)
{
	uint32_t cached = dentry_cache_lookup(dir_ino, name, name_len);
	if (cached)
		return cached;

	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0)
		return 0;
	if (!(dir.i_mode & EXT2_S_IFDIR))
		return 0;

	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */
	uint32_t offset = 0;
	uint32_t dir_size = dir.i_size;

	while (offset < dir_size) {
		uint32_t logical = offset / bs;
		uint32_t off_in = offset % bs;
		uint32_t phys = ext2_block_map(&dir, logical);
		if (phys == 0)
			break;

		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, phys, blk) < 0)
			break;

		uint32_t pos = off_in;
		while (pos < bs && (offset - off_in + pos) < dir_size) {
			const ext2_dir_entry_t *de =
			    (const ext2_dir_entry_t *)(blk + pos);
			if (de->rec_len == 0)
				break;

			if (de->inode != 0 && de->name_len == (uint8_t) name_len
			    && ext2_name_eq(de->name, name, name_len)) {
				dentry_cache_insert(dir_ino, name, name_len,
						    de->inode);
				return de->inode;
			}

			pos += de->rec_len;
		}

		offset = (offset - off_in) + bs;
	}

	return 0;
}

/*
 * Public locking wrapper: ext2_lookup_inner is called by other ext2 code that
 * already holds ext2_lock (e.g. ext2_path_resolve_impl, ext2_rename).
 */
uint32_t
ext2_lookup(uint32_t dir_ino, const char *name, uint32_t name_len)
{
	spin_lock(&ext2_lock);
	uint32_t ret = ext2_lookup_inner(dir_ino, name, name_len);
	spin_unlock(&ext2_lock);
	return ret;
}

/* ========== Add / remove directory entries ========== */

int
ext2_add_dir_entry_existing(uint32_t dir_ino, const char *name,
			    uint32_t name_len, uint32_t target_ino,
			    uint8_t file_type)
{
	if (!ext2_ready)
		return -ENODEV;
	if (!ext2_has_filetype)
		file_type = 0;
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0)
		return -EIO;
	if (!(dir.i_mode & EXT2_S_IFDIR))
		return -ENOTDIR;

	uint32_t needed = EXT2_DIR_REC_LEN_BASE + name_len;
	needed = (needed + 3) & ~3u;

	uint32_t dir_size = dir.i_size;
	uint32_t num_blocks = (dir_size + bs - 1) / bs;
	uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];

	for (uint32_t b = 0; b < num_blocks; b++) {
		uint32_t phys = ext2_block_map(&dir, b);
		if (phys == 0)
			continue;

		if (blkcache_read(ext2_dev, phys, blk) < 0)
			continue;

		uint32_t pos = 0;
		while (pos < bs) {
			ext2_dir_entry_t *de = (ext2_dir_entry_t *) (blk + pos);
			if (de->rec_len == 0)
				break;

			uint32_t actual_size =
			    EXT2_DIR_REC_LEN_BASE + de->name_len;
			actual_size = (actual_size + 3) & ~3u;
			if (de->inode == 0)
				actual_size = 0;

			uint32_t free_space = de->rec_len - actual_size;
			if (free_space >= needed) {
				if (de->inode != 0) {
					uint16_t old_rec_len = de->rec_len;
					de->rec_len = (uint16_t) actual_size;

					ext2_dir_entry_t *new_de =
					    (ext2_dir_entry_t *) (blk + pos +
								  actual_size);
					new_de->inode = target_ino;
					new_de->rec_len =
					    (uint16_t) (old_rec_len -
							actual_size);
					new_de->name_len = (uint8_t) name_len;
					new_de->file_type = file_type;
					for (uint32_t i = 0; i < name_len; i++)
						new_de->name[i] = name[i];
				} else {
					de->inode = target_ino;
					de->name_len = (uint8_t) name_len;
					de->file_type = file_type;
					for (uint32_t i = 0; i < name_len; i++)
						de->name[i] = name[i];
				}
				blkcache_write(ext2_dev, phys, blk);
				dentry_cache_insert(dir_ino, name, name_len,
						    target_ino);
				return 0;
			}

			pos += de->rec_len;
		}
	}

	uint32_t new_blk = ext2_alloc_block_inner();
	if (new_blk == 0)
		return -ENOSPC;

	uint32_t new_logical = num_blocks;
	int meta = ext2_block_set(&dir, new_logical, new_blk);
	if (meta < 0) {
		ext2_free_block_inner(new_blk);
		return -EIO;
	}

	dir.i_size += bs;
	dir.i_blocks += (1 + meta) * (bs / EXT2_SECTOR_SIZE);

	for (uint32_t i = 0; i < bs; i++)
		blk[i] = 0;

	ext2_dir_entry_t *de = (ext2_dir_entry_t *) blk;
	de->inode = target_ino;
	de->rec_len = (uint16_t) bs;
	de->name_len = (uint8_t) name_len;
	de->file_type = file_type;
	for (uint32_t i = 0; i < name_len; i++)
		de->name[i] = name[i];

	blkcache_write(ext2_dev, new_blk, blk);
	ext2_write_inode_inner(dir_ino, &dir);
	dentry_cache_insert(dir_ino, name, name_len, target_ino);
	return 0;
}

int
ext2_remove_dir_entry_only(uint32_t dir_ino, const char *name,
			   uint32_t name_len)
{
	if (!ext2_ready)
		return -ENODEV;
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0)
		return -EIO;
	if (!(dir.i_mode & EXT2_S_IFDIR))
		return -ENOTDIR;

	uint32_t dir_size = dir.i_size;
	uint32_t num_blocks = (dir_size + bs - 1) / bs;
	for (uint32_t b = 0; b < num_blocks; b++) {
		uint32_t phys = ext2_block_map(&dir, b);
		if (phys == 0)
			continue;

		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, phys, blk) < 0)
			continue;

		uint32_t pos = 0;
		uint32_t prev_pos = 0;
		int have_prev = 0;
		while (pos < bs) {
			ext2_dir_entry_t *de = (ext2_dir_entry_t *) (blk + pos);
			if (de->rec_len == 0)
				break;

			if (de->inode != 0 && de->name_len == (uint8_t) name_len
			    && ext2_name_eq(de->name, name, name_len)) {
				dentry_cache_invalidate(dir_ino, name,
							name_len);
				if (have_prev) {
					ext2_dir_entry_t *prev =
					    (ext2_dir_entry_t *) (blk +
								  prev_pos);
					prev->rec_len += de->rec_len;
				} else {
					de->inode = 0;
				}
				blkcache_write(ext2_dev, phys, blk);
				return 0;
			}

			prev_pos = pos;
			have_prev = 1;
			pos += de->rec_len;
		}
	}

	return -ENOENT;
}

int
ext2_set_dir_entry_inode(uint32_t dir_ino, const char *name, uint32_t name_len,
			 uint32_t new_ino)
{
	if (!ext2_ready)
		return -ENODEV;
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0)
		return -EIO;
	if (!(dir.i_mode & EXT2_S_IFDIR))
		return -ENOTDIR;

	uint32_t dir_size = dir.i_size;
	uint32_t num_blocks = (dir_size + bs - 1) / bs;
	for (uint32_t b = 0; b < num_blocks; b++) {
		uint32_t phys = ext2_block_map(&dir, b);
		if (phys == 0)
			continue;

		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, phys, blk) < 0)
			continue;

		uint32_t pos = 0;
		while (pos < bs) {
			ext2_dir_entry_t *de = (ext2_dir_entry_t *) (blk + pos);
			if (de->rec_len == 0)
				break;
			if (de->inode != 0 && de->name_len == (uint8_t) name_len
			    && ext2_name_eq(de->name, name, name_len)) {
				de->inode = new_ino;
				blkcache_write(ext2_dev, phys, blk);
				dentry_cache_invalidate(dir_ino, name,
							name_len);
				dentry_cache_insert(dir_ino, name, name_len,
						    new_ino);
				return 0;
			}
			pos += de->rec_len;
		}
	}
	return -ENOENT;
}

/*
 * ========== Unlink ==========
 *
 * Two-level locking pattern: ext2_unlink_inner / ext2_unlink_inner_flags are
 * called from ext2_rename and other ext2 code that already holds ext2_lock.
 * The public ext2_unlink / ext2_unlink_keep_blocks acquire the lock for
 * external callers (VFS, syscall layer).
 */

static int
ext2_unlink_inner_flags(uint32_t dir_ino, const char *name, uint32_t name_len,
			int free_blocks)
{
	if (!ext2_ready)
		return -ENODEV;
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0)
		return -EIO;
	if (!(dir.i_mode & EXT2_S_IFDIR))
		return -ENOTDIR;

	uint32_t dir_size = dir.i_size;
	uint32_t found_ino = 0;

	uint32_t num_blocks = (dir_size + bs - 1) / bs;
	for (uint32_t b = 0; b < num_blocks; b++) {
		uint32_t phys = ext2_block_map(&dir, b);
		if (phys == 0)
			continue;

		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, phys, blk) < 0)
			continue;

		uint32_t pos = 0;
		uint32_t prev_pos = 0;
		int have_prev = 0;

		while (pos < bs) {
			ext2_dir_entry_t *de = (ext2_dir_entry_t *) (blk + pos);
			if (de->rec_len == 0)
				break;

			if (de->inode != 0 && de->name_len == (uint8_t) name_len
			    && ext2_name_eq(de->name, name, name_len)) {
				found_ino = de->inode;
				dentry_cache_invalidate(dir_ino, name,
							name_len);
				if (have_prev) {
					ext2_dir_entry_t *prev =
					    (ext2_dir_entry_t *) (blk +
								  prev_pos);
					prev->rec_len += de->rec_len;
				} else {
					de->inode = 0;
				}
				blkcache_write(ext2_dev, phys, blk);
				goto found;
			}

			prev_pos = pos;
			have_prev = 1;
			pos += de->rec_len;
		}
	}

	return -ENOENT;		/* Not found. */

 found:
	if (found_ino == 0)
		return -ENOENT;

	ext2_inode_t inode;
	if (ext2_read_inode_inner(found_ino, &inode) < 0)
		return -EIO;

	if (inode.i_links_count > 0)
		inode.i_links_count--;

	int is_dir = (inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
	int should_free =
	    is_dir ? (inode.i_links_count <= 1) : (inode.i_links_count == 0);

	if (should_free && free_blocks) {
		inode.i_links_count = 0;
		ext2_free_inode_blocks(&inode);
		inode.i_size = 0;
		inode.i_blocks = 0;
		ext2_write_inode_inner(found_ino, &inode);
		ext2_free_inode_inner(found_ino);
	} else {
		ext2_write_inode_inner(found_ino, &inode);
	}

	return 0;
}

int
ext2_unlink_inner(uint32_t dir_ino, const char *name, uint32_t name_len)
{
	return ext2_unlink_inner_flags(dir_ino, name, name_len, 1);
}

int
ext2_unlink(uint32_t dir_ino, const char *name, uint32_t name_len)
{
	spin_lock(&ext2_lock);
	int ret = ext2_unlink_inner(dir_ino, name, name_len);
	spin_unlock(&ext2_lock);
	return ret;
}

int
ext2_unlink_keep_blocks(uint32_t dir_ino, const char *name, uint32_t name_len)
{
	spin_lock(&ext2_lock);
	int ret = ext2_unlink_inner_flags(dir_ino, name, name_len, 0);
	spin_unlock(&ext2_lock);
	return ret;
}

/* ========== Dir_is_empty / getdents ========== */

int
ext2_dir_is_empty(uint32_t dir_ino)
{
	spin_lock(&ext2_lock);
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */
	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0) {
		spin_unlock(&ext2_lock);
		return 0;
	}
	if (!(dir.i_mode & EXT2_S_IFDIR)) {
		spin_unlock(&ext2_lock);
		return 0;
	}

	uint32_t dir_size = dir.i_size;
	uint32_t num_blocks = (dir_size + bs - 1) / bs;
	for (uint32_t b = 0; b < num_blocks; b++) {
		uint32_t phys = ext2_block_map(&dir, b);
		if (phys == 0)
			continue;
		uint8_t blk[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, phys, blk) < 0) {
			spin_unlock(&ext2_lock);
			return 0;
		}

		uint32_t pos = 0;
		while (pos < bs) {
			const ext2_dir_entry_t *de =
			    (const ext2_dir_entry_t *)(blk + pos);
			if (de->rec_len == 0)
				break;
			if (de->inode != 0) {
				int is_dot = (de->name_len == 1
					      && de->name[0] == '.');
				int is_dotdot = (de->name_len == 2
						 && de->name[0] == '.'
						 && de->name[1] == '.');
				if (!is_dot && !is_dotdot) {
					spin_unlock(&ext2_lock);
					return 0;
				}
			}
			pos += de->rec_len;
		}
	}
	spin_unlock(&ext2_lock);
	return 1;
}

/* linux_dirent64 structure for getdents64. */
struct linux_dirent64 {
	uint64_t d_ino;
	uint64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];		/* Null-terminated. */
};

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK    10
#define DT_CHR     2
#define DT_BLK     6

int
ext2_getdents(uint32_t dir_ino, uint64_t offset, void *buf, uint64_t buflen,
	      uint64_t * new_offset)
{
	if (!ext2_ready)
		return -ENODEV;
	spin_lock(&ext2_lock);
	uint32_t bs = ext2_block_size;	/* Local copy -- workaround for tcc relocation bug. */

	ext2_inode_t dir;
	if (ext2_read_inode_inner(dir_ino, &dir) < 0) {
		spin_unlock(&ext2_lock);
		return -EIO;
	}
	if (!(dir.i_mode & EXT2_S_IFDIR)) {
		console_write("ext2_getdents: ENOTDIR ino=");
		console_hex64((uint64_t) dir_ino);
		console_write(" i_mode=");
		console_hex64((uint64_t) dir.i_mode);
		console_write(" i_size=");
		console_hex64((uint64_t) dir.i_size);
		console_write(" i_links=");
		console_hex64((uint64_t) dir.i_links_count);
		console_write("\n");

		/* Diagnostic: re-read inode block directly from disk, bypassing cache. */
		{
			uint32_t group =
			    (dir_ino - 1) / ext2_sb.s_inodes_per_group;
			uint32_t index =
			    (dir_ino - 1) % ext2_sb.s_inodes_per_group;
			uint32_t inode_table_block =
			    ext2_group_descs[group].bg_inode_table;
			uint32_t byte_off = index * ext2_inode_size;
			uint32_t blk_in_table = byte_off / bs;
			uint32_t off_in_block = byte_off % bs;
			uint32_t disk_block = inode_table_block + blk_in_table;
			uint32_t spb = bs / 512;
			uint32_t lba = disk_block * spb;
			uint8_t *raw = (uint8_t *) kmalloc(bs);
			if (!raw)
				goto diag_done;
			console_write("  disk_block=");
			console_hex64((uint64_t) disk_block);
			console_write(" off_in_block=");
			console_hex64((uint64_t) off_in_block);
			console_write("\n");
			if (blkdev_read(ext2_dev, lba, (uint8_t) spb, raw) == 0) {
				const ext2_inode_t *disk_ino =
				    (const ext2_inode_t *)(raw + off_in_block);
				console_write("  disk i_mode=");
				console_hex64((uint64_t) disk_ino->i_mode);
				console_write(" i_size=");
				console_hex64((uint64_t) disk_ino->i_size);
				console_write(" i_links=");
				console_hex64((uint64_t) disk_ino->
					      i_links_count);
				console_write("\n");
				/* Compare cached vs disk block data. */
				uint8_t *cached = (uint8_t *) kmalloc(bs);
				if (!cached) {
					kfree(raw);
					goto diag_done;
				}
				if (blkcache_read(ext2_dev, disk_block, cached)
				    == 0) {
					int differs = 0;
					for (uint32_t i = 0; i < bs; i++) {
						if (cached[i] != raw[i]) {
							differs = 1;
							break;
						}
					}
					console_write("  cache_vs_disk=");
					console_write(differs ? "MISMATCH\n" :
						      "match\n");
					if (differs) {
						/* Show first differing offset and a few bytes around the inode. */
						for (uint32_t i = 0; i < bs;
						     i++) {
							if (cached[i] != raw[i]) {
								console_write
								    ("  first_diff_at=");
								console_hex64((uint64_t) i);
								console_write
								    (" cached=");
								console_hex64((uint64_t) cached[i]);
								console_write
								    (" disk=");
								console_hex64((uint64_t) raw[i]);
								console_write
								    ("\n");
								break;
							}
						}
					}
				}
				kfree(cached);
			}
			kfree(raw);
		}
 diag_done:

		spin_unlock(&ext2_lock);
		return -ENOTDIR;
	}

	uint32_t dir_size = dir.i_size;
	uint8_t *out = (uint8_t *) buf;
	uint64_t written = 0;
	uint64_t pos = offset;

	while (pos < dir_size) {
		uint32_t logical = (uint32_t) (pos / bs);
		uint32_t off_in = (uint32_t) (pos % bs);
		uint32_t phys = ext2_block_map(&dir, logical);
		if (phys == 0)
			break;

		uint8_t blkdata[BLKCACHE_MAX_BLOCK_SIZE];
		if (blkcache_read(ext2_dev, phys, blkdata) < 0)
			break;

		uint32_t bpos = off_in;
		while (bpos < bs && (logical * bs + bpos) < dir_size) {
			const ext2_dir_entry_t *de =
			    (const ext2_dir_entry_t *)(blkdata + bpos);
			if (de->rec_len == 0)
				break;

			uint64_t next_pos =
			    (uint64_t) logical * bs + bpos + de->rec_len;

			if (de->inode != 0 && de->name_len > 0
			    && de->name[0] != '\0') {
				uint32_t name_len = de->name_len;
				uint32_t reclen = 19 + name_len + 1;
				reclen = (reclen + 7) & ~7u;

				if (written + reclen > buflen) {
					if (new_offset)
						*new_offset = pos;
					spin_unlock(&ext2_lock);
					return (int)written;
				}

				struct linux_dirent64 *ld =
				    (struct linux_dirent64 *)(out + written);
				ld->d_ino = de->inode;
				ld->d_off = next_pos;
				ld->d_reclen = (uint16_t) reclen;

				if (de->file_type == EXT2_FT_DIR)
					ld->d_type = DT_DIR;
				else if (de->file_type == EXT2_FT_REG_FILE)
					ld->d_type = DT_REG;
				else if (de->file_type == EXT2_FT_SYMLINK)
					ld->d_type = DT_LNK;
				else if (de->file_type == EXT2_FT_CHRDEV)
					ld->d_type = DT_CHR;
				else if (de->file_type == EXT2_FT_BLKDEV)
					ld->d_type = DT_BLK;
				else
					ld->d_type = DT_UNKNOWN;

				for (uint32_t i = 0; i < name_len; i++)
					ld->d_name[i] = de->name[i];
				ld->d_name[name_len] = '\0';

				uint32_t end = 19 + name_len + 1;
				while (end < reclen) {
					((uint8_t *) ld)[end] = 0;
					end++;
				}

				written += reclen;
			}

			pos = next_pos;
			bpos += de->rec_len;
		}

		if (bpos >= bs)
			pos = (uint64_t) (logical + 1) * bs;
	}

	if (new_offset)
		*new_offset = pos;
	spin_unlock(&ext2_lock);
	return (int)written;
}
