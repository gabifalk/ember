/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_FAT32_H
#define EMBER_FAT32_H

#include <stdint.h>

/*
 * Initialize FAT32 driver on the given partition.
 * part_start_lba: absolute LBA of the partition start on disk.
 * Returns 0 on success, -1 on error.
 */
int fat32_init(int blkdev_index, uint64_t part_start_lba);

/* Check if FAT32 is mounted and ready. */
int fat32_is_ready(void);

/*
 * Resolve a UNIX-style path (e.g. "/steps/manifest") to a FAT32 directory entry.
 * Returns 0 on success, -1 if not found.
 * On success, fills out_cluster (first cluster) and out_size (file size).
 * out_is_dir is set to 1 if the entry is a directory.
 */
int fat32_lookup(const char *path, uint32_t * out_cluster, uint32_t * out_size,
		 int *out_is_dir);

/*
 * Read file data starting from the given cluster.
 * offset/len specify the byte range within the file.
 * Returns bytes read, or -1 on error.
 */
int fat32_read_data(uint32_t start_cluster, uint32_t file_size,
		    uint64_t offset, void *buf, uint64_t len);

/*
 * List directory entries.
 * dir_cluster: first cluster of the directory (0 for root).
 * offset: entry index to start from.
 * buf/buflen: output buffer for linux_dirent64 entries.
 * new_offset: updated entry index after listing.
 * Returns bytes written to buf, or negative on error.
 */
int64_t fat32_getdents(uint32_t dir_cluster, uint64_t offset,
		       void *buf, uint64_t buflen, uint64_t * new_offset);

/*
 * Write file data, extending the cluster chain as needed.
 * start_cluster is updated if a first cluster is allocated.
 * file_size_out is set to the new file size.
 * Returns bytes written, or -1 on error.
 */
int fat32_write_data(uint32_t * start_cluster, uint32_t file_size,
		     uint64_t offset, const void *buf, uint64_t len,
		     uint32_t * file_size_out);

/*
 * Create a new file or directory entry in a FAT32 directory.
 * Returns the first cluster of the new entry, or 0 on failure.
 */
uint32_t fat32_create_entry(uint32_t dir_cluster, const char *name,
			    uint32_t name_len, int is_dir, uint32_t * out_size);

/* Update the file_size field of a directory entry identified by its first cluster. */
int fat32_update_dirent_size(uint32_t dir_cluster, uint32_t entry_cluster,
			     uint32_t new_size);

#endif
