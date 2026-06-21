/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/paging.h"
#include "ember/pmm.h"
#include "ember/heap.h"
#include "ember/console.h"
#include "ember/smp.h"

static uint64_t
alloc_pt_page(void)
{
	uint64_t p = pmm_alloc_page();
	if (p == UINT64_MAX)
		return 0;
	uint64_t *v = (uint64_t *) phys_to_virt(p);
	for (uint64_t i = 0; i < 512; i++)
		v[i] = 0;
	return p;
}

uint64_t
read_cr3(void)
{
	uint64_t v;
	__asm__ __volatile__("mov %%cr3, %0":"=r"(v));
	return v;
}

void
write_cr3(uint64_t pml4_phys)
{
	__asm__ __volatile__("mov %0, %%cr3"::"r"(pml4_phys):"memory");
}

static void
map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	uint64_t *pml4 = (uint64_t *) phys_to_virt(pml4_phys);
	uint64_t pml4_i = (vaddr >> 39) & 0x1ff;
	uint64_t pdpt_i = (vaddr >> 30) & 0x1ff;
	uint64_t pd_i = (vaddr >> 21) & 0x1ff;
	uint64_t pt_i = (vaddr >> 12) & 0x1ff;
	uint64_t tbl_flags = PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);

	if (!(pml4[pml4_i] & PTE_PRESENT)) {
		uint64_t new_pdpt = alloc_pt_page();
		pml4[pml4_i] = new_pdpt | tbl_flags;
	} else if (flags & PTE_USER) {
		pml4[pml4_i] |= PTE_USER;
	}
	uint64_t *pdpt =
	    (uint64_t *) phys_to_virt(pml4[pml4_i] & PTE_ADDR_MASK);

	if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
		uint64_t new_pd = alloc_pt_page();
		pdpt[pdpt_i] = new_pd | tbl_flags;
	} else if (flags & PTE_USER) {
		pdpt[pdpt_i] |= PTE_USER;
	}
	uint64_t *pd = (uint64_t *) phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

	if (!(pd[pd_i] & PTE_PRESENT)) {
		uint64_t new_pt = alloc_pt_page();
		pd[pd_i] = new_pt | tbl_flags;
	} else if (pd[pd_i] & (1ULL << 7)) {
		/*
		 * PD entry is a 2MB large page (PS bit set).  Split into 512
		 * 4K entries so we can map a single 4K page within this region.
		 * Without this split, map_page treats the 2MB phys addr as a
		 * PT pointer and corrupts memory.  Found via DF@CR2 analysis.
		 */
		uint64_t large_pa = pd[pd_i] & 0x000FFFFFFFE00000ULL;	/* 2MB-aligned phys. */
		uint64_t large_flags =
		    pd[pd_i] & ~0x000FFFFFFFE00000ULL & ~(1ULL << 7);
		uint64_t new_pt = alloc_pt_page();
		uint64_t *pt_v = (uint64_t *) phys_to_virt(new_pt);
		for (int i = 0; i < 512; i++)
			pt_v[i] =
			    (large_pa + (uint64_t) i * PAGE_SIZE) | large_flags;
		pd[pd_i] = new_pt | tbl_flags;
	} else if (flags & PTE_USER) {
		pd[pd_i] |= PTE_USER;
	}
	uint64_t *pt = (uint64_t *) phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

	pt[pt_i] = (paddr & PTE_ADDR_MASK) | flags;
}

void
paging_map_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
		 uint64_t size, uint64_t flags)
{
	uint64_t end = vaddr + size;
	uint64_t va = vaddr & ~(PAGE_SIZE - 1);
	uint64_t pa = paddr & ~(PAGE_SIZE - 1);
	while (va < end) {
		map_page(pml4_phys, va, pa, flags);
		va += PAGE_SIZE;
		pa += PAGE_SIZE;
	}
}

/*
 * Deep-copy a user page table. Walks PML4 entries 0..255 (user half),
 * allocates new pages for every level including leaf 4K pages, and copies content.
 */
