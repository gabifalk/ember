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
#include "ember/time.h"

/* ---------- Shared state ---------- */
extern vfs_node_t *vfs_head;

uint32_t vfs_node_count;
uint32_t vfs_nodes_ever;
uint32_t vfs_evictions;
spinlock_t vfs_lock = SPINLOCK_INIT;

vfs_node_t *vfs_hash[VFS_HASH_BUCKETS];

/* ---------- String helpers ---------- */
int
vfs_str_eq(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return (*a == '\0' && *b == '\0');
}

uint64_t
vfs_str_len(const char *s)
{
	uint64_t n = 0;
	while (s[n])
		n++;
	return n;
}

char *
vfs_str_dup(const char *s)
{
	uint64_t len = vfs_str_len(s);
	char *p = (char *)kmalloc(len + 1);
	if (!p)
		return 0;
	for (uint64_t i = 0; i <= len; i++)
		p[i] = s[i];
	return p;
}

int
vfs_str_has_prefix(const char *s, const char *prefix)
{
	uint64_t i = 0;
	while (prefix[i]) {
		if (s[i] != prefix[i])
			return 0;
		i++;
	}
	return 1;
}

/* ---------- Parent path splitting ---------- */
static int
vfs_split_parent(const char *path, uint32_t * parent_ino,
		 const char **name, uint32_t * name_len)
{
	uint64_t pathlen = vfs_str_len(path);
	uint64_t last_slash = 0;
	for (uint64_t i = 0; i < pathlen; i++) {
		if (path[i] == '/')
			last_slash = i;
	}

	if (last_slash == 0) {
		*parent_ino = 2;
	} else {
		char parent_path[256];
		for (uint64_t i = 0; i < last_slash && i < 255; i++)
			parent_path[i] = path[i];
		parent_path[last_slash] = '\0';
		*parent_ino = ext2_path_resolve(parent_path);
		if (*parent_ino == 0)
			return -ENOENT;
	}

	*name = path + last_slash + 1;
	*name_len = (uint32_t) (pathlen - last_slash - 1);
	if (*name_len == 0)
		return -EINVAL;
	return 0;
}

/* ---------- VFS read/write ---------- */
uint64_t
vfs_read(vfs_node_t * node, uint64_t offset, void *buf, uint64_t len)
{
	if (!node || node->type != VFS_NODE_FILE)
		return 0;

	spin_lock(&vfs_lock);

	/* Ext2-backed: read authoritative size from disk. */
	if (node->ext2_ino) {
		uint32_t ext2_ino = node->ext2_ino;
		spin_unlock(&vfs_lock);
		ext2_inode_t ei;
		if (ext2_read_inode(ext2_ino, &ei) < 0)
			return 0;
		uint64_t disk_size = ei.i_size;
		if (offset >= disk_size)
			return 0;
		if (len > disk_size - offset)
			len = disk_size - offset;
		int r = ext2_read_data(&ei, offset, buf, len);
		if (r < 0)
			return 0;
		return (uint64_t) r;
	}

	if (offset >= node->size) {
		spin_unlock(&vfs_lock);
		return 0;
	}
	uint64_t remain = node->size - offset;
	if (len > remain)
		len = remain;

	/* In-memory data (cpio nodes) */
	if (node->data) {
		/* Sanity check: data pointer must be in HHDM range or kernel range. */
		uint64_t dv = (uint64_t) (uintptr_t) node->data;
		if (dv < 0xffff800000000000ULL) {
			spin_unlock(&vfs_lock);
			return 0;
		}
		kmemcpy(buf, node->data + offset, len);
		spin_unlock(&vfs_lock);
		return len;
	}

	/* FAT32-backed node. */
	if (node->fs_type == VFS_FS_FAT32 && node->fat_cluster) {
		uint32_t fc = node->fat_cluster;
		uint32_t sz = (uint32_t) node->size;
		spin_unlock(&vfs_lock);
		int r = fat32_read_data(fc, sz, offset, buf, len);
		if (r < 0)
			return 0;
		return (uint64_t) r;
	}

	spin_unlock(&vfs_lock);
	return 0;
}

