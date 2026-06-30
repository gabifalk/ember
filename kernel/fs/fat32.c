/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * FAT32 filesystem driver (read-only + write support).
 * Reads the BPB from sector 0 of the partition, navigates the FAT chain,
 * and supports file/directory traversal via UNIX-style paths.
 */

#include <stdint.h>

#include "ember/fat32.h"
#include "ember/blkdev.h"
#include "ember/blkcache.h"
#include "ember/console.h"
#include "ember/heap.h"
#include "ember/syscall.h"

/* FAT32 BPB (BIOS Parameter Block) -- at byte 0 of partition. */
typedef struct {
	uint8_t jmp[3];
	uint8_t oem_name[8];
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint16_t reserved_sectors;
	uint8_t num_fats;
	uint16_t root_entry_count;	/* 0 For FAT32. */
	uint16_t total_sectors_16;
	uint8_t media;
	uint16_t fat_size_16;	/* 0 For FAT32. */
	uint16_t sectors_per_track;
	uint16_t num_heads;
	uint32_t hidden_sectors;
	uint32_t total_sectors_32;
	/* FAT32-specific fields. */
	uint32_t fat_size_32;
	uint16_t ext_flags;
	uint16_t fs_version;
	uint32_t root_cluster;
	uint16_t fs_info;
	uint16_t backup_boot_sector;
	uint8_t reserved[12];
	uint8_t drive_number;
	uint8_t reserved1;
	uint8_t boot_sig;
	uint32_t volume_id;
	uint8_t volume_label[11];
	uint8_t fs_type[8];
} __attribute__ ((packed)) fat32_bpb_t;

/* FAT32 directory entry (32 bytes) */
typedef struct {
	uint8_t name[11];	/* 8.3 Format. */
	uint8_t attr;
	uint8_t nt_reserved;
	uint8_t create_time_tenths;
	uint16_t create_time;
	uint16_t create_date;
	uint16_t access_date;
	uint16_t first_cluster_hi;
	uint16_t write_time;
	uint16_t write_date;
	uint16_t first_cluster_lo;
	uint32_t file_size;
} __attribute__ ((packed)) fat32_dirent_t;

/* Long filename entry (32 bytes) */
typedef struct {
	uint8_t order;
	uint16_t name1[5];
	uint8_t attr;
	uint8_t type;
	uint8_t checksum;
	uint16_t name2[6];
	uint16_t first_cluster_lo;	/* Always 0. */
	uint16_t name3[2];
} __attribute__ ((packed)) fat32_lfn_t;

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

#define FAT32_EOC           0x0FFFFFF8u	/* End-of-chain marker. */
#define FAT32_BAD           0x0FFFFFF7u
#define FAT32_MASK          0x0FFFFFFFu

/* Global FAT32 state. */
static int fat32_ready;
static int fat32_dev = -1;	/* Blkdev device index. */
static uint64_t fat32_part_lba;	/* Absolute LBA of partition start. */
static uint32_t fat32_bytes_per_sec;
static uint32_t fat32_sec_per_clus;
static uint32_t fat32_reserved_sec;
static uint32_t fat32_num_fats;
static uint32_t fat32_fat_size;	/* Sectors per FAT. */
static uint32_t fat32_root_cluster;
static uint32_t fat32_data_start;	/* First sector of data region (relative to partition) */
static uint32_t fat32_total_clusters;

/* Read a sector relative to the partition start. */
static int
fat32_read_sector(uint32_t rel_sector, void *buf)
{
	uint32_t abs_lba = (uint32_t) fat32_part_lba + rel_sector;
	return blkdev_read(fat32_dev, abs_lba, 1, buf);
}

/* Read multiple sectors relative to the partition start. */
static int
fat32_read_sectors(uint32_t rel_sector, uint32_t count, void *buf)
{
	uint8_t *p = (uint8_t *) buf;
	for (uint32_t i = 0; i < count; i++) {
		if (fat32_read_sector(rel_sector + i, p + i * 512) != 0)
			return -1;
	}
	return 0;
}

/* Convert cluster number to first sector (relative to partition) */
static uint32_t
cluster_to_sector(uint32_t cluster)
{
	return fat32_data_start + (cluster - 2) * fat32_sec_per_clus;
}

