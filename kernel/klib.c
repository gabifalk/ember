/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

/*
 * Standard memory functions required by tcc.
 *
 * tcc emits calls to memset/memcpy/memmove for struct zeroing and
 * assignment.  GCC inlines these, but they must exist as real symbols
 * when building with tcc.
 */

#include <stdint.h>

void *
memset(void *s, int c, uint64_t n)
{
	uint8_t *p = (uint8_t *)s;
	while (n--)
		*p++ = (uint8_t)c;
	return s;
}

void *
memcpy(void *dst, const void *src, uint64_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *
memmove(void *dst, const void *src, uint64_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}
