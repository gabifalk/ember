/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Host regression test for the netdev address set and device registry. */
#include <stdint.h>
#include <stdio.h>
#include "ember/netdev.h"

int
main(void)
{
	netdev_t a = {0}, b = {0};

	netdev_add_addr(&a, 0x0A000001);
	netdev_add_addr(&a, 0x0A000002);
	if (!netdev_has_addr(&a, 0x0A000001)) { printf("FAIL: addr1 missing\n"); return 1; }
	if (!netdev_has_addr(&a, 0x0A000002)) { printf("FAIL: addr2 missing\n"); return 1; }
	if (netdev_has_addr(&a, 0x0A0000FF))  { printf("FAIL: bogus addr hit\n"); return 1; }
	if (a.naddr != 2) { printf("FAIL: naddr=%u expected 2\n", a.naddr); return 1; }

	/* Cap at IFADDR_MAX: extra adds do not overflow. */
	for (int i = 0; i < IFADDR_MAX + 3; i++)
		netdev_add_addr(&b, 0x0B000000u + (uint32_t)i);
	if (b.naddr != IFADDR_MAX) { printf("FAIL: naddr=%u expected %u\n", b.naddr, IFADDR_MAX); return 1; }

	/* Registry: register links into the list, netdev_first walks it. */
	netdev_register(&a);
	netdev_register(&b);
	int seen_a = 0, seen_b = 0;
	for (netdev_t *nd = netdev_first(); nd; nd = nd->next) {
		if (nd == &a) seen_a = 1;
		if (nd == &b) seen_b = 1;
	}
	if (!seen_a || !seen_b) { printf("FAIL: registry missing a device\n"); return 1; }

	printf("test_netdev: passed\n");
	return 0;
}