/* Read a FAT entry for the given cluster. */
static uint32_t
fat32_get_fat_entry(uint32_t cluster)
{
	/* Each FAT entry is 4 bytes. */
	uint32_t fat_offset = cluster * 4;
	uint32_t fat_sector = fat32_reserved_sec + (fat_offset / 512);
	uint32_t entry_offset = fat_offset % 512;

	uint8_t sector[512];
	if (fat32_read_sector(fat_sector, sector) != 0)
		return FAT32_EOC;

	uint32_t val = *(uint32_t *) (sector + entry_offset);
	return val & FAT32_MASK;
}

/*
 * Follow the FAT chain to find the cluster at a given index in the chain.
 * Returns the cluster number, or 0 on error/EOC.
 */
static uint32_t
fat32_follow_chain(uint32_t start_cluster, uint32_t index)
{
	uint32_t cluster = start_cluster;
	for (uint32_t i = 0; i < index; i++) {
		cluster = fat32_get_fat_entry(cluster);
		if (cluster >= FAT32_EOC || cluster < 2)
			return 0;
	}
	return cluster;
}

/* Write a FAT entry. */
static int
fat32_set_fat_entry(uint32_t cluster, uint32_t value)
{
	uint32_t fat_offset = cluster * 4;
	uint32_t fat_sector = fat32_reserved_sec + (fat_offset / 512);
	uint32_t entry_offset = fat_offset % 512;

	uint8_t sector[512];
	if (fat32_read_sector(fat_sector, sector) != 0)
		return -1;

	uint32_t old = *(uint32_t *) (sector + entry_offset);
	*(uint32_t *) (sector + entry_offset) =
	    (old & 0xF0000000u) | (value & FAT32_MASK);

	uint32_t abs_lba = (uint32_t) fat32_part_lba + fat_sector;
	if (blkdev_write(fat32_dev, abs_lba, 1, sector) != 0)
		return -1;

	/* Update second FAT copy if present. */
	if (fat32_num_fats > 1) {
		uint32_t fat2_sector = fat_sector + fat32_fat_size;
		uint32_t abs_lba2 = (uint32_t) fat32_part_lba + fat2_sector;
		blkdev_write(fat32_dev, abs_lba2, 1, sector);
	}
	return 0;
}

/*
 * Allocate a free cluster from the FAT.
 * Returns cluster number, or 0 if disk full.
 */
static uint32_t
fat32_alloc_cluster(void)
{
	for (uint32_t c = 2; c < fat32_total_clusters + 2; c++) {
		uint32_t val = fat32_get_fat_entry(c);
		if (val == 0) {
			/* Mark as end-of-chain. */
			fat32_set_fat_entry(c, FAT32_EOC);
			/* Zero out the cluster. */
			uint32_t sec = cluster_to_sector(c);
			uint8_t zero[512];
			for (int i = 0; i < 512; i++)
				zero[i] = 0;
			for (uint32_t s = 0; s < fat32_sec_per_clus; s++) {
				uint32_t abs =
				    (uint32_t) fat32_part_lba + sec + s;
				blkdev_write(fat32_dev, abs, 1, zero);
			}
			return c;
		}
	}
	return 0;
}

int
fat32_init(int dev, uint64_t part_start_lba)
{
	fat32_ready = 0;
	fat32_dev = dev;
	fat32_part_lba = part_start_lba;

	uint8_t sector[512];
	if (fat32_read_sector(0, sector) != 0) {
		console_write("fat32: failed to read BPB\n");
		return -1;
	}

	fat32_bpb_t *bpb = (fat32_bpb_t *) sector;

	/* Validate. */
	if (bpb->bytes_per_sector != 512) {
		console_write("fat32: unsupported sector size\n");
		return -1;
	}
	if (bpb->root_entry_count != 0) {
		console_write("fat32: not FAT32 (root_entry_count != 0)\n");
		return -1;
	}

	fat32_bytes_per_sec = bpb->bytes_per_sector;
	fat32_sec_per_clus = bpb->sectors_per_cluster;
	fat32_reserved_sec = bpb->reserved_sectors;
	fat32_num_fats = bpb->num_fats;
	fat32_fat_size = bpb->fat_size_32;
	fat32_root_cluster = bpb->root_cluster;

	fat32_data_start = fat32_reserved_sec + fat32_num_fats * fat32_fat_size;

	uint32_t total_sectors = bpb->total_sectors_32;
	uint32_t data_sectors = total_sectors - fat32_data_start;
	fat32_total_clusters = data_sectors / fat32_sec_per_clus;

	fat32_ready = 1;

	console_write("fat32: mounted, ");
	/* Print cluster size. */
	char buf[32];
	uint32_t cluster_bytes = fat32_sec_per_clus * 512;
	int pos = 0;
	if (cluster_bytes >= 1024) {
		uint32_t kb = cluster_bytes / 1024;
		if (kb >= 100)
			buf[pos++] = '0' + (kb / 100) % 10;
		if (kb >= 10)
			buf[pos++] = '0' + (kb / 10) % 10;
		buf[pos++] = '0' + kb % 10;
		buf[pos++] = 'K';
	} else {
		uint32_t v = cluster_bytes;
		if (v >= 100)
			buf[pos++] = '0' + (v / 100) % 10;
		if (v >= 10)
			buf[pos++] = '0' + (v / 10) % 10;
		buf[pos++] = '0' + v % 10;
	}
	buf[pos] = '\0';
	console_write(buf);
	console_write("/cluster\n");

	return 0;
}

