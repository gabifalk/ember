/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/vfs.h"
#include "ember/vfs_internal.h"
#include "ember/ext2.h"
#include "ember/fat32.h"
#include "ember/heap.h"
#include "ember/syscall.h"
#include "ember/spinlock.h"

/* Internal lookup -- caller must hold vfs_lock. */
vfs_node_t *
vfs_lookup_inner_impl_err(const char *path, int nofollow, int *out_err)
{
	if (out_err)
		*out_err = 0;
	/* First: check existing nodes via hash table. */
	uint32_t bucket = vfs_path_hash(path);
	vfs_node_t *fallback = 0;
	for (vfs_node_t * cur = vfs_hash[bucket]; cur; cur = cur->hash_next) {
		if (vfs_str_eq(cur->path, path)) {
			if (!nofollow && cur->type == VFS_NODE_SYMLINK)
				continue;	/* Skip symlinks when following. */
			if (nofollow && cur->type != VFS_NODE_SYMLINK) {
				/* When not following, prefer symlink nodes; save non-symlink as fallback. */
				if (!fallback)
					fallback = cur;
				continue;
			}
			vfs_node_touch(cur);
			return cur;
		}
	}
	if (fallback) {
		vfs_node_touch(fallback);
		return fallback;
	}

	/*
	 * Second: try ext2 if initialized.
	 * Release vfs_lock during ext2 I/O to avoid holding the spinlock
	 * across disk reads (ext2_path_resolve -> blkcache -> blkdev).
	 */
	if (ext2_is_ready()) {
		spin_unlock(&vfs_lock);
		uint32_t ino = nofollow ? ext2_path_resolve_nofollow(path)
		    : ext2_path_resolve(path);
		ext2_inode_t ei;
		int ei_ok = 0;
		if (ino == UINT32_MAX) {
			/* ELOOP from symlink loop. */
			spin_lock(&vfs_lock);
			if (out_err)
				*out_err = -ELOOP;
			return 0;
		}
		if (ino) {
			ei_ok = (ext2_read_inode(ino, &ei) == 0);
		}
		spin_lock(&vfs_lock);

		if (ino && ei_ok) {
			/* Re-check cache -- another process may have added this path. */
			vfs_node_t *dup_fallback = 0;
			for (vfs_node_t * dup = vfs_hash[bucket]; dup;
			     dup = dup->hash_next) {
				if (vfs_str_eq(dup->path, path)) {
					if (!nofollow
					    && dup->type == VFS_NODE_SYMLINK)
						continue;
					if (nofollow
					    && dup->type != VFS_NODE_SYMLINK) {
						if (!dup_fallback)
							dup_fallback = dup;
						continue;
					}
					vfs_node_touch(dup);
					return dup;
				}
			}
			if (dup_fallback) {
				vfs_node_touch(dup_fallback);
				return dup_fallback;
			}

			vfs_node_t *n =
			    (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
			if (!n)
				return 0;
			n->path = vfs_str_dup(path);
			if (!n->path) {
				kfree(n);
				return 0;
			}
			{
				uint16_t type_bits = ei.i_mode & 0170000;
				if (type_bits == EXT2_S_IFDIR)
					n->type = VFS_NODE_DIR;
				else if (type_bits == EXT2_S_IFLNK)
					n->type = VFS_NODE_SYMLINK;
				else if (type_bits == EXT2_S_IFCHR)
					n->type = VFS_NODE_CHRDEV;
				else if (type_bits == EXT2_S_IFBLK)
					n->type = VFS_NODE_BLKDEV;
				else
					n->type = VFS_NODE_FILE;
			}
			n->mode = ei.i_mode;
			n->size = ei.i_size;
			n->data = 0;
			n->ext2_ino = ino;
			n->rdev = 0;
			if (n->type == VFS_NODE_CHRDEV
			    || n->type == VFS_NODE_BLKDEV) {
				if (ei.i_block[0])
					n->rdev = (uint64_t) ei.i_block[0];
				else
					n->rdev = (uint64_t) ei.i_block[1];
			}
			n->atime = ei.i_atime;
			n->mtime = ei.i_mtime;
			n->ctime = ei.i_ctime;
			n->refcount = 0;
			n->unlinked = 0;
			n->fs_type = VFS_FS_EXT2;
			n->fat_cluster = 0;
			n->fat_dir_cluster = 0;
			vfs_node_touch(n);
			/* Evict before inserting n — n has refcount==0
			 * and would match eviction criteria. */
			vfs_node_count++;
			vfs_nodes_ever++;
			if (vfs_node_count > VFS_CACHE_MAX)
				vfs_evict();
			n->next = vfs_head;
			vfs_head = n;
			vfs_hash_insert(n);

			return n;
		}
	}

	/* Third: try FAT32 if mounted. */
	if (fat32_is_ready()) {
		uint32_t fat_cluster = 0;
		uint32_t fat_size = 0;
		int fat_is_dir = 0;
		spin_unlock(&vfs_lock);
		int fat_ok =
		    (fat32_lookup(path, &fat_cluster, &fat_size, &fat_is_dir) ==
		     0);
		uint32_t parent_fat_cl = 0;
		if (fat_ok) {
			uint64_t plen = vfs_str_len(path);
			uint64_t last_slash = 0;
			for (uint64_t i = 0; i < plen; i++) {
				if (path[i] == '/')
					last_slash = i;
			}
			if (last_slash == 0) {
				parent_fat_cl = 0;
			} else {
				char parent[256];
				for (uint64_t i = 0; i < last_slash && i < 255;
				     i++)
					parent[i] = path[i];
				parent[last_slash] = '\0';
				uint32_t ps = 0;
				int pd = 0;
				fat32_lookup(parent, &parent_fat_cl, &ps, &pd);
			}
		}
		spin_lock(&vfs_lock);

		if (fat_ok) {
			/* Re-check cache. */
			for (vfs_node_t * dup = vfs_hash[bucket]; dup;
			     dup = dup->hash_next) {
				if (vfs_str_eq(dup->path, path)) {
					vfs_node_touch(dup);
					return dup;
				}
			}

			vfs_node_t *n =
			    (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
			if (!n)
				return 0;
			n->path = vfs_str_dup(path);
			if (!n->path) {
				kfree(n);
				return 0;
			}
			n->type = fat_is_dir ? VFS_NODE_DIR : VFS_NODE_FILE;
			n->mode =
			    fat_is_dir ? (EXT2_S_IFDIR | 0755) : (EXT2_S_IFREG |
								  0644);
			n->size = fat_size;
			n->data = 0;
			n->ext2_ino = 0;
			n->rdev = 0;
			n->refcount = 0;
			n->unlinked = 0;
			n->fs_type = VFS_FS_FAT32;
			n->fat_cluster = fat_cluster;
			n->fat_dir_cluster = parent_fat_cl;
			vfs_node_touch(n);
			/* Evict before inserting n — see ext2 path above. */
			vfs_node_count++;
			if (vfs_node_count > VFS_CACHE_MAX)
				vfs_evict();
			n->next = vfs_head;
			vfs_head = n;
			vfs_hash_insert(n);
			return n;
		}
	}

	return 0;
}

vfs_node_t *
vfs_lookup_inner_impl(const char *path, int nofollow)
{
	return vfs_lookup_inner_impl_err(path, nofollow, 0);
}

vfs_node_t *
vfs_lookup_inner(const char *path)
{
	return vfs_lookup_inner_impl(path, 0);
}

vfs_node_t *
vfs_lookup(const char *path)
{
	spin_lock(&vfs_lock);
	vfs_node_t *r = vfs_lookup_inner(path);
	spin_unlock(&vfs_lock);
	return r;
}

vfs_node_t *
vfs_lookup_err(const char *path, int nofollow, int *out_err)
{
	spin_lock(&vfs_lock);
	vfs_node_t *r = vfs_lookup_inner_impl_err(path, nofollow, out_err);
	spin_unlock(&vfs_lock);
	return r;
}
