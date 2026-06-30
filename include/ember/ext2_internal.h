/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/*
 * Internal declarations shared between ext2 sub-files.
 * NOT part of the public kernel API -- only #include from kernel/fs/ext2*.c.
 *
 * Naming convention: functions with an _inner suffix (or _impl for path
 * resolution) must be called with ext2_lock already held.  The corresponding
 * public functions in ext2.h acquire ext2_lock themselves and delegate to the
 * _inner variant.  This two-level pattern exists because many internal ext2
 * operations compose multiple _inner calls under a single lock hold.
 */
#ifndef EMBER_EXT2_INTERNAL_H
#define EMBER_EXT2_INTERNAL_H

#include <stdint.h>
#include "ember/ext2.h"
#include "ember/spinlock.h"

/* ---------- Shared state (defined in ext2.c) ---------- */
extern spinlock_t ext2_lock;
extern ext2_superblock_t ext2_sb;
extern ext2_group_desc_t *ext2_group_descs;
extern uint32_t ext2_block_size;
extern uint32_t ext2_groups_count;
extern uint16_t ext2_inode_size;
extern int ext2_ready;
extern int ext2_dev;
extern uint32_t ext2_ptrs_per_block;
extern int ext2_has_filetype;

/* ---------- Ext2_inode.c ---------- */
int ext2_read_inode_inner(uint32_t ino, ext2_inode_t * out);
int ext2_write_inode_inner(uint32_t ino, ext2_inode_t * inode);
uint32_t ext2_alloc_inode_inner(void);
void ext2_free_inode_inner(uint32_t ino);

/* ---------- Ext2_block.c ---------- */
uint32_t ext2_block_map(ext2_inode_t * inode, uint32_t logical);
int ext2_block_set(ext2_inode_t * inode, uint32_t logical, uint32_t phys_block);
uint32_t ext2_alloc_block_inner(void);
void ext2_free_block_inner(uint32_t block_num);
void ext2_free_inode_blocks(ext2_inode_t * inode);

/* ---------- Ext2.c (superblock/GDT persistence) ---------- */
void ext2_write_superblock(void);
void ext2_write_group_descs(void);

/* ---------- Ext2_dir.c ---------- */
uint32_t ext2_lookup_inner(uint32_t dir_ino, const char *name,
			   uint32_t name_len);
int ext2_add_dir_entry_existing(uint32_t dir_ino, const char *name,
				uint32_t name_len, uint32_t target_ino,
				uint8_t file_type);
int ext2_remove_dir_entry_only(uint32_t dir_ino, const char *name,
			       uint32_t name_len);
int ext2_set_dir_entry_inode(uint32_t dir_ino, const char *name,
			     uint32_t name_len, uint32_t new_ino);
int ext2_unlink_inner(uint32_t dir_ino, const char *name, uint32_t name_len);

/* ---------- Ext2_ops.c ---------- */
uint32_t ext2_path_resolve_impl(const char *path, int nofollow, int depth);

#endif
