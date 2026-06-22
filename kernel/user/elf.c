/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "ember/elf.h"
#include "ember/vfs.h"
#include "ember/paging.h"
#include "ember/pmm.h"
#include "ember/mmu.h"
#include "ember/console.h"
#include "ember/heap.h"
#include "ember/syscall.h"

static int
elf_validate(const elf64_ehdr_t * eh)
{
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E'
	    || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		return 0;
	if (eh->e_ident[4] != 2)
		return 0;	/* ELF64. */
	if (eh->e_machine != 0x3e)
		return 0;	/* x86_64. */
	return 1;
}

/*
 * Copy src_len bytes to user virtual address uva via HHDM (no CR3 switch).
 * Pages must already be mapped in pml4_phys. Uses 8-byte bulk copies.
 */
static void
copy_to_user(uint64_t pml4_phys, uint64_t uva,
	     const uint8_t * src, uint64_t src_len)
{
	uint64_t done = 0;
	while (done < src_len) {
		uint64_t *pte = paging_walk_pte(pml4_phys, uva + done);
		if (!pte)
			return;
		uint64_t page_off = (uva + done) & (PAGE_SIZE - 1);
		uint8_t *dst =
		    (uint8_t *) phys_to_virt(*pte & PTE_ADDR_MASK) + page_off;
		uint64_t chunk = PAGE_SIZE - page_off;
		if (chunk > src_len - done)
			chunk = src_len - done;
		uint64_t j = 0;
		for (; j + 8 <= chunk; j += 8)
			*(uint64_t *) (dst + j) =
			    *(const uint64_t *)(src + done + j);
		for (; j < chunk; j++)
			dst[j] = src[done + j];
		done += chunk;
	}
}

/* Zero len bytes at user virtual address uva via HHDM (no CR3 switch). */
static void
zero_user(uint64_t pml4_phys, uint64_t uva, uint64_t len)
{
	uint64_t done = 0;
	while (done < len) {
		uint64_t *pte = paging_walk_pte(pml4_phys, uva + done);
		if (!pte)
			return;
		uint64_t page_off = (uva + done) & (PAGE_SIZE - 1);
		uint8_t *dst =
		    (uint8_t *) phys_to_virt(*pte & PTE_ADDR_MASK) + page_off;
		uint64_t chunk = PAGE_SIZE - page_off;
		if (chunk > len - done)
			chunk = len - done;
		uint64_t j = 0;
		for (; j + 8 <= chunk; j += 8)
			*(uint64_t *) (dst + j) = 0;
		for (; j < chunk; j++)
			dst[j] = 0;
		done += chunk;
	}
}

