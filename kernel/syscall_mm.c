/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/smp.h"

/*
 * Check if the current process's PML4 is shared with another process
 * (CLONE_VM threads).  If so, munmap/mprotect must flush remote TLBs
 * before freeing pages.  Verified: models/unified_smp.pml P9.
 */
static int
pml4_is_shared(proc_t * cur)
{
	if (cpu_count <= 1)
		return 0;
	uint64_t pml4 = cur->pml4_phys;
	for (int i = 0; i < MAX_PROCS; i++) {
		if (&procs[i] != cur && procs[i].state != PROC_UNUSED &&
		    procs[i].pml4_phys == pml4)
			return 1;
	}
	return 0;
}

/* ---- VMA helpers ---- */
int
vma_add(proc_t * p, uint64_t start, uint64_t length, uint8_t prot)
{
	/* Try to merge with an adjacent VMA that has the same prot. */
	uint64_t end = start + length;
	for (int i = 0; i < MAX_VMAS; i++) {
		if (!p->vmas[i].used || p->vmas[i].prot != prot)
			continue;
		uint64_t vs = p->vmas[i].start;
		uint64_t ve = vs + p->vmas[i].length;
		if (ve == start) {
			/* Existing VMA ends where new one starts: extend right. */
			p->vmas[i].length += length;
			return 0;
		}
		if (end == vs) {
			/* New region ends where existing starts: extend left. */
			p->vmas[i].start = start;
			p->vmas[i].length += length;
			return 0;
		}
	}
	/* No merge possible: allocate a new slot. */
	for (int i = 0; i < MAX_VMAS; i++) {
		if (!p->vmas[i].used) {
			p->vmas[i].start = start;
			p->vmas[i].length = length;
			p->vmas[i].prot = prot;
			p->vmas[i].used = 1;
			return 0;
		}
	}
	return -1;
}

static vma_t *
vma_find(proc_t * p, uint64_t addr)
{
	for (int i = 0; i < MAX_VMAS; i++) {
		if (p->vmas[i].used &&
		    addr >= p->vmas[i].start &&
		    addr < p->vmas[i].start + p->vmas[i].length)
			return &p->vmas[i];
	}
	return 0;
}

static void
vma_remove(proc_t * p, uint64_t start, uint64_t length)
{
	uint64_t end = start + length;
	for (int i = 0; i < MAX_VMAS; i++) {
		if (!p->vmas[i].used)
			continue;
		uint64_t vs = p->vmas[i].start;
		uint64_t ve = vs + p->vmas[i].length;
		if (ve <= start || vs >= end)
			continue;	/* No overlap. */

		if (vs >= start && ve <= end) {
			/* VMA entirely within unmapped range: remove. */
			p->vmas[i].used = 0;
		} else if (vs < start && ve > end) {
			/* Unmapped range punches a hole: split VMA in two. */
			p->vmas[i].length = start - vs;	/* Shrink original to left part. */
			/* Try to add right part. */
			vma_add(p, end, ve - end, p->vmas[i].prot);
		} else if (vs < start) {
			/* Overlap at end of VMA: shrink. */
			p->vmas[i].length = start - vs;
		} else {
			/* Overlap at start of VMA: move start forward. */
			p->vmas[i].start = end;
			p->vmas[i].length = ve - end;
		}
	}
}

/* True if addr falls in a VMA that permits writes (protection authority). */
int
vma_addr_writable(proc_t * p, uint64_t addr)
{
	vma_t *v = vma_find(p, addr);
	return v && (v->prot & PROT_WRITE);
}

static uint64_t
prot_to_pte(uint8_t prot)
{
	if (prot == 0)
		return 0;	/* PROT_NONE: not present. */
	uint64_t f = PTE_PRESENT | PTE_USER;
	if (prot & PROT_WRITE)
		f |= PTE_WRITABLE;
	if (!(prot & PROT_EXEC))
		f |= PTE_NX;
	return f;
}

/* ---- Per-syscall handlers ---- */

