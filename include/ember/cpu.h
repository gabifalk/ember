/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_CPU_H
#define EMBER_CPU_H

#include <stdint.h>

#define MAX_CPUS 64

/* Filled by ACPI MADT parsing. Index = cpu_id (0..cpu_count-1). */
extern uint32_t cpu_lapic_ids[MAX_CPUS];
extern int cpu_count;
extern int cpu_online_count;

/*
 * Map LAPIC ID -> cpu_id. Indexed by LAPIC ID (max 256 for xAPIC).
 * Returns -1 if unknown. Filled during MADT parse.
 */
extern int lapic_to_cpu[256];

/* LAPIC MMIO base. NULL until LAPIC init. */
extern volatile uint32_t *lapic_base;

/*
 * Returns current CPU's logical ID (0..N-1).
 * Before LAPIC init: always 0 (BSP).
 */
static inline int
this_cpu_id(void)
{
	extern volatile uint32_t *lapic_base;
	if (!lapic_base)
		return 0;
	uint32_t lapic_id = lapic_base[0x20 / 4] >> 24;
	return lapic_to_cpu[lapic_id];
}

void cpu_init(void);

#endif				/* EMBER_CPU_H. */
