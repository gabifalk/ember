/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_SCHED_H
#define EMBER_SCHED_H

#include "ember/spinlock.h"

extern spinlock_t sched_lock;

/*
 * Wake channel for timer-based sleeps (nanosleep, pause).
 * timer_handler calls sched_wakeup(SCHED_TICK_CHAN) every tick.
 */
#define SCHED_TICK_CHAN 0x7FFFFF01

void sched_init(void);
void sched_init_idle(int cpu);
void sched_note_slot(int idx);
void schedule(void);
void sched_yield(void);
void sched_sleep(int chan);
void sched_wakeup(int chan);
int sched_wakeup_n(int chan, int max_wake);
int sched_any_work(void);

#endif