int
fat32_is_ready(void)
{
	return fat32_ready;
}

/* Convert a path component to 8.3 uppercase for comparison. */
static int
fat32_name_match(const fat32_dirent_t * de, const char *name, uint32_t name_len)
{
	/* Build the 8.3 name from the directory entry. */
	char entry_name[13];
	int pos = 0;

	/* Base name (first 8 bytes, trim trailing spaces) */
	int base_end = 7;
	while (base_end >= 0 && de->name[base_end] == ' ')
		base_end--;
	for (int i = 0; i <= base_end; i++) {
		char c = (char)de->name[i];
		/* Convert to lowercase for comparison. */
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		entry_name[pos++] = c;
	}

	/* Extension (last 3 bytes, trim trailing spaces) */
	int ext_end = 2;
	while (ext_end >= 0 && de->name[8 + ext_end] == ' ')
		ext_end--;
	if (ext_end >= 0) {
		entry_name[pos++] = '.';
		for (int i = 0; i <= ext_end; i++) {
			char c = (char)de->name[8 + i];
			if (c >= 'A' && c <= 'Z')
				c = c - 'A' + 'a';
			entry_name[pos++] = c;
		}
	}
	entry_name[pos] = '\0';

	/* Compare with input name (case-insensitive) */
	if ((uint32_t) pos != name_len)
		return 0;
	for (uint32_t i = 0; i < name_len; i++) {
		char a = entry_name[i];
		char b = name[i];
		if (b >= 'A' && b <= 'Z')
			b = b - 'A' + 'a';
		if (a != b)
			return 0;
	}
	return 1;
}

/* LFN state for collecting long filename characters. */
typedef struct {
	char name[256];
	uint32_t len;
	uint8_t checksum;
	int valid;
} lfn_state_t;

static uint8_t
lfn_checksum(const uint8_t * short_name)
{
	uint8_t sum = 0;
	for (int i = 0; i < 11; i++) {
		sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i];
	}
	return sum;
}

static void
lfn_collect(lfn_state_t * state, const fat32_lfn_t * lfn)
{
	uint8_t ord = lfn->order & 0x3F;
	if (ord == 0 || ord > 20) {
		state->valid = 0;
		return;
	}

	if (lfn->order & 0x40) {
		/* First (last physical) LFN entry -- reset state. */
		for (int i = 0; i < 256; i++)
			state->name[i] = 0;
		state->len = 0;
		state->checksum = lfn->checksum;
		state->valid = 1;
	} else if (lfn->checksum != state->checksum) {
		state->valid = 0;
		return;
	}

	/* Copy characters from this LFN entry (13 chars per entry) */
	uint32_t base = ((uint32_t) ord - 1) * 13;
	uint16_t chars[13];
	for (int i = 0; i < 5; i++)
		chars[i] = lfn->name1[i];
	for (int i = 0; i < 6; i++)
		chars[5 + i] = lfn->name2[i];
	for (int i = 0; i < 2; i++)
		chars[11 + i] = lfn->name3[i];

	for (int i = 0; i < 13; i++) {
		uint32_t idx = base + (uint32_t) i;
		if (idx >= 255)
			break;
		uint16_t c = chars[i];
		if (c == 0 || c == 0xFFFF) {
			if (idx > state->len)
				state->len = idx;
			break;
		}
		state->name[idx] = (char)(c & 0xFF);	/* ASCII portion of UTF-16. */
		if (idx + 1 > state->len)
			state->len = idx + 1;
	}
}

