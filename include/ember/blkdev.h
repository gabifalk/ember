/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_BLKDEV_H
#define EMBER_BLKDEV_H

#include <stdint.h>

#define BLKDEV_MAX 8

typedef struct blkdev_ops {
	const char *name;
	int (*probe) (void);
	int (*read_blocks) (uint32_t lba, uint8_t count, void *buf);
	int (*write_blocks) (uint32_t lba, uint8_t count, const void *buf);
	int (*flush) (void);
} blkdev_ops_t;

/* Register built-in block drivers. */
void blkdev_init(void);

/* Probe registered drivers; each success becomes a device. Returns device count. */
int blkdev_probe_all(void);

/* Directly register a pre-probed device. Returns device index, or -1 on error. */
int blkdev_register_device(const blkdev_ops_t * ops);

/* Read/write/flush via a specific device. */
int blkdev_read(int dev, uint32_t lba, uint8_t count, void *buf);
int blkdev_write(int dev, uint32_t lba, uint8_t count, const void *buf);
int blkdev_flush(int dev);

/* Number of active devices. */
int blkdev_count(void);

/* Name of device N. */
const char *blkdev_name(int dev);

#endif
