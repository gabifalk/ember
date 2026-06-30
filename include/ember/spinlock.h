/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_SPINLOCK_H
#define EMBER_SPINLOCK_H

#include <stdint.h>

/*
 * SMP-safe spinlock: cli + xchg-based mutual exclusion.
 * cli disables local interrupts, xchg excludes other CPUs.
 */
typedef struct {
	volatile int locked;
	uint64_t flags;
} spinlock_t;
#define SPINLOCK_INIT {0, 0}
static inline void
spin_init(spinlock_t * s)
{
	s->locked = 0;
	s->flags = 0;
}

static inline void
spin_lock(spinlock_t * s)
{
	uint64_t f;
	__asm__ __volatile__("pushfq; pop %0; cli":"=r"(f)::"memory");
	while (1) {
		int old;
		__asm__ __volatile__("xchgl %0, %1":"=r"(old), "+m"(s->locked)
				     :"0"(1)
				     :"memory");
		if (!old)
			break;
		while (s->locked)
			__asm__ __volatile__("pause":::"memory");
	}
	s->flags = f;
}

static inline void
spin_unlock(spinlock_t * s)
{
	uint64_t f = s->flags;
	__asm__ __volatile__("":::"memory");
	s->locked = 0;
	if (f & 0x200)
		__asm__ __volatile__("sti":::"memory");
}

static inline uint64_t
spin_lock_irqsave(spinlock_t * s)
{
	uint64_t flags;
	__asm__ __volatile__("pushfq; pop %0; cli":"=r"(flags)::"memory");
	while (1) {
		int old;
		__asm__ __volatile__("xchgl %0, %1":"=r"(old), "+m"(s->locked)
				     :"0"(1)
				     :"memory");
		if (!old)
			break;
		while (s->locked)
			__asm__ __volatile__("pause":::"memory");
	}
	return flags;
}

static inline void
spin_unlock_irqrestore(spinlock_t * s, uint64_t flags)
{
	__asm__ __volatile__("":::"memory");
	s->locked = 0;
	if (flags & 0x200)
		__asm__ __volatile__("sti":::"memory");
}

/* -- SMP-safe atomic spinlock (xchg-based, unfair) ---------------- */

typedef struct {
	volatile int locked;
} atomic_spinlock_t;

#define ATOMIC_SPINLOCK_INIT { .locked = 0 }

static inline int
atomic_spin_trylock(atomic_spinlock_t * l)
{
	int old;
	__asm__ __volatile__("xchgl %0, %1":"=r"(old), "+m"(l->locked)
			     :"0"(1)
			     :"memory");
	return !old;		/* 1 = Acquired, 0 = busy. */
}

static inline void
atomic_spin_lock(atomic_spinlock_t * l)
{
	while (1) {
		int old;
		__asm__ __volatile__("xchgl %0, %1":"=r"(old), "+m"(l->locked)
				     :"0"(1)
				     :"memory");
		if (!old)
			break;
		/* Spin on read (reduces bus traffic vs repeated xchg) */
		while (l->locked)
			__asm__ __volatile__("pause":::"memory");
	}
}

static inline void
atomic_spin_unlock(atomic_spinlock_t * l)
{
	__asm__ __volatile__("":::"memory");	/* Compiler barrier. */
	l->locked = 0;
}

#endif				/* EMBER_SPINLOCK_H. */
