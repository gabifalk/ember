/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Ethernet demux (rx_handler) and L2 framing. The output frame is assembled in
 * a per-call buffer owned by the send, so concurrent or reentrant senders never
 * share assembly state - no lock (see models/net_tx_buffer.pml). Untrusted
 * input: bounds-check, drop on error. */
#include <stdint.h>
#include "ember/net/eth.h"
#include "ember/net/arp.h"
#include "ember/net/ipv4.h"
#include "ember/net/inet.h"
#include "ember/netdev.h"

#define ETH_MTU 1500

int
eth_output(netdev_t *nd, uint32_t dst_ip, uint16_t ethertype,
	   const uint8_t *l3, uint16_t l3_len)
{
	if (l3_len > ETH_MTU)
		return -1;
	uint8_t eth_frame[ETH_HLEN + ETH_MTU];
	const uint8_t *dmac = arp_cache_lookup(nd, dst_ip);
	if (!dmac)
		return -1;
	if (!nd)
		return -1;

	for (int i = 0; i < 6; i++)
		eth_frame[i] = dmac[i];
	for (int i = 0; i < 6; i++)
		eth_frame[6 + i] = nd->mac[i];
	net_put16(eth_frame + 12, ethertype);
	for (uint16_t i = 0; i < l3_len; i++)
		eth_frame[ETH_HLEN + i] = l3[i];

	return nd->transmit(nd, eth_frame, (uint16_t)(ETH_HLEN + l3_len));
}

void
ether_input(netdev_t *nd, const void *frame, uint16_t len)
{
	if (len < ETH_HLEN)
		return;
	const uint8_t *f = (const uint8_t *)frame;
	uint16_t et = net_get16(f + 12);
	uint16_t payload_len = (uint16_t)(len - ETH_HLEN);

	if (et == ETHERTYPE_ARP)
		arp_input(nd, f + ETH_HLEN, payload_len);
	else if (et == ETHERTYPE_IPV4)
		ipv4_input(nd, f + ETH_HLEN, payload_len);
}

void
net_init(void)
{
	for (netdev_t *nd = netdev_first(); nd; nd = nd->next)
		nd->rx_handler = ether_input;
}