static uint64_t
do_brk(syscall_frame_t * f)
{
	uint64_t addr = f->rdi;
	uint64_t *brk_ptr = current_proc ? &current_proc->brk : 0;
	uint64_t *pml4_ptr = current_proc ? &current_proc->pml4_phys : 0;
	if (!brk_ptr || !pml4_ptr) {
		f->rax = 0;
		return 0;
	}
	if (addr == 0) {
		f->rax = *brk_ptr;
		return f->rax;
	}
	/* Reject brk below the initial brk base or in kernel space. */
	if (addr < current_proc->brk_base || addr >= 0x800000000000ULL) {
		f->rax = *brk_ptr;
		return f->rax;
	}
	if (addr > *brk_ptr) {
		uint64_t old_page =
		    (*brk_ptr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		uint64_t new_page = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		for (uint64_t va = old_page; va < new_page; va += PAGE_SIZE) {
			uint64_t pa = pmm_alloc_page();
			if (pa == UINT64_MAX) {
				f->rax = *brk_ptr;
				return f->rax;
			}
			/* Zero the page -- Linux guarantees brk memory is zeroed. */
			uint8_t *kva = (uint8_t *) phys_to_virt(pa);
			kmemzero(kva, PAGE_SIZE);
			paging_map_range(*pml4_ptr, va, pa, PAGE_SIZE,
					 PTE_PRESENT | PTE_USER | PTE_WRITABLE);
		}
		if (new_page > old_page)
			vma_add(current_proc, old_page, new_page - old_page,
				PROT_READ | PROT_WRITE);
	} else if (addr < *brk_ptr) {
		/* Shrink: unmap and free pages above new brk. */
		uint64_t new_page = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		uint64_t old_page =
		    (*brk_ptr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		int _shared = pml4_is_shared(current_proc);
		for (uint64_t va = new_page; va < old_page; va += PAGE_SIZE) {
			uint64_t pa = paging_unmap_page(*pml4_ptr, va);
			if (pa) {
				invlpg(va);
				if (_shared)
					smp_flush_tlb();
				pmm_free_page(pa);
			}
		}
		if (old_page > new_page)
			vma_remove(current_proc, new_page, old_page - new_page);
	}
	*brk_ptr = addr;
	f->rax = addr;
	return f->rax;
}

static uint64_t
do_mmap(syscall_frame_t * f)
{
	uint64_t addr = f->rdi;
	uint64_t length = f->rsi;
	uint64_t prot = f->rdx;
	uint64_t flags = f->r10;
	int mfd = (int)(int64_t) f->r8;
	uint64_t offset = f->r9;

	/* Validate length. */
	if (length == 0) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}

	/* Validate prot: only PROT_READ(1), PROT_WRITE(2), PROT_EXEC(4) bits allowed. */
	if (prot & ~(uint64_t) 7) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}

	uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t total = pages * PAGE_SIZE;

	proc_t *cur = current_proc;
	if (!cur) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	uint64_t va;
	if (flags & MAP_FIXED) {
		va = addr;
		int shared = pml4_is_shared(cur);
		/* Unmap existing pages in target range. */
		for (uint64_t off = 0; off < total; off += PAGE_SIZE) {
			uint64_t pa =
			    paging_unmap_page(cur->pml4_phys, va + off);
			if (pa) {
				invlpg(va + off);
				/*
				 * Flush remote TLBs BEFORE freeing so no
				 * sibling CPU writes through a stale entry.
				 * Verified: models/mmap_fixed_tlb.pml.
				 */
				if (shared)
					smp_flush_tlb();
				pmm_free_page(pa);
			}
		}
		vma_remove(cur, va, total);
	} else if ((flags & MAP_FIXED_NOREPLACE) && addr) {
		va = addr;
		for (uint64_t off = 0; off < total; off += PAGE_SIZE) {
			uint64_t *pte =
			    paging_walk_pte(cur->pml4_phys, va + off);
			if (pte && (*pte & PTE_PRESENT)) {
				f->rax = SYSCALL_ERR(EEXIST);
				return f->rax;
			}
		}
	} else {
		va = cur->mmap_next;
		cur->mmap_next += total;
	}

	/* File-backed: get the fd and node. */
	fd_entry_t *fentry = 0;
	vfs_node_t *fnode = 0;
	if (!(flags & MAP_ANONYMOUS)) {
		fentry = fd_get(mfd);
		if (!fentry || fentry->desc->type != FD_TYPE_FILE
		    || !fentry->desc->node) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		fnode = fentry->desc->node;
	}

	uint64_t pte_flags = prot_to_pte((uint8_t) prot);

	/* PROT_NONE: just record VMA, don't allocate pages. */
	if (pte_flags == 0) {
		if (vma_add(cur, va, total, 0) < 0) {
			f->rax = SYSCALL_ERR(ENOMEM);
			return f->rax;
		}
		f->rax = va;
		return va;
	}

	for (uint64_t off = 0; off < total; off += PAGE_SIZE) {
		uint64_t pa = pmm_alloc_page();
		if (pa == UINT64_MAX) {
			/* Roll back pages mapped so far.
			 * Verified: models/partial_alloc_rollback.pml. */
			for (uint64_t r = 0; r < off; r += PAGE_SIZE) {
				uint64_t rpa =
				    paging_unmap_page(cur->pml4_phys, va + r);
				if (rpa)
					pmm_free_page(rpa);
			}
			f->rax = SYSCALL_ERR(ENOMEM);
			return f->rax;
		}
		/* Zero the page via HHDM. */
		uint8_t *kva = (uint8_t *) phys_to_virt(pa);
		kmemzero(kva, PAGE_SIZE);

		/* Copy file contents if file-backed. */
		if (fnode) {
			uint64_t foff = offset + off;
			if (foff < fnode->size) {
				uint64_t tocopy = fnode->size - foff;
				if (tocopy > PAGE_SIZE)
					tocopy = PAGE_SIZE;
				if (fnode->data) {
					const uint8_t *src = fnode->data + foff;
					kmemcpy(kva, src, tocopy);
				} else {
					vfs_read(fnode, foff, kva, tocopy);
				}
			}
		}

		paging_map_range(cur->pml4_phys, va + off, pa, PAGE_SIZE,
				 pte_flags);
	}

	if (vma_add(cur, va, total, (uint8_t) prot) < 0) {
		/* Free the pages we just allocated. */
		for (uint64_t off = 0; off < total; off += PAGE_SIZE) {
			uint64_t pa =
			    paging_unmap_page(cur->pml4_phys, va + off);
			if (pa)
				pmm_free_page(pa);
		}
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	f->rax = va;
	return va;
}

static uint64_t
do_mprotect(syscall_frame_t * f)
{
	uint64_t addr = f->rdi;
	uint64_t length = f->rsi;
	uint64_t prot = f->rdx;

	proc_t *cur = current_proc;
	if (!cur) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}
	if (addr & (PAGE_SIZE - 1)) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}

	uint64_t end = (addr + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	uint64_t pte_flags = prot_to_pte((uint8_t) prot);

	/* Verify that at least one page in the range is mapped or in a VMA. */
	if (length > 0) {
		int any_mapped = 0;
		if (vma_find(cur, addr))
			any_mapped = 1;
		if (!any_mapped) {
			for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
				uint64_t *pte =
				    paging_walk_pte(cur->pml4_phys, va);
				if (pte && (*pte & PTE_PRESENT)) {
					any_mapped = 1;
					break;
				}
			}
		}
		/* Also check if addr is within the brk region. */
		if (!any_mapped && addr >= cur->brk_base && addr < cur->brk)
			any_mapped = 1;
		if (!any_mapped) {
			f->rax = SYSCALL_ERR(ENOMEM);
			return f->rax;
		}
	}

	for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
		uint64_t *pte = paging_walk_pte(cur->pml4_phys, va);
		if (pte && (*pte & PTE_PRESENT)) {
			uint64_t pa = *pte & PTE_ADDR_MASK;
			if (pte_flags) {
				/*
				 * Never hand write access to a shared frame: if the
				 * page is shared, map it read-only + COW so the next
				 * write faults and copies first (granting W directly
				 * would alias the frame across a fork).  When write is
				 * not granted, pte_flags carries no COW, so a later
				 * write to the now-read-only page faults to SIGSEGV.
				 * Verified: models/cow_vma_prot.pml, mprotect_revoke.pml.
				 */
				if ((pte_flags & PTE_WRITABLE)
				    && pmm_page_refcount(pa) > 1)
					pte_flags =
					    (pte_flags & ~PTE_WRITABLE) |
					    PTE_COW;
				*pte = pa | pte_flags;
			} else
				*pte = 0;	/* PROT_NONE: unmap. */
			invlpg(va);
		} else if (pte_flags) {
			/* Lazy alloc for formerly-PROT_NONE pages. */
			uint64_t pa = pmm_alloc_page();
			if (pa && pa != UINT64_MAX) {
				uint8_t *kva = (uint8_t *) phys_to_virt(pa);
				kmemzero(kva, PAGE_SIZE);
				paging_map_range(cur->pml4_phys, va, pa,
						 PAGE_SIZE, pte_flags);
				invlpg(va);
			}
		}
	}
	/* For shared address spaces: flush remote TLBs after PTE changes. */
	if (pml4_is_shared(cur))
		smp_flush_tlb();

	/* Update VMA prot if found. */
	vma_t *v = vma_find(cur, addr);
	if (v)
		v->prot = (uint8_t) prot;

	f->rax = 0;
	return 0;
}

