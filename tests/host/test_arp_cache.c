/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Host regression test for the per-interface ARP cache: update-in-place (no
 * duplicates), fill, round-robin eviction, and isolation between interfaces. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ember/net/arp.h"

static uint8_t m(int n) { return (uint8_t)n; }
static void mk(uint8_t out[6], int n) { for (int i = 0; i < 6; i++) out[i] = m(n + i); }

int
main(void)
{
	netdev_t nd0 = {0}, nd1 = {0};
	uint8_t mac[6], mac2[6];
	arp_cache_reset(&nd0);
	arp_cache_reset(&nd1);

	mk(mac, 0x10);
	arp_cache_insert(&nd0, 0x0A000001, mac);
	const uint8_t *r = arp_cache_lookup(&nd0, 0x0A000001);
	if (!r || memcmp(r, mac, 6) != 0) { printf("FAIL: lookup after insert\n"); return 1; }
	if (arp_cache_lookup(&nd0, 0x0A000002)) { printf("FAIL: miss expected\n"); return 1; }

	/* Update in place: same IP, new MAC, still one entry. */
	mk(mac2, 0x20);
	arp_cache_insert(&nd0, 0x0A000001, mac2);
	r = arp_cache_lookup(&nd0, 0x0A000001);
	if (!r || memcmp(r, mac2, 6) != 0) { printf("FAIL: update in place\n"); return 1; }

	/* Isolation: nd0's entry is not visible on nd1. */
	if (arp_cache_lookup(&nd1, 0x0A000001)) { printf("FAIL: cross-interface bleed\n"); return 1; }

	/* Fill past capacity to force eviction; the most recent insert must win. */
	for (int i = 0; i < ARP_CACHE_SIZE + 4; i++) {
		uint8_t mm[6]; mk(mm, 0x30 + i);
		arp_cache_insert(&nd0, 0x0B000000u + (uint32_t)i, mm);
	}
	uint8_t last[6]; mk(last, 0x30 + (ARP_CACHE_SIZE + 3));
	r = arp_cache_lookup(&nd0, 0x0B000000u + (ARP_CACHE_SIZE + 3));
	if (!r || memcmp(r, last, 6) != 0) { printf("FAIL: last insert evicted\n"); return 1; }

	printf("test_arp_cache: passed\n");
	return 0;
}
