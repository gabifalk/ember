/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_NET_INET_H
#define EMBER_NET_INET_H

#include <stdint.h>

/* RFC 1071 one's-complement checksum. Returns the 16-bit result as a number;
 * store it big-endian with net_put16. Over a buffer whose checksum field is
 * already filled, a correct packet sums to 0. No pseudo-header is added: TCP/
 * UDP callers must prepend their own. */
uint16_t inet_csum(const void *buf, uint16_t len);

/* Unaligned big-endian (network order) accessors. */
static inline uint16_t net_get16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t net_get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void net_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static inline void net_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

#endif				/* EMBER_NET_INET_H. */
