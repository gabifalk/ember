/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/*
 * Internal declarations shared between VFS sub-files.
 * NOT part of the public kernel API -- only #include from kernel/fs/vfs*.c.
 */
#ifndef EMBER_VFS_INTERNAL_H
#define EMBER_VFS_INTERNAL_H

#include <stdint.h>
#include "ember/vfs.h"
#include "ember/spinlock.h"

/* ---------- Shared state (defined in vfs.c) ---------- */
extern vfs_node_t *vfs_head;
extern uint32_t vfs_node_count;
extern spinlock_t vfs_lock;

#define VFS_CACHE_MAX    2048
#define VFS_CACHE_TARGET 1536

#define VFS_HASH_BUCKETS 1024
extern vfs_node_t *vfs_hash[VFS_HASH_BUCKETS];

/* ---------- String helpers (defined in vfs.c) ---------- */
int vfs_str_eq(const char *a, const char *b);
uint64_t vfs_str_len(const char *s);
char *vfs_str_dup(const char *s);
int vfs_str_has_prefix(const char *s, const char *prefix);

/* ---------- Stats (defined in vfs.c) ---------- */
extern uint32_t vfs_nodes_ever;	/* Total nodes allocated (never decremented) */
extern uint32_t vfs_evictions;	/* Total nodes evicted. */

/* ---------- Vfs_cache.c ---------- */
void vfs_hash_insert(vfs_node_t * n);
void vfs_hash_remove(vfs_node_t * n);
uint32_t vfs_path_hash(const char *s);
void vfs_node_touch(vfs_node_t * n);
void vfs_evict(void);
void vfs_forget_path(const char *path);
void vfs_rewrite_prefix(const char *old_prefix, const char *new_prefix);

/* ---------- Vfs_lookup.c ---------- */
vfs_node_t *vfs_lookup_inner_impl_err(const char *path, int nofollow,
				      int *out_err);
vfs_node_t *vfs_lookup_inner_impl(const char *path, int nofollow);
vfs_node_t *vfs_lookup_inner(const char *path);

#endif