static int
lfn_name_match(const lfn_state_t * state, const fat32_dirent_t * de,
	       const char *name, uint32_t name_len)
{
	if (!state->valid)
		return 0;
	if (lfn_checksum(de->name) != state->checksum)
		return 0;
	if (state->len != name_len)
		return 0;
	for (uint32_t i = 0; i < name_len; i++) {
		char a = state->name[i];
		char b = name[i];
		/* Case-insensitive compare. */
		if (a >= 'A' && a <= 'Z')
			a = a - 'A' + 'a';
		if (b >= 'A' && b <= 'Z')
			b = b - 'A' + 'a';
		if (a != b)
			return 0;
	}
	return 1;
}

/*
 * Look up a single name component in a directory cluster chain.
 * Returns the first cluster of the found entry, or 0 if not found.
 * Sets *out_size and *out_is_dir.
 */
static uint32_t
fat32_dir_lookup(uint32_t dir_cluster, const char *name,
		 uint32_t name_len, uint32_t * out_size, int *out_is_dir)
{
	uint8_t sector[512];
	uint32_t cluster = dir_cluster;
	if (cluster == 0)
		cluster = fat32_root_cluster;
	lfn_state_t lfn;
	lfn.valid = 0;

	while (cluster >= 2 && cluster < FAT32_EOC) {
		uint32_t sec = cluster_to_sector(cluster);
		for (uint32_t s = 0; s < fat32_sec_per_clus; s++) {
			if (fat32_read_sector(sec + s, sector) != 0)
				return 0;

			for (uint32_t e = 0; e < 512 / 32; e++) {
				fat32_dirent_t *de =
				    (fat32_dirent_t *) (sector + e * 32);
				if (de->name[0] == 0x00)
					return 0;	/* End of directory. */
				if (de->name[0] == 0xE5) {
					lfn.valid = 0;
					continue;
				}
				/* Deleted. */
				if (de->attr == FAT_ATTR_LFN) {
					lfn_collect(&lfn, (fat32_lfn_t *) de);
					continue;
				}

				if (de->attr & FAT_ATTR_VOLUME_ID) {
					lfn.valid = 0;
					continue;
				}

				/* Check LFN match first, then short name. */
				int match =
				    lfn_name_match(&lfn, de, name, name_len)
				    || fat32_name_match(de, name, name_len);
				lfn.valid = 0;

				if (match) {
					uint32_t first_cluster =
					    ((uint32_t) de->
					     first_cluster_hi << 16) |
					    (uint32_t) de->first_cluster_lo;
					if (out_size)
						*out_size = de->file_size;
					if (out_is_dir)
						*out_is_dir =
						    (de->
						     attr & FAT_ATTR_DIRECTORY)
						    ? 1 : 0;
					return first_cluster;
				}
			}
		}
		cluster = fat32_get_fat_entry(cluster);
	}
	return 0;
}

int
fat32_lookup(const char *path, uint32_t * out_cluster, uint32_t * out_size,
	     int *out_is_dir)
{
	if (!fat32_ready)
		return -1;
	if (!path || path[0] != '/')
		return -1;

	/* Root directory. */
	if (path[1] == '\0') {
		if (out_cluster)
			*out_cluster = fat32_root_cluster;
		if (out_size)
			*out_size = 0;
		if (out_is_dir)
			*out_is_dir = 1;
		return 0;
	}

	uint32_t dir_cluster = fat32_root_cluster;
	const char *p = path + 1;	/* Skip leading / */

	while (*p) {
		/* Find component end. */
		const char *comp = p;
		uint32_t comp_len = 0;
		while (*p && *p != '/') {
			p++;
			comp_len++;
		}
		if (*p == '/')
			p++;	/* Skip separator. */

		uint32_t size = 0;
		int is_dir = 0;
		uint32_t cluster =
		    fat32_dir_lookup(dir_cluster, comp, comp_len, &size,
				     &is_dir);
		if (cluster == 0 && !is_dir)
			return -1;	/* Not found. */

		if (*p == '\0' || (*p == '\0' && *(p - 1) == '/')) {
			/* Last component. */
			if (out_cluster)
				*out_cluster = cluster;
			if (out_size)
				*out_size = size;
			if (out_is_dir)
				*out_is_dir = is_dir;
			return 0;
		}

		if (!is_dir)
			return -1;	/* Intermediate component is not a directory. */
		dir_cluster = cluster;
	}

	return -1;
}

