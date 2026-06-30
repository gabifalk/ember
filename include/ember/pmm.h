/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_PMM_H
#define EMBER_PMM_H

#include <stdint.h>
#include "../boot_info.h"

void pmm_init(boot_info_v1_t * bi);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_page_below(uint64_t limit);
uint64_t pmm_alloc_pages(uint64_t pages);
void pmm_free_page(uint64_t paddr);
void pmm_free_pages(uint64_t paddr, uint64_t pages);
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);
void pmm_page_ref(uint64_t paddr);
uint16_t pmm_page_refcount(uint64_t paddr);
int pmm_page_try_exclusive(uint64_t paddr);

#endif
