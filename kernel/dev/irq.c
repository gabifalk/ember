/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "ember/irq.h"
#include "ember/lapic.h"
#include "ember/ioapic.h"
#include "ember/vectors.h"

/* One handler slot per device vector (VEC_IRQ_BASE..VEC_IRQ_MAX). */
static irq_handler_t handlers[VEC_IRQ_MAX - VEC_IRQ_BASE + 1];

/* Registration uses uint8_t; asm dispatch entry pushes int - asymmetry intentional. */
void
irq_set_handler(uint8_t vector, irq_handler_t handler)
{
	if (vector < VEC_IRQ_BASE || vector > VEC_IRQ_MAX)
		return;
	handlers[vector - VEC_IRQ_BASE] = handler;
}

void
irq_dispatch(int vector)
{
	if (vector >= VEC_IRQ_BASE && vector <= VEC_IRQ_MAX) {
		irq_handler_t h = handlers[vector - VEC_IRQ_BASE];
		if (h)
			h(vector);
	}
	lapic_eoi();
}

void
irq_route_pci(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id,
	      irq_handler_t handler)
{
	if (vector < VEC_IRQ_BASE || vector > VEC_IRQ_MAX)
		return;
	irq_set_handler(vector, handler);
	ioapic_route_irq_level(gsi, vector, dest_lapic_id);
}