int
fat32_read_data(uint32_t start_cluster, uint32_t file_size,
		uint64_t offset, void *buf, uint64_t len)
{
	if (!fat32_ready)
		return -1;
	if (offset >= file_size)
		return 0;
	if (offset + len > file_size)
		len = file_size - offset;
	if (len == 0)
		return 0;

	uint32_t cluster_bytes = fat32_sec_per_clus * 512;
	uint8_t *dst = (uint8_t *) buf;
	uint64_t bytes_read = 0;

	/* Skip to the starting cluster. */
	uint32_t cluster_index = (uint32_t) (offset / cluster_bytes);
	uint32_t cluster_offset = (uint32_t) (offset % cluster_bytes);
	uint32_t cluster = fat32_follow_chain(start_cluster, cluster_index);

	uint8_t sector[512];

	while (bytes_read < len && cluster >= 2 && cluster < FAT32_EOC) {
		uint32_t sec = cluster_to_sector(cluster);
		uint32_t bytes_in_cluster = cluster_bytes - cluster_offset;
		if (bytes_in_cluster > len - bytes_read)
			bytes_in_cluster = (uint32_t) (len - bytes_read);

		/* Read sector by sector within the cluster. */
		uint32_t sec_offset = cluster_offset / 512;
		uint32_t byte_in_sec = cluster_offset % 512;

		while (bytes_in_cluster > 0) {
			if (fat32_read_sector(sec + sec_offset, sector) != 0)
				return (int)bytes_read;

			uint32_t chunk = 512 - byte_in_sec;
			if (chunk > bytes_in_cluster)
				chunk = bytes_in_cluster;

			uint8_t *src = sector + byte_in_sec;
			for (uint32_t i = 0; i < chunk; i++)
				dst[bytes_read + i] = src[i];

			bytes_read += chunk;
			bytes_in_cluster -= chunk;
			sec_offset++;
			byte_in_sec = 0;
		}

		cluster_offset = 0;
		cluster = fat32_get_fat_entry(cluster);
	}

	return (int)bytes_read;
}

