/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

#include <stdint.h>
#include "ember/ioapic.h"
#include "ember/acpi.h"
#include "ember/mmu.h"
#include "ember/paging.h"
#include "ember/console.h"

/* I/O APIC register offsets. */
#define IOREGSEL  0x00
#define IOWIN     0x10

/* I/O APIC registers. */
#define IOAPICID  0x00
#define IOAPICVER 0x01
#define IOREDTBL(n) (0x10 + 2 * (n))

static volatile uint32_t *ioapic_base;
static int ioapic_max_redir;

static void
ioapic_write(uint8_t reg, uint32_t val)
{
	ioapic_base[IOREGSEL / 4] = reg;
	ioapic_base[IOWIN / 4] = val;
}

static uint32_t
ioapic_read(uint8_t reg)
{
	ioapic_base[IOREGSEL / 4] = reg;
	return ioapic_base[IOWIN / 4];
}

/* Look up ISA IRQ in the ISO table; return GSI and set polarity/trigger bits. */
static uint32_t
iso_lookup(uint8_t irq, uint32_t * rte_flags)
{
	for (int i = 0; i < acpi_iso_count; i++) {
		if (acpi_isos[i].source == irq) {
			uint16_t flags = acpi_isos[i].flags;
			uint32_t f = 0;

			/* Polarity: bits [1:0] -- 00/01 = active-high, 11 = active-low. */
			uint8_t pol = flags & 0x3;
			if (pol == 3)
				f |= (1 << 13);	/* INTPOL = active-low. */

			/* Trigger: bits [3:2] -- 00/01 = edge, 11 = level. */
			uint8_t trig = (flags >> 2) & 0x3;
			if (trig == 3)
				f |= (1 << 15);	/* TRIGMOD = level. */

			*rte_flags = f;
			return acpi_isos[i].gsi;
		}
	}
	/* No override: identity mapping, ISA defaults (edge, active-high) */
	*rte_flags = 0;
	return (uint32_t) irq;
}

void
ioapic_init(void)
{
	if (!acpi_ioapic_base) {
		console_write("IOAPIC: no base address from ACPI\n");
		return;
	}

	/* Map IOAPIC MMIO page into HHDM (uncacheable) */
	uint64_t ioapic_phys = acpi_ioapic_base & ~0xFFFULL;
	uint64_t pml4_phys = read_cr3() & 0x000FFFFFFFFFF000ULL;
	paging_map_range(pml4_phys, (uint64_t) phys_to_virt(ioapic_phys),
			 ioapic_phys, 0x1000,
			 PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);

	ioapic_base = (volatile uint32_t *)phys_to_virt(acpi_ioapic_base);

	uint32_t ver = ioapic_read(IOAPICVER);
	ioapic_max_redir = (int)((ver >> 16) & 0xFF);

	console_write("IOAPIC: initialized, ");
	{
		int n = ioapic_max_redir + 1;
		char buf[8];
		int i = 0;
		if (n == 0) {
			buf[i++] = '0';
		} else {
			int v = n;
			while (v > 0) {
				buf[i++] = '0' + (v % 10);
				v /= 10;
			}
			for (int a = 0, b = i - 1; a < b; a++, b--) {
				char t = buf[a];
				buf[a] = buf[b];
				buf[b] = t;
			}
		}
		buf[i] = '\0';
		console_write(buf);
	}
	console_write(" entries\n");

	/* Mask all RTEs. */
	for (int i = 0; i <= ioapic_max_redir; i++) {
		uint32_t lo = ioapic_read(IOREDTBL(i));
		lo |= (1 << 16);	/* Set mask bit. */
		ioapic_write(IOREDTBL(i), lo);
	}
	console_write("IOAPIC: masked all RTEs\n");
}

void
ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_lapic_id)
{
	uint32_t rte_flags;
	uint32_t gsi = iso_lookup(irq, &rte_flags);

	if ((int)gsi > ioapic_max_redir)
		return;

	/* RTE low dword: vector | delivery Fixed(000) | phys dest | polarity/trigger | unmask. */
	uint32_t lo = (uint32_t) vector | rte_flags;
	/* RTE high dword: destination LAPIC ID in bits [31:24]. */
	uint32_t hi = (uint32_t) dest_lapic_id << 24;

	ioapic_write(IOREDTBL(gsi) + 1, hi);
	ioapic_write(IOREDTBL(gsi), lo);
}

void
ioapic_mask(uint8_t irq)
{
	uint32_t rte_flags;
	uint32_t gsi = iso_lookup(irq, &rte_flags);

	if ((int)gsi > ioapic_max_redir)
		return;

	uint32_t lo = ioapic_read(IOREDTBL(gsi));
	lo |= (1 << 16);
	ioapic_write(IOREDTBL(gsi), lo);
}

void
ioapic_unmask(uint8_t irq)
{
	uint32_t rte_flags;
	uint32_t gsi = iso_lookup(irq, &rte_flags);

	if ((int)gsi > ioapic_max_redir)
		return;

	uint32_t lo = ioapic_read(IOREDTBL(gsi));
	lo &= ~(1 << 16);
	ioapic_write(IOREDTBL(gsi), lo);
}

void
ioapic_route_irq_level(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id)
{
	if ((int)gsi > ioapic_max_redir)
		return;

	/* Low dword: vector | level-triggered (bit15), delivery Fixed,
	 * physical dest, active-high (bit13 clear), unmasked (bit16 clear).
	 * On QEMU q35 the ICH9 PIRQ presents PCI interrupts to the IOAPIC
	 * as active-high (asserted = input high), matching the ACPI ISO
	 * polarity = 01 (active-high) recorded for the shared GSIs. */
	uint32_t lo = (uint32_t) vector | (1u << 15);
	uint32_t hi = (uint32_t) dest_lapic_id << 24;

	ioapic_write(IOREDTBL(gsi) + 1, hi);
	ioapic_write(IOREDTBL(gsi), lo);
}