uint64_t
paging_clone_user_pml4(uint64_t src_pml4_phys)
{
	uint64_t new_pml4 = alloc_pt_page();
	if (!new_pml4)
		return 0;

	uint64_t *src4 = (uint64_t *) phys_to_virt(src_pml4_phys);
	uint64_t *dst4 = (uint64_t *) phys_to_virt(new_pml4);

	/* Share the entire kernel half (PML4 indices 256..511) from current CR3. */
	uint64_t cur_pml4 = read_cr3() & PTE_ADDR_MASK;
	uint64_t *cur4 = (uint64_t *) phys_to_virt(cur_pml4);
	for (int i = 256; i < 512; i++)
		dst4[i] = cur4[i];

	/* Deep-copy ALL user-half entries (indices 0..255) */
	for (int i4 = 0; i4 < 256; i4++) {
		if (!(src4[i4] & PTE_PRESENT))
			continue;

		uint64_t new_pdpt = alloc_pt_page();
		if (!new_pdpt) {
			paging_free_user_pml4(new_pml4);
			return 0;
		}
		uint64_t *src3 =
		    (uint64_t *) phys_to_virt(src4[i4] & PTE_ADDR_MASK);
		uint64_t *dst3 = (uint64_t *) phys_to_virt(new_pdpt);
		dst4[i4] = new_pdpt | (src4[i4] & ~PTE_ADDR_MASK);

		for (int i3 = 0; i3 < 512; i3++) {
			if (!(src3[i3] & PTE_PRESENT))
				continue;

			uint64_t new_pd = alloc_pt_page();
			if (!new_pd) {
				paging_free_user_pml4(new_pml4);
				return 0;
			}
			uint64_t *src2 =
			    (uint64_t *) phys_to_virt(src3[i3] & PTE_ADDR_MASK);
			uint64_t *dst2 = (uint64_t *) phys_to_virt(new_pd);
			dst3[i3] = new_pd | (src3[i3] & ~PTE_ADDR_MASK);

			for (int i2 = 0; i2 < 512; i2++) {
				if (!(src2[i2] & PTE_PRESENT))
					continue;
				if (src2[i2] & (1ULL << 7)) {
					/*
					 * 2MB large page -- copy the PD entry directly, bump refcount
					 * on the 2MB physical page.  Do NOT treat as PT pointer.
					 */
					uint64_t large_pa =
					    src2[i2] & 0x000FFFFFFFE00000ULL;
					uint64_t large_flags =
					    src2[i2] & ~0x000FFFFFFFE00000ULL;
					if (large_flags & PTE_WRITABLE) {
						large_flags =
						    (large_flags &
						     ~PTE_WRITABLE) | PTE_COW;
						src2[i2] =
						    large_pa | large_flags;
					}
					pmm_page_ref(large_pa);
					dst2[i2] = large_pa | large_flags;
					continue;
				}

				uint64_t new_pt = alloc_pt_page();
				if (!new_pt) {
					paging_free_user_pml4(new_pml4);
					return 0;
				}
				uint64_t *src1 =
				    (uint64_t *) phys_to_virt(src2[i2] &
							      PTE_ADDR_MASK);
				uint64_t *dst1 =
				    (uint64_t *) phys_to_virt(new_pt);
				dst2[i2] = new_pt | (src2[i2] & ~PTE_ADDR_MASK);

				for (int i1 = 0; i1 < 512; i1++) {
					if (!(src1[i1] & PTE_PRESENT))
						continue;

					uint64_t phys =
					    src1[i1] & PTE_ADDR_MASK;
					uint64_t flags =
					    src1[i1] & ~PTE_ADDR_MASK;

					/* If page was writable, mark COW + read-only in BOTH parent and child. */
					if (flags & PTE_WRITABLE) {
						flags =
						    (flags & ~PTE_WRITABLE) |
						    PTE_COW;
						src1[i1] = phys | flags;	/* Update parent PTE. */
					}

					pmm_page_ref(phys);	/* Bump refcount. */
					dst1[i1] = phys | flags;	/* Child shares same page. */
				}
			}
		}
	}

	write_cr3(read_cr3());	/* Full TLB flush for parent (local) */
	/*
	 * No smp_flush_tlb: lazy TLB -- only this CPU runs the parent.
	 * Verified: models/tlb_lazy.pml.
	 */

	return new_pml4;
}

