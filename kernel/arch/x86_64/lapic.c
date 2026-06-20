/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

#include <stdint.h>
#include "ember/lapic.h"
#include "ember/cpu.h"
#include "ember/acpi.h"
#include "ember/mmu.h"
#include "ember/paging.h"
#include "ember/console.h"
#include "ember/io.h"
#include "ember/vectors.h"

/* LAPIC register offsets (byte offsets, MMIO) */
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_CCR 0x390
#define LAPIC_TIMER_DCR 0x3E0

/* SVR bits. */
#define SVR_ENABLE      (1u << 8)
#define SVR_SPURIOUS_VEC 0xFF

/* LVT mask bit. */
#define LVT_MASKED      (1u << 16)

/* Timer modes in LVT timer register. */
#define TIMER_PERIODIC  (1u << 17)

/* PIT for calibration. */
#define PIT_CH2_DATA    0x42
#define PIT_CMD         0x43
#define PIT_CH2_GATE    0x61
#define PIT_FREQ        1193182

volatile int lapic_enabled;

static inline void
lapic_write(uint32_t reg, uint32_t val)
{
	lapic_base[reg / 4] = val;
}

static inline uint32_t
lapic_read(uint32_t reg)
{
	return lapic_base[reg / 4];
}

void
lapic_init(void)
{
	lapic_enabled = 0;

	if (!acpi_lapic_base) {
		console_write("LAPIC: no base address from ACPI\n");
		return;
	}

	/*
	 * Map LAPIC MMIO page into HHDM (uncacheable).
	 * Only the BSP needs to create the mapping -- APs share the same
	 * page tables (CR3) and see it immediately.  Calling paging_map_range
	 * from multiple APs concurrently would race on the PMM and page table
	 * allocator (spinlock_t is cli/sti only, not SMP-safe).
	 */
	if (!lapic_base) {
		uint64_t lapic_phys = acpi_lapic_base & ~0xFFFULL;
		uint64_t pml4_phys = read_cr3() & 0x000FFFFFFFFFF000ULL;
		paging_map_range(pml4_phys, (uint64_t) phys_to_virt(lapic_phys),
				 lapic_phys, 0x1000,
				 PTE_PRESENT | PTE_WRITABLE | PTE_PCD |
				 PTE_PWT);
		lapic_base = (volatile uint32_t *)phys_to_virt(acpi_lapic_base);

		/*
		 * Verify the LAPIC MMIO page is actually mapped.
		 * The TLB shootdown handler writes to lapic_base + 0xB0 (EOI)
		 * after a CR3 reload that flushes the TLB.  If the page table
		 * walk for this address fails, the handler double-faults.
		 */
		uint64_t eoi_vaddr = (uint64_t) (uintptr_t) lapic_base + 0xB0;
		uint64_t *pte = paging_walk_pte(pml4_phys, eoi_vaddr);
		if (!pte || !(*pte & 1)) {
			console_write("LAPIC: EOI page NOT mapped at ");
			console_hex64(eoi_vaddr);
			console_write(" pte=");
			console_hex64(pte ? *pte : 0);
			console_write("\n");
		} else {
			console_write("LAPIC: EOI mapped at ");
			console_hex64(eoi_vaddr);
			console_write(" pte=");
			console_hex64(*pte);
			console_write("\n");
		}

		/*
		 * Record the physical addresses of the HHDM page table pages
		 * that map the LAPIC region.  If anyone frees these, the LAPIC
		 * mapping is destroyed -> triple fault.  pmm_free_page checks.
		 */
		{
			uint64_t lapic_vaddr =
			    (uint64_t) (uintptr_t) lapic_base;
			uint64_t *pml4v = (uint64_t *) phys_to_virt(pml4_phys);
			uint64_t pml4_i = (lapic_vaddr >> 39) & 0x1ff;
			uint64_t pdpt_i = (lapic_vaddr >> 30) & 0x1ff;
			uint64_t pd_i = (lapic_vaddr >> 21) & 0x1ff;

			if (pml4v[pml4_i] & 1) {
				uint64_t pdpt_pa =
				    pml4v[pml4_i] & 0x000FFFFFFFFFF000ULL;
				uint64_t *pdptv =
				    (uint64_t *) phys_to_virt(pdpt_pa);
				if (pdptv[pdpt_i] & 1) {
					uint64_t pd_pa =
					    pdptv[pdpt_i] &
					    0x000FFFFFFFFFF000ULL;
					uint64_t *pdv =
					    (uint64_t *) phys_to_virt(pd_pa);
					if (pdv[pd_i] & 1) {
						uint64_t pt_pa =
						    pdv[pd_i] &
						    0x000FFFFFFFFFF000ULL;
						extern uint64_t
						    lapic_guard_pt_pa;
						extern uint64_t
						    lapic_guard_pd_pa;
						lapic_guard_pt_pa = pt_pa;
						lapic_guard_pd_pa = pd_pa;
						console_write
						    ("LAPIC: guard PT PA=");
						console_hex64(pt_pa);
						console_write(" PD PA=");
						console_hex64(pd_pa);
						console_write("\n");
					}
				}
			}
		}
	}

	/* Enable LAPIC: set SVR bit 8, assign spurious vector 0xFF. */
	lapic_write(LAPIC_SVR, SVR_ENABLE | SVR_SPURIOUS_VEC);

	/* LINT0: ExtINT (pass PIC interrupts through virtual wire) */
	lapic_write(LAPIC_LVT_LINT0, 0x00000700);
	/* LINT1: NMI. */
	lapic_write(LAPIC_LVT_LINT1, 0x00000400);

	/* Set task priority to 0 (accept all interrupts) */
	lapic_write(LAPIC_TPR, 0);

	lapic_enabled = 1;
}

