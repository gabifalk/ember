/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_GPT_H
#define EMBER_GPT_H

#include <stdint.h>

/* GPT partition entry (128 bytes) */
typedef struct {
	uint8_t type_guid[16];
	uint8_t unique_guid[16];
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[36];	/* UTF-16LE. */
} __attribute__ ((packed)) gpt_entry_t;

/*
 * Find the EFI System Partition.
 * Returns 0 on success, -1 if not found.
 * On success, *start_lba and *size_lba are filled in.
 */
int gpt_find_esp(int blkdev_index, uint64_t * start_lba, uint64_t * size_lba);

#endif
