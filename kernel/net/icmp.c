/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* ICMP echo reply. Builds the reply from scratch (copy id/seq/data, set type
 * 0, recompute the ICMP checksum) and sends it via ipv4_output. The reply is
 * assembled in a per-call buffer, not a shared static, so each send owns its
 * bytes (see models/net_tx_buffer.pml). The serial marker is what the
 * integration harness greps for. */
#include <stdint.h>
#include "ember/net/icmp.h"
#include "ember/net/ipv4.h"
#include "ember/net/inet.h"
#include "ember/console.h"

#define ICMP_MAX 1480

static void
icmp_echo_reply(netdev_t *nd, const uint8_t *req, uint16_t len,
		uint32_t src_ip, uint32_t dst_ip)
{
	if (len > ICMP_MAX)
		return;
	uint8_t icmp_msg[ICMP_MAX];
	for (uint16_t i = 0; i < len; i++)
		icmp_msg[i] = req[i];
	icmp_msg[0] = ICMP_ECHO_REPLY;
	icmp_msg[1] = 0;
	net_put16(icmp_msg + 2, 0);			/* checksum placeholder */
	net_put16(icmp_msg + 2, inet_csum(icmp_msg, len));

	/* Intentional serial output from IRQ context: this exact line is the
	 * marker the icmp integration test greps for. Do not remove. */
	if (ipv4_output(nd, dst_ip, src_ip, IPPROTO_ICMP, icmp_msg, len) == 0)
		console_write("icmp: echo reply sent\n");
}

void
icmp_input(netdev_t *nd, const uint8_t *icmp, uint16_t len,
	   uint32_t src_ip, uint32_t dst_ip)
{
	if (len < 8)
		return;
	if (inet_csum(icmp, len) != 0)
		return;
	if (icmp[0] != ICMP_ECHO_REQUEST)
		return;
	icmp_echo_reply(nd, icmp, len, src_ip, dst_ip);
}
