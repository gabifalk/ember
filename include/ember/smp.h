/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_SMP_H
#define EMBER_SMP_H

#include <stdint.h>

/* Per-AP boot info, indexed by slot (atomic counter order) */
struct ap_boot_info {
	uint64_t stack_top;	/* Per-AP kernel stack top. */
	uint64_t cpu_local_ptr;	/* Per-AP struct cpu_local pointer. */
};

/*
 * Trampoline data block at physical 0x8F00 (base + 0xF00).
 * Filled in by smp.c before broadcast SIPI.
 * Shared by ALL APs (they self-identify via atomic slot counter).
 * Field offsets are baked into the hand-assembled trampoline code.
 */
struct ap_trampoline_data {
	uint64_t cr3;		/* +0X00: BSP's boot PML4 physical address. */
	uint64_t entry_64;	/* +0X08: virtual address of ap_entry_64() */
	uint64_t ap_info_phys;	/* +0X10: physical address of ap_boot_info array. */
	volatile uint32_t wake_flag;	/* +0X18: BSP sets to 1 to release parked APs. */
	uint32_t _pad1C;	/* +0X1C: padding. */
	volatile uint32_t ap_count;	/* +0X20: atomic counter, APs increment. */
	uint32_t max_aps;	/* +0X24: max APs to boot (excess halt in trampoline) */
	/* Temporary GDT for 16->64 transition. */
	uint64_t tmp_gdt[4];	/* +0X28: null, code32, data32, code64. */
	/* GDT pseudo-descriptor: 2-byte limit + 4-byte base, must be contiguous. */
	uint8_t tmp_gdtr[6];	/* +0X48: packed limit(2) + base(4) for lgdt. */
};

void smp_init(void);
void ap_entry_64(void *cpu_local_ptr);

/*
 * Flush TLB on all other CPUs via IPI (vector 33).
 * Call after modifying PTEs that may be cached on remote CPUs.
 * Verified: models/fork_cow_tlb_multipage.pml, models/munmap_tlb_race.pml.
 */
void smp_flush_tlb(void);

/* Install trampoline code at the given physical base and set up temp GDT. */
void trampoline_install(uint64_t phys_base);

/* Get pointer to the trampoline data block for a given base. */
struct ap_trampoline_data *trampoline_data(uint64_t phys_base);

#endif				/* EMBER_SMP_H. */
