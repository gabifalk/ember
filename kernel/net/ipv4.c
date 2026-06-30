/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* IPv4 receive (validate + demux) and output (construct from scratch). The
 * output frame is assembled in a per-call buffer, so each send owns its bytes
 * and concurrent or reentrant senders cannot corrupt one another - no lock
 * (see models/net_tx_buffer.pml). Untrusted input: validate
 * version/IHL/length/checksum and drop on any mismatch. */
#include <stdint.h>
#include "ember/net/ipv4.h"
#include "ember/net/eth.h"
#include "ember/net/icmp.h"
#include "ember/net/inet.h"

#define IP_HLEN 20
#define IP_MAX_PAYLOAD 1480		/* ETH_MTU - IP_HLEN */

int
ipv4_output(netdev_t *nd, uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
	    const uint8_t *payload, uint16_t payload_len)
{
	if (payload_len > IP_MAX_PAYLOAD)
		return -1;
	uint16_t total = (uint16_t)(IP_HLEN + payload_len);
	uint8_t ip_pkt[IP_HLEN + IP_MAX_PAYLOAD];

	ip_pkt[0] = 0x45;			/* version 4, IHL 5 */
	ip_pkt[1] = 0x00;			/* TOS */
	net_put16(ip_pkt + 2, total);
	net_put16(ip_pkt + 4, 0);		/* identification: atomic datagram */
	net_put16(ip_pkt + 6, 0x4000);		/* DF set, fragment offset 0 */
	ip_pkt[8] = 64;				/* TTL */
	ip_pkt[9] = proto;
	net_put16(ip_pkt + 10, 0);		/* checksum placeholder */
	net_put32(ip_pkt + 12, src_ip);
	net_put32(ip_pkt + 16, dst_ip);
	net_put16(ip_pkt + 10, inet_csum(ip_pkt, IP_HLEN));

	for (uint16_t i = 0; i < payload_len; i++)
		ip_pkt[IP_HLEN + i] = payload[i];

	return eth_output(nd, dst_ip, ETHERTYPE_IPV4, ip_pkt, total);
}

void
ipv4_input(netdev_t *nd, const uint8_t *p, uint16_t len)
{
	if (len < IP_HLEN)
		return;
	if ((p[0] >> 4) != 4)
		return;
	uint16_t ihl = (uint16_t)((p[0] & 0x0f) * 4);
	if (ihl < IP_HLEN || ihl > len)
		return;
	uint16_t total = net_get16(p + 2);
	if (total < ihl || total > len)
		return;
	if (inet_csum(p, ihl) != 0)
		return;
	uint32_t dst = net_get32(p + 16);
	if (!netdev_has_addr(nd, dst))		/* not addressed to this interface */
		return;

	uint32_t src = net_get32(p + 12);
	if (p[9] == IPPROTO_ICMP)
		icmp_input(nd, p + ihl, (uint16_t)(total - ihl), src, dst);
}
