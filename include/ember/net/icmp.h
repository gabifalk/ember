/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_NET_ICMP_H
#define EMBER_NET_ICMP_H

#include <stdint.h>
#include "ember/netdev.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

/* Handle an inbound ICMP message; src_ip is the requester (reply destination),
 * dst_ip is the local address it was sent to (reply source). */
void icmp_input(netdev_t *nd, const uint8_t *icmp, uint16_t len,
		uint32_t src_ip, uint32_t dst_ip);

#endif				/* EMBER_NET_ICMP_H. */
