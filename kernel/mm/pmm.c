/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "ember/pmm.h"
#include "ember/mmu.h"
#include "ember/spinlock.h"
#include "../../include/uefi_min.h"
#include "ember/console.h"
#include "ember/bug.h"

static spinlock_t pmm_lock = SPINLOCK_INIT;
static uint8_t *pmm_bitmap;
static uint64_t pmm_bitmap_bytes;
static uint64_t pmm_total_pages;
static uint16_t *pmm_refcounts;
static uint64_t pmm_next_free;	/* Hint: <= first free index. */

static inline void
bitmap_set(uint64_t idx)
{
	pmm_bitmap[idx >> 3] |= (uint8_t) (1u << (idx & 7));
}

static inline void
bitmap_clear(uint64_t idx)
{
	pmm_bitmap[idx >> 3] &= (uint8_t) ~ (1u << (idx & 7));
}

static inline int
bitmap_test(uint64_t idx)
{
	return (pmm_bitmap[idx >> 3] >> (idx & 7)) & 1u;
}

static void
mark_range_used(uint64_t base, uint64_t size)
{
	uint64_t start = base / PAGE_SIZE;
	uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	for (uint64_t i = 0; i < pages; i++) {
		if (start + i < pmm_total_pages)
			bitmap_set(start + i);
	}
}

static void
mark_range_free(uint64_t base, uint64_t size)
{
	uint64_t start = base / PAGE_SIZE;
	uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	for (uint64_t i = 0; i < pages; i++) {
		if (start + i < pmm_total_pages)
			bitmap_clear(start + i);
	}
}

