/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_PAGING_H
#define EMBER_PAGING_H

#include <stdint.h>
#include "ember/mmu.h"

uint64_t read_cr3(void);
void write_cr3(uint64_t pml4_phys);

uint64_t paging_create_user_pml4(void);
void paging_map_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
		      uint64_t size, uint64_t flags);
uint64_t paging_clone_user_pml4(uint64_t src_pml4_phys);
void paging_free_user_pml4(uint64_t pml4_phys);

uint64_t *paging_walk_pte(uint64_t pml4_phys, uint64_t vaddr);
uint64_t paging_unmap_page(uint64_t pml4_phys, uint64_t vaddr);
int paging_handle_cow(uint64_t pml4_phys, uint64_t fault_addr);

static inline void
invlpg(uint64_t vaddr)
{
	__asm__ __volatile__("invlpg (%0)"::"r"(vaddr):"memory");
}

#endif
