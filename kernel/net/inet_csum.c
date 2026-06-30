/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/net/inet.h"

uint16_t
inet_csum(const void *buf, uint16_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += net_get16(p);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (uint32_t)p[0] << 8;	/* pad odd byte with zero */
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}
