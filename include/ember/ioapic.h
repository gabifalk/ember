/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_IOAPIC_H
#define EMBER_IOAPIC_H

#include <stdint.h>

void ioapic_init(void);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_lapic_id);
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);
/* Route a PCI-style level-triggered, active-low interrupt by GSI directly
 * (no ISA ISO lookup), set vector + destination, and unmask. */
void ioapic_route_irq_level(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id);

#endif				/* EMBER_IOAPIC_H. */
