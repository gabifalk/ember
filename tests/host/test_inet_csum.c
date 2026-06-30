/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Host regression test for inet_csum (compile with the system cc, not the
 * kernel toolchain). Uses the canonical RFC 1071 / textbook IPv4 header. */
#include <stdint.h>
#include <stdio.h>
#include "ember/net/inet.h"

int
main(void)
{
	/* Canonical 20-byte IPv4 header with the checksum field zeroed.
	 * Expected one's-complement checksum: 0xb861. */
	uint8_t hdr[20] = {
		0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
		0x40, 0x11, 0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01,
		0xc0, 0xa8, 0x00, 0xc7
	};
	uint16_t c = inet_csum(hdr, 20);
	if (c != 0xb861) {
		printf("FAIL: csum=0x%04x expected 0xb861\n", c);
		return 1;
	}
	/* Filling the field and re-summing yields 0 for a valid packet. */
	net_put16(hdr + 10, c);
	if (inet_csum(hdr, 20) != 0) {
		printf("FAIL: valid packet did not sum to 0\n");
		return 1;
	}
	/* Odd length: last byte padded with zero. Sum 0x00ff + 0xff00 = 0xffff,
	 * complemented to 0x0000. */
	uint8_t odd[3] = { 0x00, 0xff, 0xff };
	if (inet_csum(odd, 3) != 0x0000) {
		printf("FAIL: odd-length csum wrong\n");
		return 1;
	}
	printf("test_inet_csum: passed\n");
	return 0;
}