void
pmm_init(boot_info_v1_t * bi)
{
	if (!bi || bi->efi_desc_size == 0 || bi->efi_mmap_size == 0) {
		console_write("PMM: bad mmap info\n");
		return;
	}

	/* Dump EFI memory map for debugging. */
	console_write("PMM: EFI memory map dump:\n");
	for (uint64_t off = 0; off < bi->efi_mmap_size;
	     off += bi->efi_desc_size) {
		EFI_MEMORY_DESCRIPTOR *d =
		    (EFI_MEMORY_DESCRIPTOR *) ((uint8_t *) bi->efi_mmap + off);
		console_write("  type=");
		console_hex64((uint64_t) d->Type);
		console_write(" phys=");
		console_hex64(d->PhysicalStart);
		console_write(" pages=");
		console_hex64(d->NumberOfPages);
		console_write("\n");
	}

	uint64_t top = 0;
	for (uint64_t off = 0; off < bi->efi_mmap_size;
	     off += bi->efi_desc_size) {
		EFI_MEMORY_DESCRIPTOR *d =
		    (EFI_MEMORY_DESCRIPTOR *) ((uint8_t *) bi->efi_mmap + off);
		/* Only track conventional (usable) memory to avoid giant bitmaps for MMIO holes. */
		if (d->Type != EfiConventionalMemory
		    && d->Type != EfiBootServicesCode
		    && d->Type != EfiBootServicesData
		    && d->Type != EfiLoaderCode && d->Type != EfiLoaderData)
			continue;
		uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
		if (end > top)
			top = end;
	}

	pmm_total_pages = (top + PAGE_SIZE - 1) / PAGE_SIZE;
	pmm_bitmap_bytes = (pmm_total_pages + 7) / 8;

	/* Place bitmap + refcount array in first suitable conventional range. */
	uint64_t bitmap_need =
	    ((pmm_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
	uint64_t rc_need =
	    ((pmm_total_pages * sizeof(uint16_t) + PAGE_SIZE -
	      1) / PAGE_SIZE) * PAGE_SIZE;
	uint64_t total_need = bitmap_need + rc_need;
	uint64_t bitmap_phys = 0;
	for (uint64_t off = 0; off < bi->efi_mmap_size;
	     off += bi->efi_desc_size) {
		EFI_MEMORY_DESCRIPTOR *d =
		    (EFI_MEMORY_DESCRIPTOR *) ((uint8_t *) bi->efi_mmap + off);
		if (d->Type != EfiConventionalMemory)
			continue;
		uint64_t base = d->PhysicalStart;
		uint64_t size = d->NumberOfPages * PAGE_SIZE;
		if (size >= total_need) {
			bitmap_phys = base;
			break;
		}
	}

	bi->pmm_bitmap_phys_base = bitmap_phys;
	bi->pmm_bitmap_bytes = pmm_bitmap_bytes;

	pmm_bitmap = (uint8_t *) phys_to_virt(bitmap_phys);

	/* Place refcount array right after the bitmap. */
	uint64_t bitmap_pages = (pmm_bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t rc_bytes = pmm_total_pages * sizeof(uint16_t);
	uint64_t rc_pages = (rc_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	pmm_refcounts =
	    (uint16_t *) phys_to_virt(bitmap_phys + bitmap_pages * PAGE_SIZE);

	/* Mark all used. */
	for (uint64_t i = 0; i < pmm_bitmap_bytes; i++)
		pmm_bitmap[i] = 0xFF;

	/* Zero the refcount array. */
	for (uint64_t i = 0; i < pmm_total_pages; i++)
		pmm_refcounts[i] = 0;

	/* Free all conventional memory. */
	for (uint64_t off = 0; off < bi->efi_mmap_size;
	     off += bi->efi_desc_size) {
		EFI_MEMORY_DESCRIPTOR *d =
		    (EFI_MEMORY_DESCRIPTOR *) ((uint8_t *) bi->efi_mmap + off);
		if (d->Type != EfiConventionalMemory)
			continue;
		uint64_t base = d->PhysicalStart;
		uint64_t size = d->NumberOfPages * PAGE_SIZE;
		mark_range_free(base, size);
	}

	/* Re-mark critical ranges used. */
	mark_range_used(bi->kernel_phys_base,
			bi->kernel_phys_end - bi->kernel_phys_base);
	if (bi->initrd_phys_base && bi->initrd_size) {
		mark_range_used(bi->initrd_phys_base, bi->initrd_size);
	}
	if (bi->fb_phys_base && bi->fb_size) {
		mark_range_used(bi->fb_phys_base, bi->fb_size);
	}
	mark_range_used(bi->pmm_bitmap_phys_base, bi->pmm_bitmap_bytes);
	mark_range_used(bitmap_phys + bitmap_pages * PAGE_SIZE,
			rc_pages * PAGE_SIZE);
	mark_range_used(virt_to_phys((void *)bi), sizeof(*bi));
	mark_range_used(virt_to_phys(bi->efi_mmap), bi->efi_mmap_size);
	if (bi->pt_pool_phys_base && bi->pt_pool_bytes) {
		mark_range_used(bi->pt_pool_phys_base, bi->pt_pool_bytes);
	}

	/*
	 * Reserve the current kernel stack to avoid PMM clobbering it.
	 * RSP may be an HHDM virtual address; convert to physical.
	 */
	uint64_t sp = 0;
	__asm__ __volatile__("mov %%rsp, %0":"=r"(sp));
	if (sp >= HHDM_BASE)
		sp -= HHDM_BASE;
	uint64_t stack_base = sp & ~(PAGE_SIZE - 1);
	uint64_t reserve_base =
	    (stack_base >= 2 * PAGE_SIZE) ? (stack_base - 2 * PAGE_SIZE) : 0;
	mark_range_used(reserve_base, 4 * PAGE_SIZE);
}

uint64_t
pmm_alloc_page(void)
{
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	/* Scan from hint, then wrap around. */
	for (uint64_t n = 0; n < pmm_total_pages; n++) {
		uint64_t i = pmm_next_free + n;
		if (i >= pmm_total_pages)
			i -= pmm_total_pages;
		if (!bitmap_test(i)) {
			/* Paranoid: bitmap says free but refcount must also be 0. */
			if (pmm_refcounts[i] != 0) {
				bug_serial_str
				    ("\n!!! PMM DOUBLE ALLOC: bitmap free but rc=");
				bug_serial_hex((uint64_t) pmm_refcounts[i]);
				bug_serial_str(" page=");
				bug_serial_hex(i * PAGE_SIZE);
				bug_serial_str(" !!!\n");
				__asm__ __volatile__("cli; hlt");
			}
			bitmap_set(i);
			pmm_refcounts[i] = 1;
			pmm_next_free = i + 1;
			if (pmm_next_free >= pmm_total_pages)
				pmm_next_free = 0;
			spin_unlock_irqrestore(&pmm_lock, flags);
			return i * PAGE_SIZE;
		}
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
	return UINT64_MAX;
}

uint64_t
pmm_alloc_page_below(uint64_t limit)
{
	uint64_t max_idx = limit / PAGE_SIZE;
	if (max_idx > pmm_total_pages)
		max_idx = pmm_total_pages;
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	for (uint64_t i = 0; i < max_idx; i++) {
		if (!bitmap_test(i)) {
			if (pmm_refcounts[i] != 0) {
				spin_unlock_irqrestore(&pmm_lock, flags);
				return UINT64_MAX;
			}
			bitmap_set(i);
			pmm_refcounts[i] = 1;
			spin_unlock_irqrestore(&pmm_lock, flags);
			return i * PAGE_SIZE;
		}
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
	return UINT64_MAX;
}

uint64_t
pmm_alloc_pages(uint64_t count)
{
	if (count == 0)
		return UINT64_MAX;
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t run = 0;
	uint64_t start = 0;
	/* Scan from hint; contiguous allocations don't wrap around. */
	for (uint64_t n = 0; n < pmm_total_pages; n++) {
		uint64_t i = pmm_next_free + n;
		if (i >= pmm_total_pages)
			i -= pmm_total_pages;
		if (!bitmap_test(i)) {
			if (run == 0)
				start = i;
			/* Reset run if indices are not contiguous (wrap boundary) */
			else if (i != start + run) {
				run = 1;
				start = i;
				continue;
			}
			run++;
			if (run == count) {
				for (uint64_t j = 0; j < count; j++) {
					bitmap_set(start + j);
					pmm_refcounts[start + j] = 1;
				}
				pmm_next_free = start + count;
				if (pmm_next_free >= pmm_total_pages)
					pmm_next_free = 0;
				spin_unlock_irqrestore(&pmm_lock, flags);
				return start * PAGE_SIZE;
			}
		} else {
			run = 0;
		}
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
	return UINT64_MAX;
}

/* LAPIC HHDM page table guard: set by lapic_init. */
uint64_t lapic_guard_pt_pa;
uint64_t lapic_guard_pd_pa;

void
pmm_free_page(uint64_t paddr)
{
	/* Trap: someone freeing the LAPIC HHDM page table pages. */
	if (paddr == lapic_guard_pt_pa || paddr == lapic_guard_pd_pa) {
		/* Use direct serial to avoid any lock/LAPIC dependency. */
		bug_serial_str("\n!!! FREEING LAPIC PT PAGE !!!\n  pa=");
		bug_serial_hex(paddr);
		bug_serial_str(" (");
		bug_serial_str(paddr == lapic_guard_pt_pa ? "PT" : "PD");
		bug_serial_str(")\n");
		/* Dump return address to identify caller. */
		uint64_t rip;
		__asm__ __volatile__("lea (%%rip), %0":"=r"(rip));
		bug_serial_str("  caller near ");
		bug_serial_hex(rip);
		bug_serial_str("\n  stack:\n");
		uint64_t *rbp;
		__asm__ __volatile__("mov %%rbp, %0":"=r"(rbp));
		for (int i = 0;
		     i < 8 && rbp && (uint64_t) rbp > 0xffffffff80000000ULL;
		     i++) {
			bug_serial_str("    ");
			bug_serial_hex(rbp[1]);	/* Return address. */
			bug_serial_str("\n");
			rbp = (uint64_t *) rbp[0];	/* Previous frame. */
		}
		__asm__ __volatile__("cli; hlt");
	}
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t idx = paddr / PAGE_SIZE;
	if (idx < pmm_total_pages && pmm_refcounts[idx] > 0) {
		pmm_refcounts[idx]--;
		if (pmm_refcounts[idx] == 0) {
			bitmap_clear(idx);
			if (idx < pmm_next_free)
				pmm_next_free = idx;
		}
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
}

void
pmm_free_pages(uint64_t paddr, uint64_t pages)
{
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t idx = paddr / PAGE_SIZE;
	for (uint64_t i = 0; i < pages; i++) {
		if (idx + i < pmm_total_pages && pmm_refcounts[idx + i] > 0) {
			pmm_refcounts[idx + i]--;
			if (pmm_refcounts[idx + i] == 0) {
				bitmap_clear(idx + i);
				if (idx + i < pmm_next_free)
					pmm_next_free = idx + i;
			}
		}
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t
pmm_get_total_pages(void)
{
	return pmm_total_pages;
}

uint64_t
pmm_get_free_pages(void)
{
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t free = 0;
	for (uint64_t i = 0; i < pmm_total_pages; i++) {
		if (!bitmap_test(i))
			free++;
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
	return free;
}

void
pmm_page_ref(uint64_t paddr)
{
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t idx = paddr / PAGE_SIZE;
	if (idx < pmm_total_pages)
		pmm_refcounts[idx]++;
	spin_unlock_irqrestore(&pmm_lock, flags);
}

uint16_t
pmm_page_refcount(uint64_t paddr)
{
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t idx = paddr / PAGE_SIZE;
	uint16_t rc = 0;
	if (idx < pmm_total_pages)
		rc = pmm_refcounts[idx];
	spin_unlock_irqrestore(&pmm_lock, flags);
	return rc;
}

int
pmm_cow_unshare(uint64_t paddr)
{
	uint64_t flags = spin_lock_irqsave(&pmm_lock);
	uint64_t idx = paddr / PAGE_SIZE;
	if (idx < pmm_total_pages && pmm_refcounts[idx] == 1) {
		spin_unlock_irqrestore(&pmm_lock, flags);
		return 1;
	}
	if (idx < pmm_total_pages && pmm_refcounts[idx] > 1) {
		pmm_refcounts[idx]--;
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
	return 0;
}
