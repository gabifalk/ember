/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/vfs.h"
#include "ember/vfs_internal.h"
#include "ember/heap.h"

uint32_t
vfs_path_hash(const char *s)
{
	uint32_t h = 5381;
	while (*s) {
		h = ((h << 5) + h) ^ (uint8_t) * s;
		s++;
	}
	return h & (VFS_HASH_BUCKETS - 1);
}

void
vfs_hash_insert(vfs_node_t * n)
{
	uint32_t b = vfs_path_hash(n->path);
	n->hash_next = vfs_hash[b];
	vfs_hash[b] = n;
}

void
vfs_hash_remove(vfs_node_t * n)
{
	uint32_t b = vfs_path_hash(n->path);
	vfs_node_t **pp = &vfs_hash[b];
	while (*pp) {
		if (*pp == n) {
			*pp = n->hash_next;
			n->hash_next = 0;
			return;
		}
		pp = &(*pp)->hash_next;
	}
}

void
vfs_node_touch(vfs_node_t * n)
{
	static uint64_t vfs_access_counter;
	n->last_access = ++vfs_access_counter;
}

void
vfs_evict(void)
{
	if (vfs_node_count <= VFS_CACHE_MAX)
		return;
	/*
	 * Single-pass eviction: find and remove the N oldest evictable nodes
	 * in one list traversal instead of O(n^2) repeated scans.
	 */
	uint32_t to_evict = vfs_node_count - VFS_CACHE_TARGET;

	vfs_node_t **pp = &vfs_head;
	uint32_t evicted = 0;
	while (*pp && evicted < to_evict) {
		vfs_node_t *n = *pp;
		if (n->data == 0
		    && (n->ext2_ino != 0 || n->fs_type == VFS_FS_FAT32)
		    && n->refcount == 0) {
			*pp = n->next;
			vfs_hash_remove(n);
			n->next = 0;
			/*
			 * refcount == 0 is checked above, so no FDs reference
			 * this node.  Safe to kfree immediately.
			 * Verified: models/vfs_node_lifecycle.pml (P1-P4).
			 */
			vfs_node_count--;
			vfs_evictions++;
			evicted++;
			if (n->path)
				kfree(n->path);
			kfree(n);
		} else {
			pp = &n->next;
		}
	}
}

void
vfs_forget_path(const char *path)
{
	vfs_node_t **pp = &vfs_head;
	while (*pp) {
		if (vfs_str_eq((*pp)->path, path)) {
			vfs_node_t *old = *pp;
			*pp = old->next;
			vfs_hash_remove(old);
			old->next = 0;
			vfs_node_count--;
			if (old->refcount == 0) {
				/*
				 * No open FDs — free immediately.
				 * Verified: models/vfs_node_lifecycle.pml.
				 */
				if (old->path)
					kfree(old->path);
				kfree(old);
			} else {
				/*
				 * FDs still reference this node.  Mark evicted
				 * so vfs_unref() frees when refcount hits 0.
				 */
				old->evicted = 1;
			}
			break;
		}
		pp = &(*pp)->next;
	}
}

void
vfs_rewrite_prefix(const char *old_prefix, const char *new_prefix)
{
	uint64_t old_len = vfs_str_len(old_prefix);
	uint64_t new_len = vfs_str_len(new_prefix);

	for (vfs_node_t * n = vfs_head; n; n = n->next) {
		if (!n->path)
			continue;
		if (!vfs_str_has_prefix(n->path, old_prefix))
			continue;
		char next_ch = n->path[old_len];
		if (next_ch != '\0' && next_ch != '/')
			continue;

		vfs_hash_remove(n);

		uint64_t tail_len = vfs_str_len(n->path + old_len);
		char *np = (char *)kmalloc(new_len + tail_len + 1);
		if (!np) {
			vfs_hash_insert(n);
			continue;
		}

		kmemcpy(np, new_prefix, new_len);
		for (uint64_t i = 0; i <= tail_len; i++)
			np[new_len + i] = n->path[old_len + i];
		kfree(n->path);
		n->path = np;

		vfs_hash_insert(n);
	}
}
