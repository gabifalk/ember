/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_BUG_H
#define EMBER_BUG_H

#include <ember/cpu.h>
#include <ember/io.h>

/*
 * Runtime invariant checks matching Promela model assertions.
 * Prints via direct serial (no console lock) to avoid deadlock
 * in the panic path, then halts.
 */

static inline void
bug_serial_str(const char *s)
{
	for (; *s; s++) {
		while (!(inb(0x3F8 + 5) & 0x20)) ;
		outb(0x3F8, *s);
	}
}

static inline void
bug_serial_hex(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	bug_serial_str("0x");
	for (int i = 60; i >= 0; i -= 4) {
		while (!(inb(0x3F8 + 5) & 0x20)) ;
		outb(0x3F8, hex[(v >> i) & 0xF]);
	}
}

#define BUG_ON(cond) do { \
    if (__builtin_expect(!!(cond), 0)) { \
        bug_serial_str("\nBUG: "); \
        bug_serial_str(__FILE__); \
        bug_serial_str(":"); \
        bug_serial_hex(__LINE__); \
        bug_serial_str(" cpu"); \
        bug_serial_hex(this_cpu_id()); \
        { \
            extern volatile int bkl_holder_cpu; \
            bug_serial_str(" bkl_holder="); \
            bug_serial_hex((uint64_t)(int64_t)bkl_holder_cpu); \
        } \
        bug_serial_str(" "); \
        bug_serial_str(#cond); \
        bug_serial_str("\n"); \
        for (;;) __asm__ __volatile__("cli; hlt"); \
    } \
} while (0)

#endif				/* EMBER_BUG_H. */
