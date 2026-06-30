/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_NET_ARP_H
#define EMBER_NET_ARP_H

#include <stdint.h>
#include "ember/netdev.h"

/* Cache: scan-first insert (update in place), round-robin eviction. */
void arp_cache_insert(netdev_t *nd, uint32_t ip, const uint8_t mac[6]);
const uint8_t *arp_cache_lookup(netdev_t *nd, uint32_t ip);	/* 6-byte MAC, or 0 on miss */
void arp_cache_reset(netdev_t *nd);				/* clear this device's entries */

/* Handle an inbound ARP payload (Ethernet header already stripped). */
void arp_input(netdev_t *nd, const uint8_t *arp, uint16_t len);

#endif				/* EMBER_NET_ARP_H. */
