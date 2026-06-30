/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_TIME_H
#define EMBER_TIME_H
#include <stdint.h>
#define KERNEL_HZ 100
#define EPOCH_BASE 1700000000ULL
extern volatile uint64_t kernel_ticks;
static inline uint64_t
kernel_time_sec(void)
{
	return EPOCH_BASE + kernel_ticks / KERNEL_HZ;
}

static inline uint64_t
kernel_time_nsec(void)
{
	return (kernel_ticks % KERNEL_HZ) * 10000000ULL;
}
#endif
