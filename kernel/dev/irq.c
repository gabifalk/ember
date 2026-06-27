/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "ember/irq.h"
#include "ember/lapic.h"
#include "ember/ioapic.h"
#include "ember/vectors.h"
#include "ember/console.h"

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

static volatile int irq_selftest_hit;

static void
irq_selftest_handler(int vector)
{
	(void)vector;
	irq_selftest_hit = 1;
}

void
irq_selftest(void)
{
	irq_selftest_hit = 0;
	irq_set_handler(VEC_IRQ_SELFTEST, irq_selftest_handler);

	/* Self-IPI to the BSP at the self-test vector. */
	lapic_send_ipi((uint8_t) lapic_id(), VEC_IRQ_SELFTEST);

	/*
	 * Spin with a sti;pause;cli window so the pending self-IPI can be
	 * delivered even if the caller has not yet enabled interrupts.
	 * This mirrors the pattern used by smp_flush_tlb for IPI delivery.
	 */
	for (volatile int i = 0; i < 1000000 && !irq_selftest_hit; i++)
		__asm__ __volatile__("sti; pause; cli" ::: "memory");

	if (irq_selftest_hit)
		console_write("irq-selftest: passed\n");
	else
		console_write("irq-selftest: FAIL (handler not invoked)\n");

	irq_set_handler(VEC_IRQ_SELFTEST, 0);
}
