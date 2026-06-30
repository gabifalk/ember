/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_NETDEV_H
#define EMBER_NETDEV_H

#include <stdint.h>

#define IFADDR_MAX 4
#define ARP_CACHE_SIZE 16

typedef struct netdev {
	const char *name;
	uint8_t mac[6];
	uint32_t addr[IFADDR_MAX];	/* configured IPv4 addrs, network numeric order */
	uint8_t naddr;			/* count of valid entries in addr[] */
	int (*transmit)(struct netdev *nd, const void *frame, uint16_t len);
	void (*rx_handler)(struct netdev *nd, const void *frame, uint16_t len);
	struct {
		uint8_t valid;
		uint32_t ip;
		uint8_t mac[6];
	} arp_cache[ARP_CACHE_SIZE];	/* per-interface ARP cache */
	uint16_t arp_victim;		/* round-robin evict cursor */
	struct netdev *next;		/* registry list link */
} netdev_t;

void netdev_register(netdev_t *nd);
netdev_t *netdev_first(void);
void netdev_add_addr(netdev_t *nd, uint32_t ip);
int netdev_has_addr(const netdev_t *nd, uint32_t ip);
void netdev_deliver_rx(netdev_t *nd, const void *frame, uint16_t len);

#endif				/* EMBER_NETDEV_H. */