int
elf_load_user(vfs_node_t * node, uint64_t pml4, elf_info_t * info)
{
	if (!node || !info)
		return 0;

	/*
	 * Read the ELF header. If data is in memory (cpio), use directly.
	 * Otherwise read via vfs_read() for ext2-backed nodes.
	 */
	elf64_ehdr_t ehdr_buf;
	const elf64_ehdr_t *eh;

	if (node->data) {
		eh = (const elf64_ehdr_t *)node->data;
	} else {
		uint64_t hdr_nr =
		    vfs_read(node, 0, &ehdr_buf, sizeof(ehdr_buf));
		if (hdr_nr < sizeof(ehdr_buf)) {
			console_write("elf_load: hdr read fail got=");
			console_hex64(hdr_nr);
			console_write(" need=");
			console_hex64(sizeof(ehdr_buf));
			console_write("\n");
			return 0;
		}
		eh = &ehdr_buf;
	}

	if (!elf_validate(eh)) {
		console_write("elf_load: validate fail magic=");
		console_hex64(*(const uint64_t *)eh);
		console_write("\n");
		return 0;
	}
	if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
		console_write("elf_load: bad type=");
		console_hex64((uint64_t) eh->e_type);
		console_write("\n");
		return 0;
	}

	/* PIE (ET_DYN) binaries have p_vaddr starting at 0; load at a fixed base. */
	uint64_t base = 0;
	if (eh->e_type == ET_DYN)
		base = 0x400000;

	/* Read all program headers. */
	uint64_t phdr_size = (uint64_t) eh->e_phnum * eh->e_phentsize;
	uint8_t *phdr_buf = 0;
	const uint8_t *phdr_data;

	if (node->data) {
		phdr_data = node->data + eh->e_phoff;
	} else {
		phdr_buf = (uint8_t *) kmalloc(phdr_size);
		if (!phdr_buf)
			return 0;
		if (vfs_read(node, eh->e_phoff, phdr_buf, phdr_size) <
		    phdr_size) {
			kfree(phdr_buf);
			return 0;
		}
		phdr_data = phdr_buf;
	}

	/* Save header info before we potentially free phdr_buf. */
	uint64_t e_entry = eh->e_entry;
	uint16_t e_phentsize = eh->e_phentsize;
	uint16_t e_phnum = eh->e_phnum;
	uint64_t e_phoff = eh->e_phoff;

	uint64_t max_end = 0;
	uint64_t first_load_vaddr = 0;
	uint64_t first_load_offset = 0;
	int found_first_load = 0;

	info->nsegs = 0;

	for (uint16_t i = 0; i < e_phnum; i++) {
		const elf64_phdr_t *ph =
		    (const elf64_phdr_t *)(phdr_data +
					   (uint64_t) i * e_phentsize);
		if (ph->p_type != PT_LOAD)
			continue;

		uint64_t vaddr = ph->p_vaddr + base;

		if (!found_first_load) {
			first_load_vaddr = vaddr;
			first_load_offset = ph->p_offset;
			found_first_load = 1;
		}

		uint64_t seg_end = vaddr + ph->p_memsz;
		if (seg_end > max_end)
			max_end = seg_end;

		uint64_t seg_start = vaddr & ~(PAGE_SIZE - 1);
		uint64_t seg_end_aligned =
		    (seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

		/*
		 * Honor segment protection (W^X): code maps read-execute, data
		 * read-write, rodata read-only.  The VMA carries the same prot
		 * so the page-fault path enforces it.
		 */
		uint8_t prot = 0;
		if (ph->p_flags & PF_R)
			prot |= PROT_READ;
		if (ph->p_flags & PF_W)
			prot |= PROT_WRITE;
		if (ph->p_flags & PF_X)
			prot |= PROT_EXEC;

		if (info->nsegs < ELF_MAX_SEGS) {
			info->segs[info->nsegs].vaddr = seg_start;
			info->segs[info->nsegs].len =
			    seg_end_aligned - seg_start;
			info->segs[info->nsegs].prot = prot;
			info->nsegs++;
		}

		uint64_t flags = PTE_PRESENT | PTE_USER;
		if (ph->p_flags & PF_W)
			flags |= PTE_WRITABLE;
		if (!(ph->p_flags & PF_X))
			flags |= PTE_NX;

		for (uint64_t va = seg_start; va < seg_end_aligned;
		     va += PAGE_SIZE) {
			/*
			 * If a previous PT_LOAD already mapped this page (segments
			 * sharing a boundary page), reuse it but take the UNION of
			 * permissions: a writable or executable segment sharing the
			 * page must win, else a write to .data on the shared page
			 * would fault.  Verified: models/elf_overlap.pml,
			 * models/wx_shared_page.pml.
			 */
			uint64_t *pte = paging_walk_pte(pml4, va);
			if (pte && (*pte & PTE_PRESENT)) {
				if (flags & PTE_WRITABLE)
					*pte |= PTE_WRITABLE;
				if (!(flags & PTE_NX))
					*pte &= ~PTE_NX;
				continue;
			}
			uint64_t pa = pmm_alloc_page();
			if (pa == UINT64_MAX) {
				if (phdr_buf)
					kfree(phdr_buf);
				return 0;
			}
			paging_map_range(pml4, va, pa, PAGE_SIZE, flags);
		}

		/* Copy segment data to user pages via HHDM -- no CR3 switch needed. */
		if (node->data) {
			copy_to_user(pml4, vaddr, node->data + ph->p_offset,
				     ph->p_filesz);
		} else {
			/* Ext2-backed: read via VFS in chunks, copy via HHDM. */
			uint64_t remaining = ph->p_filesz;
			uint64_t file_off = ph->p_offset;
			uint64_t seg_off = 0;
			uint8_t bounce[512];
			while (remaining > 0) {
				uint64_t chunk =
				    remaining > 512 ? 512 : remaining;
				uint64_t nr =
				    vfs_read(node, file_off, bounce, chunk);
				if (nr == 0)
					break;
				copy_to_user(pml4, vaddr + seg_off, bounce, nr);
				file_off += nr;
				seg_off += nr;
				remaining -= nr;
			}
		}

		/* Zero BSS portion via HHDM. */
		if (ph->p_memsz > ph->p_filesz)
			zero_user(pml4, vaddr + ph->p_filesz,
				  ph->p_memsz - ph->p_filesz);
	}

	if (phdr_buf)
		kfree(phdr_buf);

	info->entry = e_entry + base;
	info->phentsize = e_phentsize;
	info->phnum = e_phnum;
	info->brk_base = (max_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	info->phdr_vaddr = first_load_vaddr + (e_phoff - first_load_offset);

	return 1;
}