uint64_t
vfs_write(vfs_node_t * node, uint64_t offset, const void *buf, uint64_t len)
{
	if (!node || node->type != VFS_NODE_FILE)
		return 0;

	spin_lock(&vfs_lock);
	if (node->ext2_ino) {
		uint32_t ext2_ino = node->ext2_ino;
		spin_unlock(&vfs_lock);
		ext2_inode_t ei;
		if (ext2_read_inode(ext2_ino, &ei) < 0)
			return 0;
		int r = ext2_write_data(&ei, ext2_ino, offset, buf, len);
		if (r < 0)
			return 0;
		/* Update cached size and timestamps. */
		ext2_read_inode(ext2_ino, &ei);
		spin_lock(&vfs_lock);
		node->size = ei.i_size;
		node->mtime = ei.i_mtime;
		node->ctime = ei.i_ctime;
		spin_unlock(&vfs_lock);
		return (uint64_t) r;
	}
	/* FAT32-backed node. */
	if (node->fs_type == VFS_FS_FAT32) {
		uint32_t fc = node->fat_cluster;
		uint32_t sz = (uint32_t) node->size;
		uint32_t dir_c = node->fat_dir_cluster;
		spin_unlock(&vfs_lock);
		uint32_t new_size = sz;
		int r = fat32_write_data(&fc, sz, offset, buf, len, &new_size);
		if (r < 0)
			return 0;
		/* Update cached node. */
		spin_lock(&vfs_lock);
		node->fat_cluster = fc;
		node->size = new_size;
		spin_unlock(&vfs_lock);
		/* Update directory entry size on disk. */
		fat32_update_dirent_size(dir_c, fc, new_size);
		return (uint64_t) r;
	}

	/* Memfd: growable in-memory buffer. */
	if (node->fs_type == VFS_FS_MEMFD) {
		uint64_t end = offset + len;
		uint8_t *old_data = (uint8_t *) node->data;
		uint64_t old_size = node->size;

		/* Grow buffer if needed. */
		if (end > old_size || !old_data) {
			/* Round up to next power of 2, minimum 4096. */
			uint64_t new_cap = 4096;
			while (new_cap < end)
				new_cap <<= 1;

			uint8_t *new_buf = (uint8_t *) kmalloc(new_cap);
			if (!new_buf) {
				spin_unlock(&vfs_lock);
				return 0;
			}
			/* Zero entire new buffer. */
			for (uint64_t i = 0; i < new_cap; i++)
				new_buf[i] = 0;
			/* Copy old data. */
			if (old_data && old_size > 0) {
				for (uint64_t i = 0; i < old_size; i++)
					new_buf[i] = old_data[i];
			}
			node->data = (const uint8_t *)new_buf;
			if (old_data)
				kfree(old_data);
		}

		/* Write data. */
		uint8_t *dst = (uint8_t *) node->data + offset;
		const uint8_t *src = (const uint8_t *)buf;
		for (uint64_t i = 0; i < len; i++)
			dst[i] = src[i];

		if (end > node->size)
			node->size = end;

		spin_unlock(&vfs_lock);
		return len;
	}

	spin_unlock(&vfs_lock);
	return 0;		/* Cpio nodes are read-only. */
}