/*
 * Guard: verify a physical address is NOT in the boot page table pool.
 * If it is, paging_free_user_pml4 is about to free a kernel page table page.
 */
extern boot_info_v1_t *g_boot_info;

static void
guard_free(uint64_t pa, const char *what)
{
	if (g_boot_info && g_boot_info->pt_pool_phys_base) {
		uint64_t pool_base = g_boot_info->pt_pool_phys_base;
		uint64_t pool_end = pool_base + g_boot_info->pt_pool_bytes;
		if (pa >= pool_base && pa < pool_end) {
			console_write("\n!!! FREE BOOT PT PAGE !!!\n");
			console_write("  pa=");
			console_hex64(pa);
			console_write(" what=");
			console_write(what);
			console_write(" pool=");
			console_hex64(pool_base);
			console_write("-");
			console_hex64(pool_end);
			console_write("\n");
			serial_flush();
			for (;;)
				__asm__ __volatile__("hlt");
		}
	}
}

/*
 * Free an entire user-half page table tree (PML4 entries 0..255).
 * Frees leaf pages, PT/PD/PDPT pages, and finally the PML4 page itself.
 */
void
paging_free_user_pml4(uint64_t pml4_phys)
{
	if (!pml4_phys)
		return;

	uint64_t *pml4 = (uint64_t *) phys_to_virt(pml4_phys);

	for (int i4 = 0; i4 < 256; i4++) {
		if (!(pml4[i4] & PTE_PRESENT))
			continue;
		uint64_t pdpt_pa = pml4[i4] & PTE_ADDR_MASK;
		uint64_t *pdpt = (uint64_t *) phys_to_virt(pdpt_pa);

		for (int i3 = 0; i3 < 512; i3++) {
			if (!(pdpt[i3] & PTE_PRESENT))
				continue;
			uint64_t pd_pa = pdpt[i3] & PTE_ADDR_MASK;
			uint64_t *pd = (uint64_t *) phys_to_virt(pd_pa);

			for (int i2 = 0; i2 < 512; i2++) {
				if (!(pd[i2] & PTE_PRESENT))
					continue;
				if (pd[i2] & (1ULL << 7)) {
					/*
					 * 2MB large page: free the 2MB physical page (decrement refcount).
					 * Without this, fork bumps refcount but exit never decrements -> leak.
					 * No PT page to free (large page has no PT level).
					 */
					uint64_t large_pa =
					    pd[i2] & 0x000FFFFFFFE00000ULL;
					guard_free(large_pa, "2MB");
					pmm_free_page(large_pa);
					continue;
				}
				uint64_t pt_pa = pd[i2] & PTE_ADDR_MASK;
				uint64_t *pt = (uint64_t *) phys_to_virt(pt_pa);

				for (int i1 = 0; i1 < 512; i1++) {
					if (!(pt[i1] & PTE_PRESENT))
						continue;
					uint64_t leaf_pa =
					    pt[i1] & PTE_ADDR_MASK;
					guard_free(leaf_pa, "leaf");
					pmm_free_page(leaf_pa);
				}
				guard_free(pt_pa, "PT");
				pmm_free_page(pt_pa);
			}
			guard_free(pd_pa, "PD");
			pmm_free_page(pd_pa);
		}
		guard_free(pdpt_pa, "PDPT");
		pmm_free_page(pdpt_pa);
	}
	guard_free(pml4_phys, "PML4");
	pmm_free_page(pml4_phys);
}

