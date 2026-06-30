/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Memory-backed block device for ext2 initrd.
 */

#include <stdint.h>

#include "ember/ramdisk.h"
#include "ember/blkdev.h"
#include "ember/mmu.h"

#define SECTOR_SIZE 512

static uint8_t *ramdisk_base;	/* HHDM virtual address. */
static uint64_t ramdisk_sectors;
static int ramdisk_ready;
static int ramdisk_devno = -1;

static int
ramdisk_read_blocks(uint32_t lba, uint8_t count, void *buf)
{
	uint64_t end = (uint64_t) lba + count;
	if (end > ramdisk_sectors)
		return -1;
	uint8_t *src = ramdisk_base + (uint64_t) lba * SECTOR_SIZE;
	uint8_t *dst = (uint8_t *) buf;
	uint64_t bytes = (uint64_t) count * SECTOR_SIZE;
	for (uint64_t i = 0; i < bytes; i++)
		dst[i] = src[i];
	return 0;
}

static int
ramdisk_write_blocks(uint32_t lba, uint8_t count, const void *buf)
{
	uint64_t end = (uint64_t) lba + count;
	if (end > ramdisk_sectors)
		return -1;
	uint8_t *dst = ramdisk_base + (uint64_t) lba * SECTOR_SIZE;
	const uint8_t *src = (const uint8_t *)buf;
	uint64_t bytes = (uint64_t) count * SECTOR_SIZE;
	for (uint64_t i = 0; i < bytes; i++)
		dst[i] = src[i];
	return 0;
}

static int
ramdisk_probe(void)
{
	return ramdisk_ready ? 0 : -1;
}

static const blkdev_ops_t ramdisk_ops = {
	.name = "ramdisk",
	.probe = ramdisk_probe,
	.read_blocks = ramdisk_read_blocks,
	.write_blocks = ramdisk_write_blocks,
};

int
ramdisk_init(uint64_t phys_base, uint64_t size_bytes)
{
	if (size_bytes < SECTOR_SIZE)
		return -1;
	ramdisk_base = (uint8_t *) phys_to_virt(phys_base);
	ramdisk_sectors = size_bytes / SECTOR_SIZE;
	ramdisk_ready = 1;
	ramdisk_devno = blkdev_register_device(&ramdisk_ops);
	return ramdisk_devno;
}

int
ramdisk_is_ready(void)
{
	return ramdisk_ready;
}

int
ramdisk_dev(void)
{
	return ramdisk_devno;
}