static uint64_t
do_mremap(syscall_frame_t * f)
{
	uint64_t old_addr = f->rdi;
	uint64_t old_size = f->rsi;
	uint64_t new_size = f->rdx;
	uint64_t mrflags = f->r10;

	proc_t *cur = current_proc;
	if (!cur) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	if (new_size == 0) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}

	uint64_t old_pages = (old_size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t old_total = old_pages * PAGE_SIZE;
	uint64_t new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t new_total = new_pages * PAGE_SIZE;

	if (new_total <= old_total) {
		/* Shrink: unmap excess pages. */
		for (uint64_t off = new_total; off < old_total;
		     off += PAGE_SIZE) {
			uint64_t pa =
			    paging_unmap_page(cur->pml4_phys, old_addr + off);
			if (pa) {
				pmm_free_page(pa);
				invlpg(old_addr + off);
			}
		}
		/* Lazy TLB: no IPI needed (tlb_lazy.pml) */
		/* Update VMA. */
		vma_t *v = vma_find(cur, old_addr);
		if (v)
			v->length = new_total;
		f->rax = old_addr;
		return f->rax;
	}

	/* Try to grow in place: check if pages beyond old range are free. */
	{
		int can_grow = 1;
		for (uint64_t off = old_total; off < new_total;
		     off += PAGE_SIZE) {
			uint64_t *pte =
			    paging_walk_pte(cur->pml4_phys, old_addr + off);
			if (pte && (*pte & PTE_PRESENT)) {
				can_grow = 0;
				break;
			}
		}
		if (can_grow) {
			/* Determine prot from existing VMA. */
			vma_t *v = vma_find(cur, old_addr);
			uint8_t prot = v ? v->prot : (PROT_READ | PROT_WRITE);
			uint64_t pte_flags = prot_to_pte(prot);
			if (!pte_flags)
				pte_flags =
				    PTE_PRESENT | PTE_USER | PTE_WRITABLE;
			for (uint64_t off = old_total; off < new_total;
			     off += PAGE_SIZE) {
				uint64_t pa = pmm_alloc_page();
				if (pa == UINT64_MAX) {
					/* Roll back grow pages.
					 * Verified: models/partial_alloc_rollback.pml. */
					for (uint64_t r = old_total; r < off;
					     r += PAGE_SIZE) {
						uint64_t rpa =
						    paging_unmap_page(cur->pml4_phys,
								     old_addr + r);
						if (rpa)
							pmm_free_page(rpa);
					}
					f->rax = SYSCALL_ERR(ENOMEM);
					return f->rax;
				}
				uint8_t *kva = (uint8_t *) phys_to_virt(pa);
				kmemzero(kva, PAGE_SIZE);
				paging_map_range(cur->pml4_phys, old_addr + off,
						 pa, PAGE_SIZE, pte_flags);
			}
			if (v)
				v->length = new_total;
			/* Advance mmap_next past the grown region. */
			if (old_addr + new_total > cur->mmap_next)
				cur->mmap_next = old_addr + new_total;
			f->rax = old_addr;
			return f->rax;
		}
	}

	/* Move if MREMAP_MAYMOVE. */
	if (!(mrflags & MREMAP_MAYMOVE)) {
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	/* Allocate new region. */
	uint64_t new_addr = cur->mmap_next;
	cur->mmap_next += new_total;

	vma_t *v = vma_find(cur, old_addr);
	uint8_t prot = v ? v->prot : (PROT_READ | PROT_WRITE);
	uint64_t pte_flags = prot_to_pte(prot);
	if (!pte_flags)
		pte_flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;

	/* Alloc new pages and copy old data. */
	for (uint64_t off = 0; off < new_total; off += PAGE_SIZE) {
		uint64_t new_pa = pmm_alloc_page();
		if (new_pa == UINT64_MAX) {
			/* Roll back new pages mapped so far.
			 * Verified: models/partial_alloc_rollback.pml. */
			for (uint64_t r = 0; r < off; r += PAGE_SIZE) {
				uint64_t rpa =
				    paging_unmap_page(cur->pml4_phys,
						     new_addr + r);
				if (rpa)
					pmm_free_page(rpa);
			}
			f->rax = SYSCALL_ERR(ENOMEM);
			return f->rax;
		}
		uint8_t *new_kva = (uint8_t *) phys_to_virt(new_pa);
		kmemzero(new_kva, PAGE_SIZE);

		if (off < old_total) {
			/* Copy old page data. */
			uint64_t *old_pte =
			    paging_walk_pte(cur->pml4_phys, old_addr + off);
			if (old_pte && (*old_pte & PTE_PRESENT)) {
				uint64_t old_pa = *old_pte & PTE_ADDR_MASK;
				uint8_t *old_kva =
				    (uint8_t *) phys_to_virt(old_pa);
				kmemcpy(new_kva, old_kva, PAGE_SIZE);
			}
		}

		paging_map_range(cur->pml4_phys, new_addr + off, new_pa,
				 PAGE_SIZE, pte_flags);
	}

	/* Unmap old pages -- flush remote TLBs before freeing if shared. */
	{
		int _shared = pml4_is_shared(cur);
		for (uint64_t off = 0; off < old_total; off += PAGE_SIZE) {
			uint64_t pa =
			    paging_unmap_page(cur->pml4_phys, old_addr + off);
			if (pa) {
				invlpg(old_addr + off);
				if (_shared)
					smp_flush_tlb();
				pmm_free_page(pa);
			}
		}
	}

	/* Update VMAs. */
	vma_remove(cur, old_addr, old_total);
	(void)vma_add(cur, new_addr, new_total, prot);

	f->rax = new_addr;
	return f->rax;
}

static uint64_t
do_munmap(syscall_frame_t * f)
{
	uint64_t addr = f->rdi;
	uint64_t length = f->rsi;

	proc_t *cur = current_proc;
	if (!cur) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}
	if (addr & (PAGE_SIZE - 1)) {
		f->rax = SYSCALL_ERR(EINVAL);
		return f->rax;
	}

	uint64_t end = (addr + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	int shared = pml4_is_shared(cur);
	for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
		uint64_t pa = paging_unmap_page(cur->pml4_phys, va);
		if (pa) {
			invlpg(va);
			/*
			 * For shared address spaces (CLONE_VM threads): flush remote
			 * TLBs BEFORE freeing, so no sibling CPU writes to the freed
			 * page through a stale TLB entry.  Verified: unified_smp.pml P9.
			 */
			if (shared)
				smp_flush_tlb();
			pmm_free_page(pa);
		}
	}

	vma_remove(cur, addr, end - addr);

	f->rax = 0;
	return 0;
}

uint64_t
syscall_handle_mm(syscall_frame_t * f)
{
	switch (f->rax) {
	case SYS_BRK:
		return do_brk(f);
	case SYS_MMAP:
		return do_mmap(f);
	case SYS_MPROTECT:
		return do_mprotect(f);
	case SYS_MREMAP:
		return do_mremap(f);
	case SYS_MUNMAP:
		return do_munmap(f);
	default:
		f->rax = SYSCALL_ERR(ENOSYS);
		return f->rax;
	}
}
