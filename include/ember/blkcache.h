/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_BLKCACHE_H
#define EMBER_BLKCACHE_H

#include <stdint.h>

#define BLKCACHE_MAX_BLOCK_SIZE 4096

void blkcache_init(void);
void blkcache_set_block_size(int dev, uint32_t bs);
uint32_t blkcache_get_block_size(int dev);
int blkcache_read(int dev, uint32_t block_num, void *out_buf);
int blkcache_write(int dev, uint32_t block_num, const void *data);
void blkcache_invalidate(void);
void blkcache_discard(int dev, uint32_t block_num);
void blkcache_sync(int dev);
void blkcache_prefetch(int dev, const uint32_t * blocks, uint32_t count);

#endif
