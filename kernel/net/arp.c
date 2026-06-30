/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* ARP responder. Learns the sender of every valid ARP into the cache, and
 * answers requests for any of the device's addresses (netdev_has_addr). The
 * reply is built fresh (no reflection) and sent via eth_output, which
 * resolves the requester's MAC from the entry we just learned. IRQ, BKL. */
#include <stdint.h>
#include "ember/net/arp.h"
#include "ember/net/eth.h"
#include "ember/net/inet.h"
#include "ember/netdev.h"

#define ARP_HLEN 28
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

void
arp_input(netdev_t *nd, const uint8_t *a, uint16_t len)
{
	if (len < ARP_HLEN)
		return;
	if (net_get16(a + 0) != 1)			/* HTYPE Ethernet */
		return;
	if (net_get16(a + 2) != ETHERTYPE_IPV4)		/* PTYPE IPv4 */
		return;
	if (a[4] != 6 || a[5] != 4)			/* HLEN/PLEN */
		return;

	uint16_t op = net_get16(a + 6);
	uint32_t spa = net_get32(a + 14);
	arp_cache_insert(nd, spa, a + 8);		/* learn the sender */

	if (op != ARP_OP_REQUEST)
		return;
	uint32_t tpa = net_get32(a + 24);
	if (!netdev_has_addr(nd, tpa))			/* TPA: not one of ours */
		return;

	uint8_t reply[ARP_HLEN];
	net_put16(reply + 0, 1);
	net_put16(reply + 2, ETHERTYPE_IPV4);
	reply[4] = 6;
	reply[5] = 4;
	net_put16(reply + 6, ARP_OP_REPLY);
	for (int i = 0; i < 6; i++)
		reply[8 + i] = nd->mac[i];		/* SHA = us */
	net_put32(reply + 14, tpa);			/* SPA = the targeted address */
	for (int i = 0; i < 6; i++)
		reply[18 + i] = a[8 + i];		/* THA = requester */
	net_put32(reply + 24, spa);			/* TPA = requester */

	eth_output(nd, spa, ETHERTYPE_ARP, reply, ARP_HLEN);
}
