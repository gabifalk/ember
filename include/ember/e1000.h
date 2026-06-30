/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_E1000_H
#define EMBER_E1000_H

/* Probe and bring up the e1000 NIC if present. Registers a netdev on success;
 * no-op (kernel boots normally) if absent. */
void e1000_init(void);

#endif				/* EMBER_E1000_H. */