void
lapic_eoi(void)
{
	if (lapic_base)
		lapic_write(LAPIC_EOI, 0);
}

uint32_t
lapic_id(void)
{
	if (!lapic_base)
		return 0;
	return lapic_read(LAPIC_ID) >> 24;
}

/* Wait for ICR delivery status to clear (bit 12 = send pending) */
static void
lapic_icr_wait(void)
{
	for (int i = 0; i < 100000; i++) {
		if (!(lapic_read(LAPIC_ICR_LO) & (1u << 12)))
			return;
		__asm__ __volatile__("pause");
	}
}

void
lapic_send_ipi(uint8_t dest, uint8_t vector)
{
	if (!lapic_base)
		return;
	lapic_icr_wait();
	lapic_write(LAPIC_ICR_HI, (uint32_t) dest << 24);
	lapic_write(LAPIC_ICR_LO, (uint32_t) vector);
	lapic_icr_wait();
}

static uint32_t lapic_timer_count;	/* Calibrated count, reused by APs. */

void
lapic_timer_init(uint32_t hz)
{
	if (!lapic_base)
		return;

	/* Set divider to 16. */
	lapic_write(LAPIC_TIMER_DCR, 0x03);	/* Divide by 16. */

	/*
	 * Calibrate: use PIT channel 2 to measure LAPIC timer frequency.
	 * Program PIT ch2 for a one-shot countdown of ~10ms.
	 */
	uint32_t pit_ticks = PIT_FREQ / 100;	/* ~10Ms worth of PIT ticks. */

	/* Gate PIT channel 2 off, set to mode 0 (one-shot) */
	uint8_t gate = inb(PIT_CH2_GATE);
	outb(PIT_CH2_GATE, (gate & 0xFD) | 0x01);	/* Gate on, speaker off. */
	outb(PIT_CMD, 0xB0);	/* Ch2, lobyte/hibyte, mode 0, binary. */
	outb(PIT_CH2_DATA, (uint8_t) (pit_ticks & 0xFF));
	outb(PIT_CH2_DATA, (uint8_t) ((pit_ticks >> 8) & 0xFF));

	/* Start LAPIC timer with max count. */
	lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

	/* Wait for PIT channel 2 to expire (bit 5 of port 0x61 goes high) */
	while ((inb(PIT_CH2_GATE) & 0x20) == 0) {
		__asm__ __volatile__("pause");
	}

	/* Stop LAPIC timer. */
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);

	/* Read how many LAPIC ticks elapsed. */
	uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

	/* Elapsed ticks in ~10ms -> ticks per second = elapsed * 100. */
	uint32_t ticks_per_sec = elapsed * 100;
	uint32_t count = ticks_per_sec / hz;

	if (count == 0)
		count = 1;
	lapic_timer_count = count;

	/* Configure LAPIC timer: periodic mode, vector 32 (same as PIT was) */
	lapic_write(LAPIC_LVT_TIMER, TIMER_PERIODIC | VEC_TIMER);
	lapic_write(LAPIC_TIMER_DCR, 0x03);	/* Divide by 16. */
	lapic_write(LAPIC_TIMER_ICR, count);

	console_write("LAPIC timer started\n");
}

uint32_t
lapic_get_timer_count(void)
{
	return lapic_timer_count;
}

void
lapic_timer_init_count(uint32_t count)
{
	if (!lapic_base)
		return;
	lapic_write(LAPIC_LVT_TIMER, TIMER_PERIODIC | VEC_TIMER);
	lapic_write(LAPIC_TIMER_DCR, 0x03);	/* Divide by 16. */
	lapic_write(LAPIC_TIMER_ICR, count);
}

void
lapic_send_init(uint8_t dest)
{
	if (!lapic_base)
		return;
	lapic_icr_wait();
	lapic_write(LAPIC_ICR_HI, (uint32_t) dest << 24);
	lapic_write(LAPIC_ICR_LO, 0x00004500);	/* INIT, edge, physical. */
	lapic_icr_wait();
}

/*
 * Broadcast INIT to all APs (all-excluding-self shorthand).
 * No lapic_icr_wait -- smp_init's tsc_delay_ms ensures delivery,
 * and LAPIC MMIO polls cause KVM VM exit storms with many CPUs.
 * Verified: models/ap_boot_twophase.pml.
 */
void
lapic_send_init_all(void)
{
	if (!lapic_base)
		return;
	lapic_write(LAPIC_ICR_LO, 0x000C4500);	/* INIT, all-excl-self. */
}

/*
 * Broadcast SIPI to all APs (all-excluding-self shorthand).
 * No lapic_icr_wait -- same rationale as lapic_send_init_all.
 */
void
lapic_send_sipi_all(uint8_t vector)
{
	if (!lapic_base)
		return;
	lapic_write(LAPIC_ICR_LO, 0x000C4600 | vector);	/* SIPI, all-excl-self. */
}

/* Broadcast fixed IPI to all CPUs except self. */
void
lapic_send_ipi_all_excl_self(uint8_t vector)
{
	if (!lapic_base)
		return;
	lapic_icr_wait();
	lapic_write(LAPIC_ICR_LO, 0x000C4000 | vector);	/* Fixed, all-excl-self. */
	lapic_icr_wait();
}

void
lapic_send_sipi(uint8_t dest, uint8_t vector)
{
	if (!lapic_base)
		return;
	lapic_icr_wait();
	lapic_write(LAPIC_ICR_HI, (uint32_t) dest << 24);
	lapic_write(LAPIC_ICR_LO, 0x00004600 | vector);	/* SIPI, vector = page#. */
	lapic_icr_wait();
}
