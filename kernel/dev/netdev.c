/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "ember/netdev.h"

static netdev_t *netdev_list;

void
netdev_register(netdev_t *nd)
{
	nd->next = netdev_list;
	netdev_list = nd;
}

netdev_t *
netdev_first(void)
{
	return netdev_list;
}

void
netdev_add_addr(netdev_t *nd, uint32_t ip)
{
	if (nd->naddr < IFADDR_MAX)
		nd->addr[nd->naddr++] = ip;
}

int
netdev_has_addr(const netdev_t *nd, uint32_t ip)
{
	for (uint8_t i = 0; i < nd->naddr; i++)
		if (nd->addr[i] == ip)
			return 1;
	return 0;
}

void
netdev_deliver_rx(netdev_t *nd, const void *frame, uint16_t len)
{
	if (nd->rx_handler)
		nd->rx_handler(nd, frame, len);
}
