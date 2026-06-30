/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_NET_ETH_H
#define EMBER_NET_ETH_H

#include <stdint.h>
#include "ember/netdev.h"

#define ETH_HLEN 14
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

/* L2 demux: registered as the netdev rx_handler by net_init(). */
void ether_input(netdev_t *nd, const void *frame, uint16_t len);

/* Build an Ethernet frame for dst_ip (resolved via the ARP cache) carrying
 * l3[0..l3_len) as the given ethertype, and transmit it. Returns 0 on success,
 * -1 if unresolved, oversized, or the TX ring is full. */
int eth_output(netdev_t *nd, uint32_t dst_ip, uint16_t ethertype,
	       const uint8_t *l3, uint16_t l3_len);

/* Install ether_input as the netdev rx_handler. Call after e1000_init(). */
void net_init(void);

#endif				/* EMBER_NET_ETH_H. */