/* linux_dirent64 structure. */
struct fat32_linux_dirent64 {
	uint64_t d_ino;
	uint64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

/* Extract a readable name from a short directory entry. */
static uint32_t
fat32_extract_short_name(const fat32_dirent_t * de, char *out)
{
	uint32_t pos = 0;

	/* Base name. */
	int base_end = 7;
	while (base_end >= 0 && de->name[base_end] == ' ')
		base_end--;
	for (int i = 0; i <= base_end; i++) {
		char c = (char)de->name[i];
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		out[pos++] = c;
	}

	/* Extension. */
	int ext_end = 2;
	while (ext_end >= 0 && de->name[8 + ext_end] == ' ')
		ext_end--;
	if (ext_end >= 0) {
		out[pos++] = '.';
		for (int i = 0; i <= ext_end; i++) {
			char c = (char)de->name[8 + i];
			if (c >= 'A' && c <= 'Z')
				c = c - 'A' + 'a';
			out[pos++] = c;
		}
	}
	out[pos] = '\0';
	return pos;
}

int64_t
fat32_getdents(uint32_t dir_cluster, uint64_t offset,
	       void *buf, uint64_t buflen, uint64_t * new_offset)
{
	if (!fat32_ready)
		return -ENOENT;

	uint32_t cluster = dir_cluster;
	if (cluster == 0)
		cluster = fat32_root_cluster;

	uint8_t sector[512];
	uint8_t *out = (uint8_t *) buf;
	uint64_t written = 0;
	uint64_t entry_idx = 0;
	lfn_state_t lfn;
	lfn.valid = 0;

	while (cluster >= 2 && cluster < FAT32_EOC) {
		uint32_t sec = cluster_to_sector(cluster);
		for (uint32_t s = 0; s < fat32_sec_per_clus; s++) {
			if (fat32_read_sector(sec + s, sector) != 0)
				goto done;

			for (uint32_t e = 0; e < 512 / 32; e++) {
				fat32_dirent_t *de =
				    (fat32_dirent_t *) (sector + e * 32);
				if (de->name[0] == 0x00)
					goto done;
				if (de->name[0] == 0xE5) {
					lfn.valid = 0;
					continue;
				}

				if (de->attr == FAT_ATTR_LFN) {
					lfn_collect(&lfn, (fat32_lfn_t *) de);
					continue;
				}

				if (de->attr & FAT_ATTR_VOLUME_ID) {
					lfn.valid = 0;
					continue;
				}

				/* Skip . and .. */
				if (de->name[0] == '.' && de->name[1] == ' ') {
					lfn.valid = 0;
					continue;
				}
				if (de->name[0] == '.' && de->name[1] == '.'
				    && de->name[2] == ' ') {
					lfn.valid = 0;
					continue;
				}

				if (entry_idx < offset) {
					entry_idx++;
					lfn.valid = 0;
					continue;
				}

				/* Get name: prefer LFN, fallback to short name. */
				char name_buf[256];
				uint32_t name_len;
				if (lfn.valid
				    && lfn_checksum(de->name) == lfn.checksum) {
					for (uint32_t i = 0;
					     i < lfn.len && i < 255; i++)
						name_buf[i] = lfn.name[i];
					name_buf[lfn.len <
						 255 ? lfn.len : 255] = '\0';
					name_len =
					    lfn.len < 255 ? lfn.len : 255;
				} else {
					name_len =
					    fat32_extract_short_name(de,
								     name_buf);
				}
				lfn.valid = 0;

				uint32_t reclen = 19 + name_len + 1;
				reclen = (reclen + 7) & ~7u;

				if (written + reclen > buflen) {
					if (new_offset)
						*new_offset = entry_idx;
					return (int64_t) written;
				}

				struct fat32_linux_dirent64 *ld =
				    (struct fat32_linux_dirent64 *)(out +
								    written);
				uint32_t fc =
				    ((uint32_t) de->
				     first_cluster_hi << 16) | (uint32_t) de->
				    first_cluster_lo;
				ld->d_ino = fc ? fc : 1;
				ld->d_off = entry_idx + 1;
				ld->d_reclen = (uint16_t) reclen;
				ld->d_type =
				    (de->attr & FAT_ATTR_DIRECTORY) ? 4 : 8;

				for (uint32_t i = 0; i < name_len; i++)
					ld->d_name[i] = name_buf[i];
				ld->d_name[name_len] = '\0';

				/* Zero padding. */
				uint32_t end = 19 + name_len + 1;
				while (end < reclen) {
					((uint8_t *) ld)[end] = 0;
					end++;
				}

				written += reclen;
				entry_idx++;
			}
		}
		cluster = fat32_get_fat_entry(cluster);
	}

 done:
	if (new_offset)
		*new_offset = entry_idx;
	return (int64_t) written;
}

/*
 * Write file data starting from the given cluster chain.
 * Extends the chain if needed.  Updates file_size_out with new size.
 * Returns bytes written, or -1 on error.
 */
int
fat32_write_data(uint32_t * start_cluster, uint32_t file_size,
		 uint64_t offset, const void *buf, uint64_t len,
		 uint32_t * file_size_out)
{
	if (!fat32_ready)
		return -1;
	if (len == 0)
		return 0;

	uint32_t cluster_bytes = fat32_sec_per_clus * 512;
	const uint8_t *src = (const uint8_t *)buf;
	uint64_t bytes_written = 0;
	uint32_t new_size = file_size;
	if (offset + len > new_size)
		new_size = (uint32_t) (offset + len);

	/* Allocate first cluster if needed. */
	if (*start_cluster == 0) {
		*start_cluster = fat32_alloc_cluster();
		if (*start_cluster == 0)
			return -1;
	}

	uint32_t cluster_index = (uint32_t) (offset / cluster_bytes);
	uint32_t cluster_offset = (uint32_t) (offset % cluster_bytes);

	/* Navigate to starting cluster, allocating as needed. */
	uint32_t cluster = *start_cluster;
	for (uint32_t i = 0; i < cluster_index; i++) {
		uint32_t next = fat32_get_fat_entry(cluster);
		if (next >= FAT32_EOC || next < 2) {
			/* Allocate new cluster. */
			next = fat32_alloc_cluster();
			if (next == 0)
				return (int)bytes_written;
			fat32_set_fat_entry(cluster, next);
		}
		cluster = next;
	}

	uint8_t sector[512];
	while (bytes_written < len && cluster >= 2 && cluster < FAT32_EOC) {
		uint32_t sec = cluster_to_sector(cluster);
		uint32_t bytes_in_cluster = cluster_bytes - cluster_offset;
		if (bytes_in_cluster > len - bytes_written)
			bytes_in_cluster = (uint32_t) (len - bytes_written);

		uint32_t sec_offset = cluster_offset / 512;
		uint32_t byte_in_sec = cluster_offset % 512;

		while (bytes_in_cluster > 0) {
			/* Read-modify-write if partial sector. */
			if (byte_in_sec != 0 || bytes_in_cluster < 512) {
				if (fat32_read_sector(sec + sec_offset, sector)
				    != 0)
					break;
			}

			uint32_t chunk = 512 - byte_in_sec;
			if (chunk > bytes_in_cluster)
				chunk = bytes_in_cluster;

			for (uint32_t i = 0; i < chunk; i++)
				sector[byte_in_sec + i] =
				    src[bytes_written + i];

			uint32_t abs_lba =
			    (uint32_t) fat32_part_lba + sec + sec_offset;
			if (blkdev_write(fat32_dev, abs_lba, 1, sector) != 0)
				break;

			bytes_written += chunk;
			bytes_in_cluster -= chunk;
			sec_offset++;
			byte_in_sec = 0;
		}

		cluster_offset = 0;

		/* Move to next cluster, allocating if needed. */
		if (bytes_written < len) {
			uint32_t next = fat32_get_fat_entry(cluster);
			if (next >= FAT32_EOC || next < 2) {
				next = fat32_alloc_cluster();
				if (next == 0)
					break;
				fat32_set_fat_entry(cluster, next);
			}
			cluster = next;
		}
	}

	if (file_size_out)
		*file_size_out = new_size;
	return (int)bytes_written;
}

/*
 * Create a new file or directory in a FAT32 directory.
 * Returns the first cluster of the new entry, or 0 on failure.
 * For directories, also creates . and .. entries.
 */
uint32_t
fat32_create_entry(uint32_t dir_cluster, const char *name, uint32_t name_len,
		   int is_dir, uint32_t * out_size)
{
	if (!fat32_ready)
		return 0;

	/* Allocate a cluster for the new entry. */
	uint32_t new_cluster = fat32_alloc_cluster();
	if (new_cluster == 0)
		return 0;

	/* Build the 8.3 name. */
	uint8_t short_name[11];
	for (int i = 0; i < 11; i++)
		short_name[i] = ' ';

	/* Find dot position for extension. */
	uint32_t dot_pos = name_len;
	for (uint32_t i = 0; i < name_len; i++) {
		if (name[i] == '.')
			dot_pos = i;
	}

	/* Fill base name. */
	uint32_t base_len = dot_pos < 8 ? dot_pos : 8;
	for (uint32_t i = 0; i < base_len; i++) {
		char c = name[i];
		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		short_name[i] = (uint8_t) c;
	}

	/* Fill extension. */
	if (dot_pos < name_len) {
		uint32_t ext_start = dot_pos + 1;
		uint32_t ext_len = name_len - ext_start;
		if (ext_len > 3)
			ext_len = 3;
		for (uint32_t i = 0; i < ext_len; i++) {
			char c = name[ext_start + i];
			if (c >= 'a' && c <= 'z')
				c = c - 'a' + 'A';
			short_name[8 + i] = (uint8_t) c;
		}
	}

	/* If creating a directory, create . and .. entries. */
	if (is_dir) {
		uint8_t dir_sector[512];
		for (int i = 0; i < 512; i++)
			dir_sector[i] = 0;

		/* . Entry. */
		fat32_dirent_t *dot = (fat32_dirent_t *) dir_sector;
		dot->name[0] = '.';
		for (int i = 1; i < 11; i++)
			dot->name[i] = ' ';
		dot->attr = FAT_ATTR_DIRECTORY;
		dot->first_cluster_hi = (uint16_t) (new_cluster >> 16);
		dot->first_cluster_lo = (uint16_t) (new_cluster & 0xFFFF);

		/* .. Entry. */
		fat32_dirent_t *dotdot = (fat32_dirent_t *) (dir_sector + 32);
		dotdot->name[0] = '.';
		dotdot->name[1] = '.';
		for (int i = 2; i < 11; i++)
			dotdot->name[i] = ' ';
		dotdot->attr = FAT_ATTR_DIRECTORY;
		uint32_t parent =
		    (dir_cluster == fat32_root_cluster) ? 0 : dir_cluster;
		dotdot->first_cluster_hi = (uint16_t) (parent >> 16);
		dotdot->first_cluster_lo = (uint16_t) (parent & 0xFFFF);

		uint32_t sec = cluster_to_sector(new_cluster);
		uint32_t abs_lba = (uint32_t) fat32_part_lba + sec;
		blkdev_write(fat32_dev, abs_lba, 1, dir_sector);
	}

	/* Find a free slot in the parent directory. */
	uint32_t cluster = dir_cluster;
	if (cluster == 0)
		cluster = fat32_root_cluster;
	uint8_t sector[512];

	while (cluster >= 2 && cluster < FAT32_EOC) {
		uint32_t sec = cluster_to_sector(cluster);
		for (uint32_t s = 0; s < fat32_sec_per_clus; s++) {
			if (fat32_read_sector(sec + s, sector) != 0)
				return 0;

			for (uint32_t e = 0; e < 512 / 32; e++) {
				fat32_dirent_t *de =
				    (fat32_dirent_t *) (sector + e * 32);
				if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
					/* Found free slot -- write our entry. */
					for (int i = 0; i < 32; i++)
						((uint8_t *) de)[i] = 0;
					for (int i = 0; i < 11; i++)
						de->name[i] = short_name[i];
					de->attr =
					    is_dir ? FAT_ATTR_DIRECTORY :
					    FAT_ATTR_ARCHIVE;
					de->first_cluster_hi =
					    (uint16_t) (new_cluster >> 16);
					de->first_cluster_lo =
					    (uint16_t) (new_cluster & 0xFFFF);
					de->file_size = 0;

					uint32_t abs_lba =
					    (uint32_t) fat32_part_lba + sec + s;
					blkdev_write(fat32_dev, abs_lba, 1,
						     sector);

					if (out_size)
						*out_size = 0;
					return new_cluster;
				}
			}
		}

		uint32_t next = fat32_get_fat_entry(cluster);
		if (next >= FAT32_EOC || next < 2) {
			/* Extend directory. */
			next = fat32_alloc_cluster();
			if (next == 0)
				return 0;
			fat32_set_fat_entry(cluster, next);
		}
		cluster = next;
	}
	return 0;
}

