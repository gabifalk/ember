/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_MMU_H
#define EMBER_MMU_H

#include <stdint.h>

#define PAGE_SIZE 4096ULL

#define HHDM_BASE   0xffff800000000000ULL
#define KERNEL_BASE 0xffffffff80000000ULL

#define PTE_PRESENT  0x001ULL
#define PTE_WRITABLE 0x002ULL
#define PTE_USER     0x004ULL
#define PTE_PWT      0x008ULL
#define PTE_PCD      0x010ULL
#define PTE_PS       0x080ULL	/* Bit 7: page size (1=2MB/1GB huge page) */
#define PTE_COW      0x200ULL	/* Bit 9: copy-on-write (OS-available) */
#define PTE_NX       (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

static inline void *
phys_to_virt(uint64_t paddr)
{
	return (void *)(HHDM_BASE + paddr);
}

static inline uint64_t
virt_to_phys(void *vaddr)
{
	return (uint64_t) vaddr - HHDM_BASE;
}

#endif
