/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_EXT2_H
#define EMBER_EXT2_H

#include <stdint.h>

#define EXT2_MAGIC            0xEF53
#define EXT2_ROOT_INO         2
#define EXT2_MAX_BLOCK_GROUPS 256

/* Superblock (located at byte offset 1024, i.e. block 1 for 1 KiB blocks) */
typedef struct {
	uint32_t s_inodes_count;
	uint32_t s_blocks_count;
	uint32_t s_r_blocks_count;
	uint32_t s_free_blocks_count;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_frag_size;
	uint32_t s_blocks_per_group;
	uint32_t s_frags_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;
	/* Rev 1 fields. */
	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t s_uuid[16];
	char s_volume_name[16];
	char s_last_mounted[64];
	uint32_t s_algo_bitmap;
	/* Remaining fields not needed. */
} __attribute__ ((packed)) ext2_superblock_t;

/* Block group descriptor (32 bytes) */
typedef struct {
	uint32_t bg_block_bitmap;
	uint32_t bg_inode_bitmap;
	uint32_t bg_inode_table;
	uint16_t bg_free_blocks_count;
	uint16_t bg_free_inodes_count;
	uint16_t bg_used_dirs_count;
	uint16_t bg_pad;
	uint8_t bg_reserved[12];
} __attribute__ ((packed)) ext2_group_desc_t;

/* Inode (128 bytes for rev 0) */
typedef struct {
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	uint32_t i_dtime;
	uint16_t i_gid;
	uint16_t i_links_count;
	uint32_t i_blocks;
	uint32_t i_flags;
	uint32_t i_osd1;
	uint32_t i_block[15];
	uint32_t i_generation;
	uint32_t i_file_acl;
	uint32_t i_dir_acl;
	uint32_t i_faddr;
	uint8_t i_osd2[12];
} __attribute__ ((packed)) ext2_inode_t;

/* Directory entry. */
typedef struct {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[];
} __attribute__ ((packed)) ext2_dir_entry_t;

/* File type values in dir entry. */
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

/* Mode flags. */
#define EXT2_S_IFMT   0170000	/* File type mask. */
#define EXT2_S_IFDIR  0040000
#define EXT2_S_IFREG  0100000
#define EXT2_S_IFLNK  0120000
#define EXT2_S_IFCHR  0020000
#define EXT2_S_IFBLK  0060000

/* i_block[] layout. */
#define EXT2_NDIR_BLOCKS       12	/* Direct blocks: indices 0..11. */
#define EXT2_IND_BLOCK         12	/* Single-indirect block index. */
#define EXT2_DIND_BLOCK        13	/* Double-indirect block index. */
#define EXT2_TIND_BLOCK        14	/* Triple-indirect block index. */
#define EXT2_N_BLOCKS          15	/* Total entries in i_block[]. */

/* Directory entry base size (inode + rec_len + name_len + file_type, before name) */
#define EXT2_DIR_REC_LEN_BASE  8

/* Sector size used in i_blocks accounting (always 512 per POSIX/ext2 spec) */
#define EXT2_SECTOR_SIZE       512

/* Maximum length for inline (fast) symlinks stored in i_block[]. */
#define EXT2_SYMLINK_INLINE_MAX 60

/* File type values in dir entry. */
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_SYMLINK  7

/* Superblock feature flags. */
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

int ext2_statfs(void *buf);
int ext2_init(int blkdev_index);
int ext2_read_inode(uint32_t ino, ext2_inode_t * out);
int ext2_read_data(ext2_inode_t * inode, uint64_t offset, void *buf,
		   uint64_t len);
uint32_t ext2_lookup(uint32_t dir_ino, const char *name, uint32_t name_len);
uint32_t ext2_path_resolve(const char *path);
int ext2_is_ready(void);
int ext2_get_dev(void);

/* Write operations (M9) */
int ext2_write_inode(uint32_t ino, ext2_inode_t * inode);
int ext2_write_data(ext2_inode_t * inode, uint32_t ino, uint64_t offset,
		    const void *buf, uint64_t len);
uint32_t ext2_alloc_block(void);
uint32_t ext2_alloc_inode(void);
void ext2_free_block(uint32_t block_num);
void ext2_free_inode(uint32_t ino);
uint32_t ext2_create(uint32_t dir_ino, const char *name, uint32_t name_len,
		     uint16_t mode);
int ext2_unlink(uint32_t dir_ino, const char *name, uint32_t name_len);
int ext2_unlink_keep_blocks(uint32_t dir_ino, const char *name,
			    uint32_t name_len);
void ext2_free_inode_deferred(uint32_t ino);
int ext2_truncate(uint32_t ino, uint32_t new_size);
int ext2_rename(uint32_t old_dir_ino, const char *old_name,
		uint32_t old_name_len, uint32_t new_dir_ino,
		const char *new_name, uint32_t new_name_len);
int ext2_mkdir(uint32_t parent_ino, const char *name, uint32_t name_len,
	       uint16_t mode);
int ext2_getdents(uint32_t dir_ino, uint64_t offset, void *buf, uint64_t buflen,
		  uint64_t * new_offset);
int ext2_dir_is_empty(uint32_t dir_ino);

/* Symlink, chmod, hard link operations. */
int ext2_chmod(uint32_t ino, uint16_t mode);
int ext2_link(uint32_t dir_ino, const char *name, uint32_t name_len,
	      uint32_t target_ino);
int ext2_symlink(uint32_t dir_ino, const char *name, uint32_t name_len,
		 const char *target);
int ext2_readlink(uint32_t ino, char *buf, uint32_t bufsize);
uint32_t ext2_path_resolve_nofollow(const char *path);
uint32_t ext2_mknod(uint32_t dir_ino, const char *name, uint32_t name_len,
		    uint16_t mode, uint32_t dev);

#endif