/*
 * Update the size field of a directory entry.
 * Searches the parent directory for the entry with matching first_cluster.
 */
int
fat32_update_dirent_size(uint32_t dir_cluster, uint32_t entry_cluster,
			 uint32_t new_size)
{
	if (!fat32_ready)
		return -1;

	uint32_t cluster = dir_cluster;
	if (cluster == 0)
		cluster = fat32_root_cluster;
	uint8_t sector[512];

	while (cluster >= 2 && cluster < FAT32_EOC) {
		uint32_t sec = cluster_to_sector(cluster);
		for (uint32_t s = 0; s < fat32_sec_per_clus; s++) {
			if (fat32_read_sector(sec + s, sector) != 0)
				return -1;

			for (uint32_t e = 0; e < 512 / 32; e++) {
				fat32_dirent_t *de =
				    (fat32_dirent_t *) (sector + e * 32);
				if (de->name[0] == 0x00)
					return -1;
				if (de->name[0] == 0xE5)
					continue;
				if (de->attr == FAT_ATTR_LFN
				    || (de->attr & FAT_ATTR_VOLUME_ID))
					continue;

				uint32_t fc =
				    ((uint32_t) de->
				     first_cluster_hi << 16) | (uint32_t) de->
				    first_cluster_lo;
				if (fc == entry_cluster) {
					de->file_size = new_size;
					uint32_t abs_lba =
					    (uint32_t) fat32_part_lba + sec + s;
					blkdev_write(fat32_dev, abs_lba, 1,
						     sector);
					return 0;
				}
			}
		}
		cluster = fat32_get_fat_entry(cluster);
	}
	return -1;
}
