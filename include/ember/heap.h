/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_HEAP_H
#define EMBER_HEAP_H

#include <stdint.h>

void heap_init(void);
void *kmalloc(uint64_t size);
void *kzalloc(uint64_t size);
void kfree(void *ptr);
void *kmemcpy(void *dst, const void *src, uint64_t n);
void *kmemzero(void *dst, uint64_t n);

#endif
