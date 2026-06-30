/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_NET_IPV4_H
#define EMBER_NET_IPV4_H

#include <stdint.h>
#include "ember/netdev.h"

#define IPPROTO_ICMP 1
#define OUR_IP 0x0A00020FU		/* 10.0.2.15: default host IPv4, assigned to
					   the e1000 at init (network numeric order) */

/* Build an IPv4 packet (src=src_ip, ttl=64) carrying payload and transmit it
 * via eth_output. Returns eth_output's result. */
int ipv4_output(netdev_t *nd, uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
		const uint8_t *payload, uint16_t payload_len);

/* Handle an inbound IPv4 packet (Ethernet header already stripped). */
void ipv4_input(netdev_t *nd, const uint8_t *pkt, uint16_t len);

#endif				/* EMBER_NET_IPV4_H. */
