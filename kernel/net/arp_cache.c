/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Per-interface ARP cache. Scan-first insert (update in place), first free
 * slot, else round-robin eviction. BKL-serialized: every caller runs in the
 * e1000 IRQ. */
#include <stdint.h>
#include "ember/net/arp.h"
#include "ember/netdev.h"

static void
copy_mac(uint8_t dst[6], const uint8_t src[6])
{
	for (int i = 0; i < 6; i++)
		dst[i] = src[i];
}

void
arp_cache_reset(netdev_t *nd)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++)
		nd->arp_cache[i].valid = 0;
	nd->arp_victim = 0;
}

void
arp_cache_insert(netdev_t *nd, uint32_t ip, const uint8_t mac[6])
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (nd->arp_cache[i].valid && nd->arp_cache[i].ip == ip) {
			copy_mac(nd->arp_cache[i].mac, mac);
			return;
		}
	}
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!nd->arp_cache[i].valid) {
			nd->arp_cache[i].valid = 1;
			nd->arp_cache[i].ip = ip;
			copy_mac(nd->arp_cache[i].mac, mac);
			return;
		}
	}
	nd->arp_cache[nd->arp_victim].ip = ip;
	copy_mac(nd->arp_cache[nd->arp_victim].mac, mac);
	nd->arp_cache[nd->arp_victim].valid = 1;
	nd->arp_victim = (uint16_t)((nd->arp_victim + 1) % ARP_CACHE_SIZE);
}

const uint8_t *
arp_cache_lookup(netdev_t *nd, uint32_t ip)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (nd->arp_cache[i].valid && nd->arp_cache[i].ip == ip)
			return nd->arp_cache[i].mac;
	}
	return 0;
}
