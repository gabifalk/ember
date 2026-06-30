/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/pipe.h"
#include "ember/heap.h"
#include "ember/sched.h"
#include "ember/spinlock.h"

static spinlock_t pipe_chan_lock = SPINLOCK_INIT;
static int next_pipe_chan = 1;

pipe_t *
pipe_create(void)
{
	pipe_t *p = (pipe_t *) kmalloc(sizeof(pipe_t));
	if (!p)
		return 0;
	p->read_pos = 0;
	p->write_pos = 0;
	p->count = 0;
	p->readers = 1;
	p->writers = 1;
	spin_init(&p->lock);
	spin_lock(&pipe_chan_lock);
	p->wake_chan = next_pipe_chan++;
	spin_unlock(&pipe_chan_lock);
	return p;
}

static inline void
pipe_memcpy(void *dst, const void *src, uint64_t n)
{
	kmemcpy(dst, src, n);
}

uint64_t
pipe_read(pipe_t * p, void *buf, uint64_t len)
{
	if (!p)
		return 0;
	spin_lock(&p->lock);
	if (p->count == 0) {
		spin_unlock(&p->lock);
		return 0;
	}
	uint8_t *dst = (uint8_t *) buf;
	uint64_t to_read = len < p->count ? len : p->count;
	uint64_t chunk1 = PIPE_BUF_SIZE - p->read_pos;
	if (chunk1 > to_read)
		chunk1 = to_read;
	pipe_memcpy(dst, &p->buf[p->read_pos], chunk1);
	uint64_t chunk2 = to_read - chunk1;
	if (chunk2 > 0)
		pipe_memcpy(dst + chunk1, &p->buf[0], chunk2);
	p->read_pos = (p->read_pos + to_read) % PIPE_BUF_SIZE;
	p->count -= to_read;
	spin_unlock(&p->lock);
	return to_read;
}

uint64_t
pipe_write(pipe_t * p, const void *buf, uint64_t len)
{
	if (!p)
		return 0;
	spin_lock(&p->lock);
	const uint8_t *src = (const uint8_t *)buf;
	uint64_t avail = PIPE_BUF_SIZE - p->count;
	uint64_t to_write = len < avail ? len : avail;
	uint64_t chunk1 = PIPE_BUF_SIZE - p->write_pos;
	if (chunk1 > to_write)
		chunk1 = to_write;
	pipe_memcpy(&p->buf[p->write_pos], src, chunk1);
	uint64_t chunk2 = to_write - chunk1;
	if (chunk2 > 0)
		pipe_memcpy(&p->buf[0], src + chunk1, chunk2);
	p->write_pos = (p->write_pos + to_write) % PIPE_BUF_SIZE;
	p->count += to_write;
	spin_unlock(&p->lock);
	return to_write;
}

void
pipe_close_read(pipe_t * p)
{
	if (!p)
		return;
	spin_lock(&p->lock);
	p->readers--;
	int dead = (p->readers <= 0 && p->writers <= 0);
	spin_unlock(&p->lock);
	if (dead) {
		kfree(p);
		return;
	}
	sched_wakeup(p->wake_chan);
}

void
pipe_close_write(pipe_t * p)
{
	if (!p)
		return;
	spin_lock(&p->lock);
	p->writers--;
	int dead = (p->readers <= 0 && p->writers <= 0);
	spin_unlock(&p->lock);
	if (dead) {
		kfree(p);
		return;
	}
	sched_wakeup(p->wake_chan);
}