uint64_t *
paging_walk_pte(uint64_t pml4_phys, uint64_t vaddr)
{
	uint64_t *pml4 = (uint64_t *) phys_to_virt(pml4_phys);
	uint64_t pml4_i = (vaddr >> 39) & 0x1ff;
	if (!(pml4[pml4_i] & PTE_PRESENT))
		return 0;
	uint64_t *pdpt =
	    (uint64_t *) phys_to_virt(pml4[pml4_i] & PTE_ADDR_MASK);

	uint64_t pdpt_i = (vaddr >> 30) & 0x1ff;
	if (!(pdpt[pdpt_i] & PTE_PRESENT))
		return 0;
	uint64_t *pd = (uint64_t *) phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

	uint64_t pd_i = (vaddr >> 21) & 0x1ff;
	if (!(pd[pd_i] & PTE_PRESENT))
		return 0;
	uint64_t *pt = (uint64_t *) phys_to_virt(pd[pd_i] & PTE_ADDR_MASK);

	uint64_t pt_i = (vaddr >> 12) & 0x1ff;
	return &pt[pt_i];
}

uint64_t
paging_unmap_page(uint64_t pml4_phys, uint64_t vaddr)
{
	uint64_t *pte = paging_walk_pte(pml4_phys, vaddr);
	if (!pte || !(*pte & PTE_PRESENT))
		return 0;
	uint64_t pa = *pte & PTE_ADDR_MASK;
	*pte = 0;
	return pa;
}

uint64_t
paging_create_user_pml4(void)
{
	uint64_t new_pml4 = alloc_pt_page();
	if (!new_pml4)
		return 0;

	uint64_t cur_pml4 = read_cr3() & PTE_ADDR_MASK;
	uint64_t *src = (uint64_t *) phys_to_virt(cur_pml4);
	uint64_t *dst = (uint64_t *) phys_to_virt(new_pml4);

	for (int i = 256; i < 512; i++)
		dst[i] = src[i];

	/* PML4[0] starts empty -- elf_load_user will populate it. */
	return new_pml4;
}

/* Handle a COW fault. Returns 1 if handled, 0 if not a COW fault. */
int
paging_handle_cow(uint64_t pml4_phys, uint64_t fault_addr)
{
	uint64_t *pte = paging_walk_pte(pml4_phys, fault_addr);
	if (!pte)
		return 0;
	if (!(*pte & PTE_PRESENT))
		return 0;
	if (!(*pte & PTE_COW))
		return 0;

	uint64_t old_phys = *pte & PTE_ADDR_MASK;
	uint64_t flags = *pte & ~PTE_ADDR_MASK;

	if (pmm_page_try_exclusive(old_phys)) {
		*pte = old_phys | ((flags & ~PTE_COW) | PTE_WRITABLE);
	} else {
		uint64_t new_phys = pmm_alloc_page();
		if (new_phys == UINT64_MAX)
			return 0;

		uint8_t *src = (uint8_t *) phys_to_virt(old_phys);
		uint8_t *dst = (uint8_t *) phys_to_virt(new_phys);
		kmemcpy(dst, src, 4096);

		/*
		 * DIAGNOSTIC: verify copy succeeded.  If src was freed/reused
		 * between the read and this check, the comparison will fail.
		 */
		{
			int mismatch = 0;
			for (int i = 0; i < 4096; i++) {
				if (dst[i] != src[i]) {
					mismatch = 1;
					break;
				}
			}
			if (mismatch) {
				extern void console_hex64(uint64_t v);
				console_write("\n!!! COW COPY MISMATCH !!!\n");
				console_write("  fault_addr=");
				console_hex64(fault_addr);
				console_write(" old_pa=");
				console_hex64(old_phys);
				console_write(" new_pa=");
				console_hex64(new_phys);
				console_write("\n");
			}
		}

		*pte = new_phys | ((flags & ~PTE_COW) | PTE_WRITABLE);
	}

	invlpg(fault_addr & ~(uint64_t) 0xFFF);
	return 1;
}
