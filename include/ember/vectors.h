/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_VECTORS_H
#define EMBER_VECTORS_H

/* CPU exception vectors (Intel-defined, 0..31). */
#define VEC_DE	0		/* divide error */
#define VEC_DB	1		/* debug */
#define VEC_BP	3		/* breakpoint */
#define VEC_UD	6		/* invalid opcode */
#define VEC_DF	8		/* double fault */
#define VEC_NP	11		/* segment not present */
#define VEC_SS	12		/* stack-segment fault */
#define VEC_GP	13		/* general protection */
#define VEC_PF	14		/* page fault */

/* IRQ / IPI vectors (Ember allocation, 32..255). */
#define VEC_TIMER		32	/* LAPIC/PIT timer */
#define VEC_SCHED_KICK		0x40	/* IPI: wake idle CPUs to reschedule */
#define VEC_TLB_SHOOTDOWN	0x41	/* IPI: remote TLB flush */

#endif				/* EMBER_VECTORS_H. */
