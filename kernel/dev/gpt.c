/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * GPT partition table parser.
 * Finds the EFI System Partition by scanning the GPT partition entries.
 */

#include <stdint.h>

#include "ember/gpt.h"
#include "ember/blkdev.h"
#include "ember/console.h"

/* GPT header (LBA 1) -- we only need a few fields. */
typedef struct {
	uint64_t signature;	/* "EFI PART" = 0x5452415020494645. */
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t disk_guid[16];
	uint64_t partition_entry_lba;
	uint32_t num_partition_entries;
	uint32_t partition_entry_size;
	uint32_t partition_array_crc32;
} __attribute__ ((packed)) gpt_header_t;

#define GPT_SIGNATURE 0x5452415020494645ULL	/* "EFI PART". */

/* ESP type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B (mixed-endian) */
static const uint8_t esp_type_guid[16] = {
	0x28, 0x73, 0x2A, 0xC1,	/* Data1 LE. */
	0x1F, 0xF8,		/* Data2 LE. */
	0xD2, 0x11,		/* Data3 LE. */
	0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static int
guid_eq(const uint8_t * a, const uint8_t * b)
{
	for (int i = 0; i < 16; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

static void
hex8(uint64_t v, char *buf)
{
	for (int i = 15; i >= 0; i--) {
		int d = (int)(v & 0xf);
		buf[i] = (d < 10) ? ('0' + d) : ('a' + d - 10);
		v >>= 4;
	}
	buf[16] = '\0';
}

int
gpt_find_esp(int dev, uint64_t * start_lba, uint64_t * size_lba)
{
	uint8_t sector[512];

	/* Read GPT header at LBA 1. */
	if (blkdev_read(dev, 1, 1, sector) != 0) {
		console_write("gpt: failed to read LBA 1\n");
		return -1;
	}

	gpt_header_t *hdr = (gpt_header_t *) sector;
	if (hdr->signature != GPT_SIGNATURE) {
		console_write("gpt: bad signature\n");
		return -1;
	}

	uint32_t num_entries = hdr->num_partition_entries;
	uint32_t entry_size = hdr->partition_entry_size;
	uint64_t entry_lba = hdr->partition_entry_lba;

	if (entry_size < 128 || entry_size > 512) {
		console_write("gpt: unsupported entry size\n");
		return -1;
	}

	/* Scan partition entries. */
	uint32_t entries_per_sector = 512 / entry_size;
	uint32_t sectors_needed =
	    (num_entries + entries_per_sector - 1) / entries_per_sector;

	/* Limit to 128 entries (32 sectors) for safety. */
	if (sectors_needed > 32)
		sectors_needed = 32;

	for (uint32_t s = 0; s < sectors_needed; s++) {
		if (blkdev_read(dev, (uint32_t) (entry_lba + s), 1, sector) !=
		    0) {
			console_write
			    ("gpt: failed to read partition entry sector\n");
			return -1;
		}

		for (uint32_t e = 0; e < entries_per_sector; e++) {
			uint32_t idx = s * entries_per_sector + e;
			if (idx >= num_entries)
				break;

			gpt_entry_t *ent =
			    (gpt_entry_t *) (sector + e * entry_size);

			/* Skip empty entries (all-zero type GUID) */
			int empty = 1;
			for (int i = 0; i < 16; i++) {
				if (ent->type_guid[i] != 0) {
					empty = 0;
					break;
				}
			}
			if (empty)
				continue;

			if (guid_eq(ent->type_guid, esp_type_guid)) {
				*start_lba = ent->first_lba;
				*size_lba = ent->last_lba - ent->first_lba + 1;

				char buf[32];
				console_write("gpt: ESP found at LBA ");
				hex8(*start_lba, buf);
				console_write(buf);
				console_write(" size ");
				hex8(*size_lba, buf);
				console_write(buf);
				console_write("\n");
				return 0;
			}
		}
	}

	console_write("gpt: no ESP found\n");
	return -1;
}
