/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "ember/vfs.h"
#include "ember/vfs_internal.h"
#include "ember/heap.h"
#include "ember/mmu.h"

vfs_node_t *vfs_head;

static int
is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A'
								    && c <=
								    'F');
}

static uint32_t
hex_u32(const char *s)
{
	uint32_t v = 0;
	for (int i = 0; i < 8; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (uint32_t) (c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (uint32_t) (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (uint32_t) (c - 'A' + 10);
	}
	return v;
}

static uint64_t
align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void
vfs_add_node(const char *path, vfs_node_type_t type, uint32_t mode,
	     uint64_t size, const uint8_t * data)
{
	vfs_node_t *n = (vfs_node_t *) kmalloc(sizeof(vfs_node_t));
	if (!n)
		return;

	while (path[0] == '.' && path[1] == '/')
		path += 2;

	uint64_t len = 0;
	while (path[len])
		len++;
	uint64_t needs_slash = (path[0] != '/');
	char *p = (char *)kmalloc(len + needs_slash + 1);
	if (!p)
		return;
	uint64_t off = 0;
	if (needs_slash)
		p[off++] = '/';
	for (uint64_t i = 0; i < len; i++)
		p[off + i] = path[i];
	p[off + len] = '\0';

	n->path = p;
	n->type = type;
	n->mode = mode;
	n->size = size;
	n->data = data;
	n->atime = 0;
	n->mtime = 0;
	n->ctime = 0;
	n->refcount = 0;
	n->unlinked = 0;
	n->evicted = 0;
	n->last_access = 0;
	n->next = vfs_head;
	vfs_head = n;
	vfs_hash_insert(n);
	vfs_node_count++;
}

void
vfs_init_from_cpio(uint64_t initrd_phys_base, uint64_t initrd_size)
{
	vfs_head = NULL;

	/* Always create root directory node. */
	vfs_add_node("/", VFS_NODE_DIR, 0755, 0, 0);

	if (!initrd_phys_base || !initrd_size)
		return;

	const uint8_t *base = (const uint8_t *)phys_to_virt(initrd_phys_base);
	uint64_t off = 0;

	while (off + 110 <= initrd_size) {
		const char *hdr = (const char *)(base + off);
		if (hdr[0] != '0' || hdr[1] != '7' || hdr[2] != '0'
		    || hdr[3] != '7' || hdr[4] != '0' || hdr[5] != '1') {
			break;
		}

		uint32_t namesz = hex_u32(hdr + 94);
		uint32_t filesize = hex_u32(hdr + 54);
		uint32_t mode = hex_u32(hdr + 14);

		const char *name = (const char *)(base + off + 110);
		if (namesz == 0 || !is_hex(hdr[94]))
			break;

		uint64_t name_off = off + 110;
		uint64_t name_end = name_off + namesz;
		uint64_t data_off = align_up(name_end, 4);

		if (name_end > initrd_size)
			break;

		if (namesz == 11 && name[0] == 'T' && name[1] == 'R'
		    && name[2] == 'A' && name[3] == 'I' && name[4] == 'L'
		    && name[5] == 'E' && name[6] == 'R' && name[7] == '!') {
			break;
		}

		vfs_node_type_t type =
		    ((mode & 0040000) != 0) ? VFS_NODE_DIR : VFS_NODE_FILE;
		const uint8_t *data = (const uint8_t *)(base + data_off);

		vfs_add_node(name, type, mode, filesize, data);

		uint64_t file_end = data_off + filesize;
		off = align_up(file_end, 4);
	}
}
