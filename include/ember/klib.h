/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_KLIB_H
#define EMBER_KLIB_H

#include <stdint.h>

/* String comparison: returns 1 if equal, 0 otherwise. */
static inline int
kstreq(const char *a, const char *b)
{
	int i;
	for (i = 0; a[i] && b[i]; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return a[i] == b[i];
}

/* String length. */
static inline uint64_t
kstrlen(const char *s)
{
	uint64_t len = 0;
	while (s[len])
		len++;
	return len;
}

/* String copy (including NUL). */
static inline void
kstrcpy(char *dst, const char *src)
{
	while ((*dst++ = *src++)) ;
}

/*
 * Standard memory functions -- tcc emits calls to these for struct
 * zeroing, assignment, etc.  GCC inlines them, but they must exist
 * as real symbols for tcc.  Defined in kernel/klib.c.
 */
void *memset(void *s, int c, uint64_t n);
void *memcpy(void *dst, const void *src, uint64_t n);
void *memmove(void *dst, const void *src, uint64_t n);

#endif
