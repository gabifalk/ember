/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_PIPE_H
#define EMBER_PIPE_H

#include <stdint.h>
#include "ember/spinlock.h"

#define PIPE_BUF_SIZE 65536

typedef struct pipe {
	uint8_t buf[PIPE_BUF_SIZE];
	uint32_t read_pos;
	uint32_t write_pos;
	uint32_t count;
	int readers;		/* Number of open read ends. */
	int writers;		/* Number of open write ends. */
	int wake_chan;		/* Unique channel for sleep/wakeup. */
	spinlock_t lock;
} pipe_t;

pipe_t *pipe_create(void);
uint64_t pipe_read(pipe_t * p, void *buf, uint64_t len);
uint64_t pipe_write(pipe_t * p, const void *buf, uint64_t len);
void pipe_close_read(pipe_t * p);
void pipe_close_write(pipe_t * p);

#endif
