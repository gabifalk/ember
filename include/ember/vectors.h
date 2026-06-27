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
#define IRQ_VECTOR_BASE		32	/* remapped IRQs start here (0..31 = exceptions) */
#define VEC_TIMER		IRQ_VECTOR_BASE	/* timer is IRQ0 */
#define VEC_SCHED_KICK		0x40	/* IPI: wake idle CPUs to reschedule */
#define VEC_TLB_SHOOTDOWN	0x41	/* IPI: remote TLB flush */
#define VEC_IRQ_BASE		0x42	/* device IRQ vectors: 0x42..0x4f */
#define VEC_IRQ_MAX		0x4f
#define VEC_IRQ_SELFTEST	VEC_IRQ_BASE	/* used by irq_selftest() */

#endif				/* EMBER_VECTORS_H. */
