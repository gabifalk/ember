/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_DESC_H
#define EMBER_DESC_H

/* Access-byte bits: P and DPL apply to any descriptor or gate. */
#define GDT_P		(1 << 7)	/* present */
#define GDT_DPL(n)	((n) << 5)	/* ring 0..3 */
/* Code/data segment access-byte bits (S=1 family). */
#define GDT_S		(1 << 4)	/* code/data, not system */
#define GDT_EXEC	(1 << 3)	/* code (else data) */
#define GDT_RW		(1 << 1)	/* code: readable; data: writable */
/* GDT flags-nibble bits (descriptor bits 52..55). */
#define GDT_LONG	(1ULL << 53)	/* 64-bit code segment */
#define GDT_GRAN	(1ULL << 55)	/* 4 KiB limit granularity */

enum desc_type {
	DESC_TSS_AVAIL = 0x9,
	DESC_INT_GATE  = 0xE,
};

#endif
