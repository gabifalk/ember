/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_IRQ_H
#define EMBER_IRQ_H

#include <stdint.h>

typedef void (*irq_handler_t) (int vector);

/* Install a handler for a device IRQ vector (VEC_IRQ_BASE..VEC_IRQ_MAX). */
void irq_set_handler(uint8_t vector, irq_handler_t handler);

/* Called from the asm interrupt entry: run the handler then LAPIC EOI. */
void irq_dispatch(int vector);

/* Route a PCI INTx (level-triggered, active-low) GSI to a vector + handler,
 * targeting one LAPIC, and unmask it. */
void irq_route_pci(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id,
		   irq_handler_t handler);

/* Boot self-test: self-IPI exercises the dispatch path. Prints to serial. */
void irq_selftest(void);

#endif				/* EMBER_IRQ_H. */
