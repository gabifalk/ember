/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_RAMDISK_H
#define EMBER_RAMDISK_H

#include <stdint.h>

/*
 * Initialize a memory-backed block device.
 * phys_base: physical address of the RAM region (HHDM-mapped).
 * size_bytes: size of the region.
 * Returns blkdev device index, or -1 on error.
 */
int ramdisk_init(uint64_t phys_base, uint64_t size_bytes);

/* Check if ramdisk is ready. */
int ramdisk_is_ready(void);

/* Returns the blkdev device index, or -1 if not initialized. */
int ramdisk_dev(void);

#endif