/* ---------- VFS create ---------- */
static vfs_node_t *
vfs_create_impl(const char *path, uint16_t mode, int *err_out)
{
	if (err_out)
		*err_out = -EIO;
	if (!ext2_is_ready() && !fat32_is_ready())
		return 0;
	if (!path || path[0] != '/') {
		if (err_out)
			*err_out = -EINVAL;
		return 0;
	}

	spin_lock(&vfs_lock);

	/* Find parent directory and filename. */
	/* Find last '/'. */
	uint64_t pathlen = vfs_str_len(path);
	uint64_t last_slash = 0;
	for (uint64_t i = 0; i < pathlen; i++) {
		if (path[i] == '/')
			last_slash = i;
	}

	/* Resolve parent directory. */
	uint32_t parent_ino;
	if (last_slash == 0) {
		/* File in root directory. */
		parent_ino = 2;	/* EXT2_ROOT_INO. */
	} else {
		char parent_path[256];
		for (uint64_t i = 0; i < last_slash && i < 255; i++)
			parent_path[i] = path[i];
		parent_path[last_slash] = '\0';
		spin_unlock(&vfs_lock);
		parent_ino = ext2_path_resolve(parent_path);
		if (parent_ino == 0) {
			if (err_out)
				*err_out = -ENOENT;
			return 0;
		}
		spin_lock(&vfs_lock);
	}

	/* Get filename. */
	const char *fname = path + last_slash + 1;
	uint32_t fname_len = (uint32_t) (pathlen - last_slash - 1);
	if (fname_len == 0) {
		if (err_out)
			*err_out = -EINVAL;
		spin_unlock(&vfs_lock);
		return 0;
	}

	if (vfs_lookup_inner(path)) {
		if (err_out)
			*err_out = -EEXIST;
		spin_unlock(&vfs_lock);
		return 0;
	}

	/* Create the file -- release vfs_lock while doing I/O. */
	spin_unlock(&vfs_lock);

	/* Try ext2 first, then FAT32. */
	uint32_t new_ino = 0;
	uint32_t new_fat_cluster = 0;
	uint32_t parent_fat_cluster = 0;

	if (ext2_is_ready()) {
		new_ino =
		    ext2_create(parent_ino, fname, fname_len,
				EXT2_S_IFREG | (mode & 07777));
		if (new_ino == 0) {
			if (err_out)
				*err_out = -ENOSPC;
			return 0;
		}
	} else if (fat32_is_ready()) {
		/* Resolve parent directory on FAT32. */
		if (last_slash == 0) {
			parent_fat_cluster = 0;	/* Root. */
		} else {
			char parent_path2[256];
			for (uint64_t i = 0; i < last_slash && i < 255; i++)
				parent_path2[i] = path[i];
			parent_path2[last_slash] = '\0';
			uint32_t ps = 0;
			int pd = 0;
			if (fat32_lookup
			    (parent_path2, &parent_fat_cluster, &ps,
			     &pd) != 0) {
				if (err_out)
					*err_out = -ENOENT;
				return 0;
			}
		}
		uint32_t out_size = 0;
		new_fat_cluster =
		    fat32_create_entry(parent_fat_cluster, fname, fname_len, 0,
				       &out_size);
		if (new_fat_cluster == 0) {
			if (err_out)
				*err_out = -ENOSPC;
			return 0;
		}
	} else {
		if (err_out)
			*err_out = -EIO;
		return 0;
	}

	/* Re-acquire and insert into VFS cache. */
	spin_lock(&vfs_lock);

	/* Create VFS node. */
	vfs_node_t *n = (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
	if (!n) {
		if (err_out)
			*err_out = -ENOMEM;
		spin_unlock(&vfs_lock);
		return 0;
	}
	n->path = vfs_str_dup(path);
	if (!n->path) {
		kfree(n);
		if (err_out)
			*err_out = -ENOMEM;
		spin_unlock(&vfs_lock);
		return 0;
	}
	n->type = VFS_NODE_FILE;
	n->mode = EXT2_S_IFREG | (mode & 07777);
	n->size = 0;
	n->data = 0;
	n->rdev = 0;
	{
		uint32_t now = (uint32_t) kernel_time_sec();
		n->atime = now;
		n->mtime = now;
		n->ctime = now;
	}
	n->refcount = 0;
	n->unlinked = 0;
	if (new_ino) {
		n->ext2_ino = new_ino;
		n->fs_type = VFS_FS_EXT2;
		n->fat_cluster = 0;
		n->fat_dir_cluster = 0;
	} else {
		n->ext2_ino = 0;
		n->fs_type = VFS_FS_FAT32;
		n->fat_cluster = new_fat_cluster;
		n->fat_dir_cluster = parent_fat_cluster;
	}
	vfs_node_touch(n);
	/* Evict before inserting n — n has refcount==0 and would match
	 * eviction criteria, causing a use-after-free on the returned pointer. */
	vfs_node_count++;
	if (vfs_node_count > VFS_CACHE_MAX)
		vfs_evict();
	n->next = vfs_head;
	vfs_head = n;
	vfs_hash_insert(n);
	if (err_out)
		*err_out = 0;
	spin_unlock(&vfs_lock);
	return n;
}

vfs_node_t *
vfs_create(const char *path, uint16_t mode, int *err_out)
{
	return vfs_create_impl(path, mode, err_out);
}

/* ---------- VFS unlink ---------- */
int
vfs_unlink(const char *path)
{
	if (!path || path[0] != '/')
		return -EINVAL;

	spin_lock(&vfs_lock);
	vfs_node_t *node = vfs_lookup_inner_impl(path, 1);	/* Nofollow: unlink the symlink itself. */
	if (!node) {
		spin_unlock(&vfs_lock);
		return -ENOENT;
	}
	if (node->type == VFS_NODE_DIR) {
		/*
		 * The cache may hold a stale follow-through entry (a previous
		 * nofollow=0 lookup resolved through a symlink and cached the
		 * target's type).  Verify with ext2 before returning EISDIR.
		 */
		if (node->fs_type == VFS_FS_EXT2 && ext2_is_ready()) {
			spin_unlock(&vfs_lock);
			uint32_t nf_ino = ext2_path_resolve_nofollow(path);
			ext2_inode_t nf_ei;
			if (nf_ino && ext2_read_inode(nf_ino, &nf_ei) == 0 &&
			    (nf_ei.i_mode & 0170000) == EXT2_S_IFLNK) {
				/* It's really a symlink -- fix the cache and proceed. */
				spin_lock(&vfs_lock);
				node->type = VFS_NODE_SYMLINK;
				node->mode = nf_ei.i_mode;
				node->size = nf_ei.i_size;
				node->ext2_ino = nf_ino;
				goto unlink_proceed;
			}
			return -EISDIR;
		}
		spin_unlock(&vfs_lock);
		return -EISDIR;
	}
 unlink_proceed:;

	uint8_t fs = node->fs_type;

	/* CPIO nodes: no on-disk structure to update, just remove from VFS cache. */
	if (fs == VFS_FS_CPIO) {
		if (node->refcount > 0)
			node->unlinked = 1;
		vfs_forget_path(path);
		spin_unlock(&vfs_lock);
		return 0;
	}

	if (!ext2_is_ready() && !fat32_is_ready()) {
		spin_unlock(&vfs_lock);
		return -EIO;
	}

	uint64_t pathlen = vfs_str_len(path);
	uint64_t last_slash = 0;
	for (uint64_t i = 0; i < pathlen; i++) {
		if (path[i] == '/')
			last_slash = i;
	}

	uint32_t parent_ino;
	if (last_slash == 0) {
		parent_ino = 2;
	} else {
		char parent_path[256];
		for (uint64_t i = 0; i < last_slash && i < 255; i++)
			parent_path[i] = path[i];
		parent_path[last_slash] = '\0';
		spin_unlock(&vfs_lock);
		parent_ino = ext2_path_resolve(parent_path);
		if (parent_ino == 0)
			return -ENOENT;
		spin_lock(&vfs_lock);
	}

	const char *fname = path + last_slash + 1;
	uint32_t fname_len = (uint32_t) (pathlen - last_slash - 1);
	if (fname_len == 0) {
		spin_unlock(&vfs_lock);
		return -EINVAL;
	}

	uint32_t saved_ino = 0;
	node = vfs_lookup_inner_impl(path, 1);
	if (node)
		saved_ino = node->ext2_ino;
	spin_unlock(&vfs_lock);

	/*
	 * Linux-style deferred free: always just remove the directory entry
	 * and decrement link count.  Don't free the inode yet.
	 */
	int ur = ext2_unlink_keep_blocks(parent_ino, fname, fname_len);
	if (ur < 0)
		return ur;

	/* Atomically remove from VFS cache and decide inode fate. */
	spin_lock(&vfs_lock);
	node = vfs_lookup_inner_impl(path, 1);
	int do_free = 0;
	if (node) {
		saved_ino = node->ext2_ino;
		node->unlinked = 1;
		if (node->refcount == 0) {
			do_free = 1;
			node->ext2_ino = 0;
		}
		vfs_forget_path(path);
	} else {
		do_free = 1;
	}
	spin_unlock(&vfs_lock);

	if (do_free && saved_ino)
		ext2_free_inode_deferred(saved_ino);

	return 0;
}

/* ---------- VFS rename ---------- */
int
vfs_rename(const char *old_path, const char *new_path)
{
	if (!old_path || !new_path)
		return -EINVAL;
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	if (old_path[0] != '/' || new_path[0] != '/')
		return -EINVAL;
	if (vfs_str_eq(old_path, new_path))
		return 0;

	spin_lock(&vfs_lock);
	vfs_node_t *src = vfs_lookup_inner(old_path);
	if (!src) {
		spin_unlock(&vfs_lock);
		return -ENOENT;
	}
	if (src->ext2_ino == 0) {
		spin_unlock(&vfs_lock);
		return -EIO;
	}
	int src_is_dir = (src->type == VFS_NODE_DIR);

	vfs_node_t *dst = vfs_lookup_inner(new_path);
	if (dst && dst->ext2_ino == 0) {
		spin_unlock(&vfs_lock);
		return -EIO;
	}
	if (dst) {
		int dst_is_dir = (dst->type == VFS_NODE_DIR);
		if (src_is_dir != dst_is_dir) {
			spin_unlock(&vfs_lock);
			return -EISDIR;
		}
		if (src_is_dir) {
			spin_unlock(&vfs_lock);
			return -EEXIST;
		}
	}

	if (src_is_dir && vfs_str_has_prefix(new_path, old_path)) {
		char c = new_path[vfs_str_len(old_path)];
		if (c == '/' || c == '\0') {
			spin_unlock(&vfs_lock);
			return -EINVAL;
		}
	}
	spin_unlock(&vfs_lock);

	uint32_t old_parent_ino, new_parent_ino;
	const char *old_name, *new_name;
	uint32_t old_name_len, new_name_len;
	int r =
	    vfs_split_parent(old_path, &old_parent_ino, &old_name,
			     &old_name_len);
	if (r < 0)
		return r;
	r = vfs_split_parent(new_path, &new_parent_ino, &new_name,
			     &new_name_len);
	if (r < 0)
		return r;

	r = ext2_rename(old_parent_ino, old_name, old_name_len,
			new_parent_ino, new_name, new_name_len);
	if (r < 0)
		return r;

	spin_lock(&vfs_lock);
	/* Re-lookup src since we dropped the lock. */
	src = vfs_lookup_inner(old_path);
	if (dst && dst != src)
		vfs_forget_path(new_path);

	if (src_is_dir) {
		vfs_rewrite_prefix(old_path, new_path);
	} else if (src && src->path && vfs_str_eq(src->path, old_path)) {
		char *np = vfs_str_dup(new_path);
		if (!np) {
			spin_unlock(&vfs_lock);
			return -ENOMEM;
		}
		vfs_hash_remove(src);
		kfree(src->path);
		src->path = np;
		vfs_hash_insert(src);
	} else {
		vfs_forget_path(old_path);
	}
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS truncate ---------- */
int
vfs_truncate_path(const char *path, uint64_t new_size)
{
	if (!path || path[0] != '/')
		return -EINVAL;

	spin_lock(&vfs_lock);
	vfs_node_t *node = vfs_lookup_inner(path);
	if (!node) {
		spin_unlock(&vfs_lock);
		return -ENOENT;
	}
	if (node->type != VFS_NODE_FILE
	    || (node->ext2_ino == 0 && node->fs_type != VFS_FS_FAT32)) {
		spin_unlock(&vfs_lock);
		return -EISDIR;
	}
	if (new_size > 0xffffffffULL) {
		spin_unlock(&vfs_lock);
		return -EINVAL;
	}
	if (node->fs_type == VFS_FS_FAT32) {
		/* For FAT32, just update cached size and directory entry. */
		uint32_t fc = node->fat_cluster;
		uint32_t dc = node->fat_dir_cluster;
		node->size = new_size;
		spin_unlock(&vfs_lock);
		fat32_update_dirent_size(dc, fc, (uint32_t) new_size);
		return 0;
	}

	uint32_t ext2_ino = node->ext2_ino;
	spin_unlock(&vfs_lock);

	int r = ext2_truncate(ext2_ino, (uint32_t) new_size);
	if (r < 0)
		return r;

	spin_lock(&vfs_lock);
	node->size = new_size;
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS refcounting ---------- */
vfs_node_t *
vfs_head_get(void)
{
	return vfs_head;
}

void
vfs_ref(vfs_node_t * node)
{
	if (!node)
		return;
	spin_lock(&vfs_lock);
	node->refcount++;
	spin_unlock(&vfs_lock);
}

void
vfs_unref(vfs_node_t * node)
{
	if (!node)
		return;
	spin_lock(&vfs_lock);
	if (node->refcount > 0)
		node->refcount--;
	uint8_t do_deferred_free = (node->refcount == 0 && node->unlinked
				    && node->ext2_ino);
	uint32_t ino = node->ext2_ino;
	/*
	 * If refcount hit 0 and node was evicted from the VFS list,
	 * free the node memory.  Verified: models/vfs_node_lifecycle.pml.
	 */
	uint8_t do_kfree = (node->refcount == 0 && node->evicted);
	spin_unlock(&vfs_lock);
	if (do_deferred_free)
		ext2_free_inode_deferred(ino);
	if (do_kfree) {
		if (node->path)
			kfree(node->path);
		kfree(node);
	}
}

void
vfs_set_size(vfs_node_t * node, uint64_t size)
{
	if (!node)
		return;
	spin_lock(&vfs_lock);
	node->size = size;
	spin_unlock(&vfs_lock);
}

/* ---------- VFS mkdir ---------- */
int
vfs_mkdir(const char *path, uint32_t mode)
{
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	if (!path || path[0] != '/')
		return -ENOENT;

	/* Check if already exists. */
	spin_lock(&vfs_lock);
	if (vfs_lookup_inner(path)) {
		spin_unlock(&vfs_lock);
		return -EEXIST;
	}
	spin_unlock(&vfs_lock);

	uint32_t new_ino = 0;
	uint32_t new_fat_cluster = 0;
	uint32_t parent_fat_cluster = 0;

	if (ext2_is_ready()) {
		uint32_t parent_ino;
		const char *name;
		uint32_t name_len;
		int r = vfs_split_parent(path, &parent_ino, &name, &name_len);
		if (r < 0)
			return r;

		int mr =
		    ext2_mkdir(parent_ino, name, name_len, (uint16_t) mode);
		if (mr < 0)
			return mr;
		new_ino = ext2_lookup(parent_ino, name, name_len);
	} else if (fat32_is_ready()) {
		uint64_t pathlen = vfs_str_len(path);
		uint64_t last_slash = 0;
		for (uint64_t i = 0; i < pathlen; i++) {
			if (path[i] == '/')
				last_slash = i;
		}
		const char *fname = path + last_slash + 1;
		uint32_t fname_len = (uint32_t) (pathlen - last_slash - 1);
		if (fname_len == 0)
			return -EINVAL;

		if (last_slash == 0) {
			parent_fat_cluster = 0;
		} else {
			char parent_path[256];
			for (uint64_t i = 0; i < last_slash && i < 255; i++)
				parent_path[i] = path[i];
			parent_path[last_slash] = '\0';
			uint32_t ps = 0;
			int pd = 0;
			if (fat32_lookup
			    (parent_path, &parent_fat_cluster, &ps, &pd) != 0)
				return -ENOENT;
		}
		uint32_t out_size = 0;
		new_fat_cluster =
		    fat32_create_entry(parent_fat_cluster, fname, fname_len, 1,
				       &out_size);
		if (new_fat_cluster == 0)
			return -ENOSPC;
	}

	/* Create VFS cache node. */
	spin_lock(&vfs_lock);
	vfs_node_t *n = (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
	if (!n) {
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->path = vfs_str_dup(path);
	if (!n->path) {
		kfree(n);
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->type = VFS_NODE_DIR;
	n->mode = EXT2_S_IFDIR | (mode & 0777);
	n->size = 0;
	n->data = 0;
	n->rdev = 0;
	{
		uint32_t now = (uint32_t) kernel_time_sec();
		n->atime = now;
		n->mtime = now;
		n->ctime = now;
	}
	n->refcount = 0;
	n->unlinked = 0;
	if (new_ino) {
		n->ext2_ino = new_ino;
		n->fs_type = VFS_FS_EXT2;
		n->fat_cluster = 0;
		n->fat_dir_cluster = 0;
	} else {
		n->ext2_ino = 0;
		n->fs_type = VFS_FS_FAT32;
		n->fat_cluster = new_fat_cluster;
		n->fat_dir_cluster = parent_fat_cluster;
	}
	vfs_node_touch(n);
	vfs_node_count++;
	if (vfs_node_count > VFS_CACHE_MAX)
		vfs_evict();
	n->next = vfs_head;
	vfs_head = n;
	vfs_hash_insert(n);
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS rmdir ---------- */
int
vfs_rmdir(const char *path)
{
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	if (!path || path[0] != '/')
		return -EINVAL;

	spin_lock(&vfs_lock);
	vfs_node_t *node = vfs_lookup_inner(path);
	if (!node) {
		spin_unlock(&vfs_lock);
		return -ENOENT;
	}
	if (node->type != VFS_NODE_DIR) {
		spin_unlock(&vfs_lock);
		return -ENOTDIR;
	}
	if (!node->ext2_ino) {
		spin_unlock(&vfs_lock);
		return -EIO;
	}
	uint32_t dir_ext2_ino = node->ext2_ino;
	spin_unlock(&vfs_lock);

	/* Check directory is empty (only . and ..) */
	if (!ext2_dir_is_empty(dir_ext2_ino))
		return -ENOTEMPTY;

	uint32_t parent_ino;
	const char *name;
	uint32_t name_len;
	int r = vfs_split_parent(path, &parent_ino, &name, &name_len);
	if (r < 0)
		return r;

	/*
	 * Linux-style deferred free: remove the directory entry and decrement
	 * link count, but don't free the inode yet.  This is safe regardless
	 * of whether any fd is open -- the inode stays allocated on disk.
	 */
	r = ext2_unlink_keep_blocks(parent_ino, name, name_len);
	if (r < 0)
		return r;

	/* Decrement parent link count (remove .. back-link) */
	ext2_inode_t parent;
	if (ext2_read_inode(parent_ino, &parent) == 0) {
		if (parent.i_links_count > 0)
			parent.i_links_count--;
		ext2_write_inode(parent_ino, &parent);
	}

	/*
	 * Now atomically remove from VFS cache and decide inode fate.
	 * After cache removal no new opens can find this path, so
	 * refcount can only decrease (via close), never increase.
	 */
	spin_lock(&vfs_lock);
	node = vfs_lookup_inner(path);
	int do_free = 0;
	if (node) {
		node->unlinked = 1;
		if (node->refcount == 0) {
			do_free = 1;
			node->ext2_ino = 0;	/* Prevent double-free from vfs_unref. */
		}
		vfs_forget_path(path);
	} else {
		do_free = 1;	/* Node already evicted, nobody holds a ref. */
	}
	spin_unlock(&vfs_lock);

	if (do_free)
		ext2_free_inode_deferred(dir_ext2_ino);

	return 0;
}

/* ---------- VFS getdents ---------- */
/* linux_dirent64 structure (matches ext2.c definition) */
struct linux_dirent64 {
	uint64_t d_ino;
	uint64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

int64_t
vfs_getdents(vfs_node_t * dir, uint64_t offset, void *buf, uint64_t buflen,
	     uint64_t * new_offset)
{
	if (!dir || dir->type != VFS_NODE_DIR)
		return -ENOTDIR;

	/* Ext2-backed directory. */
	if (dir->ext2_ino) {
		return ext2_getdents(dir->ext2_ino, offset, buf, buflen,
				     new_offset);
	}

	/* FAT32-backed directory. */
	if (dir->fs_type == VFS_FS_FAT32) {
		return fat32_getdents(dir->fat_cluster, offset, buf, buflen,
				      new_offset);
	}

	/* Cpio-backed directory: walk VFS list for direct children. */
	spin_lock(&vfs_lock);
	uint64_t dir_path_len = vfs_str_len(dir->path);
	uint8_t *out = (uint8_t *) buf;
	uint64_t written = 0;
	uint64_t entry_idx = 0;

	for (vfs_node_t * n = vfs_head; n; n = n->next) {
		if (!n->path)
			continue;

		/* Check if n->path is a direct child of dir->path. */
		int is_child = 0;
		if (dir_path_len == 1 && dir->path[0] == '/') {
			/* Root dir: children are /foo (no slash after first char) */
			if (n->path[0] == '/' && n->path[1] != '\0') {
				is_child = 1;
				for (uint64_t i = 1; n->path[i]; i++) {
					if (n->path[i] == '/') {
						is_child = 0;
						break;
					}
				}
			}
		} else {
			if (vfs_str_has_prefix(n->path, dir->path) &&
			    n->path[dir_path_len] == '/') {
				/* Check no further slashes. */
				is_child = 1;
				for (uint64_t i = dir_path_len + 1; n->path[i];
				     i++) {
					if (n->path[i] == '/') {
						is_child = 0;
						break;
					}
				}
				if (n->path[dir_path_len + 1] == '\0')
					is_child = 0;
			}
		}

		if (!is_child)
			continue;

		if (entry_idx < offset) {
			entry_idx++;
			continue;
		}

		/* Extract basename. */
		const char *basename = n->path;
		uint64_t plen = vfs_str_len(n->path);
		for (uint64_t i = plen; i > 0; i--) {
			if (n->path[i - 1] == '/') {
				basename = n->path + i;
				break;
			}
		}
		uint32_t name_len = (uint32_t) vfs_str_len(basename);

		uint32_t reclen = 19 + name_len + 1;
		reclen = (reclen + 7) & ~7u;

		if (written + reclen > buflen) {
			if (new_offset)
				*new_offset = entry_idx;
			spin_unlock(&vfs_lock);
			return (int64_t) written;
		}

		struct linux_dirent64 *ld =
		    (struct linux_dirent64 *)(out + written);
		ld->d_ino = n->ext2_ino ? n->ext2_ino : 1;
		ld->d_off = entry_idx + 1;
		ld->d_reclen = (uint16_t) reclen;
		ld->d_type = (n->type == VFS_NODE_DIR) ? 4 : 8;

		for (uint32_t i = 0; i < name_len; i++)
			ld->d_name[i] = basename[i];
		ld->d_name[name_len] = '\0';

		uint32_t end = 19 + name_len + 1;
		while (end < reclen) {
			((uint8_t *) ld)[end] = 0;
			end++;
		}

		written += reclen;
		entry_idx++;
	}

	if (new_offset)
		*new_offset = entry_idx;
	spin_unlock(&vfs_lock);
	return (int64_t) written;
}

/* ---------- VFS chmod ---------- */
int
vfs_chmod(const char *path, uint32_t mode)
{
	if (!path || path[0] != '/')
		return -EINVAL;

	spin_lock(&vfs_lock);
	vfs_node_t *node = vfs_lookup_inner(path);
	if (!node) {
		spin_unlock(&vfs_lock);
		return -ENOENT;
	}
	if (!node->ext2_ino) {
		spin_unlock(&vfs_lock);
		return -EIO;
	}
	uint32_t ext2_ino = node->ext2_ino;
	spin_unlock(&vfs_lock);

	int r = ext2_chmod(ext2_ino, (uint16_t) mode);
	if (r < 0)
		return r;

	/* Update cached mode: preserve type, replace perms. */
	spin_lock(&vfs_lock);
	node->mode = (node->mode & 0170000) | (mode & 07777);
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS link ---------- */
int
vfs_link(const char *oldpath, const char *newpath)
{
	if (!oldpath || !newpath)
		return -EINVAL;
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	if (oldpath[0] != '/' || newpath[0] != '/')
		return -EINVAL;

	/* Lookup source. */
	spin_lock(&vfs_lock);
	vfs_node_t *src = vfs_lookup_inner(oldpath);
	if (!src) {
		spin_unlock(&vfs_lock);
		return -ENOENT;
	}
	if (!src->ext2_ino) {
		spin_unlock(&vfs_lock);
		return -EIO;
	}
	uint32_t src_ext2_ino = src->ext2_ino;
	int src_type = src->type;
	uint16_t src_mode = (uint16_t) src->mode;
	uint64_t src_size = src->size;
	const uint8_t *src_data = src->data;
	uint32_t src_atime = src->atime;
	uint32_t src_mtime = src->mtime;
	uint32_t src_ctime = src->ctime;
	spin_unlock(&vfs_lock);

	/* Split newpath into parent dir + name. */
	uint32_t parent_ino;
	const char *name;
	uint32_t name_len;
	int r = vfs_split_parent(newpath, &parent_ino, &name, &name_len);
	if (r < 0)
		return r;

	r = ext2_link(parent_ino, name, name_len, src_ext2_ino);
	if (r < 0)
		return r;

	/* Create VFS cache node for new path. */
	spin_lock(&vfs_lock);
	vfs_node_t *n = (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
	if (!n) {
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->path = vfs_str_dup(newpath);
	if (!n->path) {
		kfree(n);
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->type = src_type;
	n->mode = src_mode;
	n->size = src_size;
	n->data = src_data;
	n->ext2_ino = src_ext2_ino;
	n->fs_type = VFS_FS_EXT2;
	n->atime = src_atime;
	n->mtime = src_mtime;
	n->ctime = src_ctime;
	n->refcount = 0;
	n->unlinked = 0;
	vfs_node_touch(n);
	vfs_node_count++;
	if (vfs_node_count > VFS_CACHE_MAX)
		vfs_evict();
	n->next = vfs_head;
	vfs_head = n;
	vfs_hash_insert(n);
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS symlink ---------- */
int
vfs_symlink(const char *target, const char *linkpath)
{
	if (!target || !linkpath)
		return -EINVAL;
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	if (linkpath[0] != '/')
		return -EINVAL;

	/* Check if already exists. */
	spin_lock(&vfs_lock);
	if (vfs_lookup_inner(linkpath)) {
		spin_unlock(&vfs_lock);
		return -EEXIST;
	}
	spin_unlock(&vfs_lock);

	uint32_t parent_ino;
	const char *name;
	uint32_t name_len;
	int r = vfs_split_parent(linkpath, &parent_ino, &name, &name_len);
	if (r < 0)
		return r;

	int sr = ext2_symlink(parent_ino, name, name_len, target);
	if (sr < 0)
		return sr;
	uint32_t new_ino = ext2_lookup(parent_ino, name, name_len);

	/* Create VFS cache node. */
	spin_lock(&vfs_lock);
	vfs_node_t *n = (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
	if (!n) {
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->path = vfs_str_dup(linkpath);
	if (!n->path) {
		kfree(n);
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->type = VFS_NODE_SYMLINK;
	n->mode = EXT2_S_IFLNK | 0777;
	n->size = 0;
	{
		uint32_t tl = 0;
		while (target[tl])
			tl++;
		n->size = tl;
	}
	n->data = 0;
	n->ext2_ino = new_ino;
	n->fs_type = VFS_FS_EXT2;
	{
		uint32_t now = (uint32_t) kernel_time_sec();
		n->atime = now;
		n->mtime = now;
		n->ctime = now;
	}
	n->refcount = 0;
	n->unlinked = 0;
	vfs_node_touch(n);
	vfs_node_count++;
	if (vfs_node_count > VFS_CACHE_MAX)
		vfs_evict();
	n->next = vfs_head;
	vfs_head = n;
	vfs_hash_insert(n);
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS mknod ---------- */
int
vfs_mknod(const char *path, uint32_t mode, uint64_t dev)
{
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	if (!path || path[0] != '/')
		return -EINVAL;

	/* Check if already exists. */
	spin_lock(&vfs_lock);
	if (vfs_lookup_inner(path)) {
		spin_unlock(&vfs_lock);
		return -EEXIST;
	}
	spin_unlock(&vfs_lock);

	uint32_t parent_ino;
	const char *name;
	uint32_t name_len;
	int r = vfs_split_parent(path, &parent_ino, &name, &name_len);
	if (r < 0)
		return r;

	uint32_t new_ino =
	    ext2_mknod(parent_ino, name, name_len, (uint16_t) mode,
		       (uint32_t) dev);
	if (new_ino == 0)
		return -ENOSPC;

	/* Create VFS cache node. */
	spin_lock(&vfs_lock);
	vfs_node_t *n = (vfs_node_t *) kzalloc(sizeof(vfs_node_t));
	if (!n) {
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	n->path = vfs_str_dup(path);
	if (!n->path) {
		kfree(n);
		spin_unlock(&vfs_lock);
		return -ENOMEM;
	}
	uint16_t type_bits = (uint16_t) (mode & 0170000);
	if (type_bits == EXT2_S_IFCHR)
		n->type = VFS_NODE_CHRDEV;
	else if (type_bits == EXT2_S_IFBLK)
		n->type = VFS_NODE_BLKDEV;
	else
		n->type = VFS_NODE_FILE;
	n->mode = mode;
	n->size = 0;
	n->data = 0;
	n->ext2_ino = new_ino;
	n->fs_type = VFS_FS_EXT2;
	n->rdev = dev;
	{
		uint32_t now = (uint32_t) kernel_time_sec();
		n->atime = now;
		n->mtime = now;
		n->ctime = now;
	}
	n->refcount = 0;
	n->unlinked = 0;
	vfs_node_touch(n);
	vfs_node_count++;
	if (vfs_node_count > VFS_CACHE_MAX)
		vfs_evict();
	n->next = vfs_head;
	vfs_head = n;
	vfs_hash_insert(n);
	spin_unlock(&vfs_lock);
	return 0;
}

/* ---------- VFS readlink ---------- */
int
vfs_readlink(const char *path, char *buf, uint32_t bufsize)
{
	if (!path || path[0] != '/')
		return -EINVAL;
	if (!ext2_is_ready() && !fat32_is_ready())
		return -EIO;
	/* Use nofollow to get the symlink itself. */
	uint32_t ino = ext2_path_resolve_nofollow(path);
	if (ino == 0)
		return -ENOENT;
	int r = ext2_readlink(ino, buf, bufsize);
	if (r < 0)
		return r;
	return r;
}
