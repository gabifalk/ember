/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "../include/uefi_min.h"
#include "../include/boot_info.h"
#include "ember/mmu.h"

/*
 * UEFI uses Microsoft x64 calling convention: rcx, rdx, r8, r9, stack.
 * We receive args in SysV ABI: rdi, rsi, rdx, rcx, r8, r9.
 * Implemented in uefi_call.S. With ember-ld, PLT32 relocations are
 * resolved as direct PC-relative calls, so no PLT issues.
 */
extern uint64_t uefi_call2(void *fn, uint64_t a1, uint64_t a2);
extern uint64_t uefi_call3(void *fn, uint64_t a1, uint64_t a2, uint64_t a3);
extern uint64_t uefi_call4(void *fn, uint64_t a1, uint64_t a2,
			   uint64_t a3, uint64_t a4);
extern uint64_t uefi_call5(void *fn, uint64_t a1, uint64_t a2, uint64_t a3,
			   uint64_t a4, uint64_t a5);

/* Minimal ELF64 definitions for kernel loading. */
#define ELF_MAGIC 0x464C457F	/* "\X7fELF" as uint32_t. */
#define PT_LOAD   1

typedef struct {
	uint32_t magic;
	uint8_t class, data, ei_version, osabi;
	uint8_t pad[8];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum;
	uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

/* Information about the loaded kernel, filled by load_kernel_elf() */
typedef struct {
	uint64_t entry;		/* Kmain virtual address (from e_entry) */
	uint64_t vma_base;	/* Lowest PT_LOAD vaddr (0xffffffff80000000) */
	uint64_t phys_base;	/* Physical address where first segment was loaded. */
	uint64_t total_memsz;	/* Total virtual span (vma_base to end of last segment) */
} kernel_load_info_t;

/* Minimal freestanding helpers. */
static void *
memset_local(void *dst, int c, size_t n)
{
	uint8_t *p = (uint8_t *) dst;
	while (n--) {
		*p++ = (uint8_t) c;
	}
	return dst;
}

static void *
memcpy_local(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *) dst;
	const uint8_t *s = (const uint8_t *)src;
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

/* I/O ports. */
static inline void
outb(uint16_t port, uint8_t val)
{
	__asm__ __volatile__("outb %0, %1"::"a"(val), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
	uint8_t ret;
	__asm__ __volatile__("inb %1, %0":"=a"(ret):"Nd"(port));
	return ret;
}

/* Serial (COM1) */
static void
serial_init(void)
{
	const uint16_t base = 0x3F8;
	outb(base + 1, 0x00);	/* Disable interrupts. */
	outb(base + 3, 0x80);	/* Enable DLAB. */
	outb(base + 0, 0x01);	/* 115200 Baud divisor lo. */
	outb(base + 1, 0x00);	/* Divisor hi. */
	outb(base + 3, 0x03);	/* 8N1. */
	outb(base + 2, 0xC7);	/* FIFO enable. */
	outb(base + 4, 0x0B);	/* IRQs disabled, RTS/DSR set. */
}

static void
serial_putc(char c)
{
	const uint16_t base = 0x3F8;
	/* Wait for THR empty. */
	while ((inb(base + 5) & 0x20) == 0) {
	}
	outb(base, (uint8_t) c);
}

static void
serial_write(const char *s)
{
	while (*s) {
		if (*s == '\n') {
			serial_putc('\r');
		}
		serial_putc(*s++);
	}
}

static void
serial_hex64(uint64_t v)
{
	serial_write("0x");
	for (int i = 60; i >= 0; i -= 4) {
		int d = (v >> i) & 0xF;
		serial_putc(d < 10 ? '0' + d : 'a' + d - 10);
	}
}

#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

static void *
alloc_pages_max(EFI_BOOT_SERVICES * bs, size_t pages, uint64_t * max_addr)
{
	EFI_STATUS st = uefi_call4((void *)bs->AllocatePages,
				   (uint64_t) AllocateMaxAddress,
				   (uint64_t) EfiLoaderData,
				   (uint64_t) pages,
				   (uint64_t) (uintptr_t) max_addr);
	if (EFI_ERROR(st)) {
		return NULL;
	}
	return (void *)(uintptr_t) (*max_addr);
}

static void *
alloc_pool(EFI_BOOT_SERVICES * bs, size_t size)
{
	void *buf = NULL;
	EFI_STATUS st = uefi_call3((void *)bs->AllocatePool,
				   (uint64_t) EfiBootServicesData,
				   size,
				   (uint64_t) (uintptr_t) & buf);
	if (EFI_ERROR(st)) {
		return NULL;
	}
	return buf;
}

static void *
alloc_pages(EFI_BOOT_SERVICES * bs, size_t size)
{
	uint64_t addr = 0;
	size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	EFI_STATUS st = uefi_call4((void *)bs->AllocatePages,
				   (uint64_t) AllocateAnyPages,
				   (uint64_t) EfiLoaderData,
				   (uint64_t) pages,
				   (uint64_t) (uintptr_t) & addr);
	if (EFI_ERROR(st)) {
		return NULL;
	}
	return (void *)(uintptr_t) addr;
}

static uint8_t *pt_pool_base;
static size_t pt_pool_pages;
static size_t pt_pool_used;

static uint64_t
alloc_pt_page(void)
{
	if (pt_pool_used >= pt_pool_pages) {
		serial_write("FATAL: page table pool exhausted\n");
		for (;;)
			__asm__ __volatile__("hlt");
	}
	uint8_t *p = pt_pool_base + pt_pool_used * PAGE_SIZE;
	pt_pool_used++;
	memset_local(p, 0, PAGE_SIZE);
	return (uint64_t) (uintptr_t) p;
}

#define LARGE_PAGE_SIZE 0x200000ULL	/* 2 MiB. */
#define LARGE_PAGE_MASK (LARGE_PAGE_SIZE - 1)

static void
map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	uint64_t *pml4 = (uint64_t *) (uintptr_t) pml4_phys;
	uint64_t pml4_i = (vaddr >> 39) & 0x1ff;
	uint64_t pdpt_i = (vaddr >> 30) & 0x1ff;
	uint64_t pd_i = (vaddr >> 21) & 0x1ff;
	uint64_t pt_i = (vaddr >> 12) & 0x1ff;

	if (!(pml4[pml4_i] & PTE_PRESENT)) {
		uint64_t new_pdpt = alloc_pt_page();
		pml4[pml4_i] = new_pdpt | PTE_PRESENT | PTE_WRITABLE;
	}
	uint64_t *pdpt =
	    (uint64_t *) (uintptr_t) (pml4[pml4_i] & PTE_ADDR_MASK);

	if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
		uint64_t new_pd = alloc_pt_page();
		pdpt[pdpt_i] = new_pd | PTE_PRESENT | PTE_WRITABLE;
	}
	uint64_t *pd = (uint64_t *) (uintptr_t) (pdpt[pdpt_i] & PTE_ADDR_MASK);

	if (pd[pd_i] & PTE_PS) {
		/* Split a 2MB page into 512 x 4KB pages. */
		uint64_t large_pa = pd[pd_i] & ~LARGE_PAGE_MASK;
		uint64_t old_flags = pd[pd_i] & (PTE_PRESENT | PTE_WRITABLE);
		uint64_t new_pt = alloc_pt_page();
		for (int k = 0; k < 512; k++)
			((uint64_t *) (uintptr_t) new_pt)[k] =
			    (large_pa + k * PAGE_SIZE) | old_flags;
		pd[pd_i] = new_pt | PTE_PRESENT | PTE_WRITABLE;
	} else if (!(pd[pd_i] & PTE_PRESENT)) {
		uint64_t new_pt = alloc_pt_page();
		pd[pd_i] = new_pt | PTE_PRESENT | PTE_WRITABLE;
	}
	uint64_t *pt = (uint64_t *) (uintptr_t) (pd[pd_i] & PTE_ADDR_MASK);

	pt[pt_i] = (paddr & PTE_ADDR_MASK) | flags;
}

static void
map_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t size,
	  uint64_t flags)
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

/* Map a single 2MB page: PD entry with PS bit set, bypassing PT level. */
static void
map_page_2mb(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
	uint64_t *pml4 = (uint64_t *) (uintptr_t) pml4_phys;
	uint64_t pml4_i = (vaddr >> 39) & 0x1ff;
	uint64_t pdpt_i = (vaddr >> 30) & 0x1ff;
	uint64_t pd_i = (vaddr >> 21) & 0x1ff;

	if (!(pml4[pml4_i] & PTE_PRESENT)) {
		uint64_t new_pdpt = alloc_pt_page();
		pml4[pml4_i] = new_pdpt | PTE_PRESENT | PTE_WRITABLE;
	}
	uint64_t *pdpt =
	    (uint64_t *) (uintptr_t) (pml4[pml4_i] & PTE_ADDR_MASK);

	if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
		uint64_t new_pd = alloc_pt_page();
		pdpt[pdpt_i] = new_pd | PTE_PRESENT | PTE_WRITABLE;
	}
	uint64_t *pd = (uint64_t *) (uintptr_t) (pdpt[pdpt_i] & PTE_ADDR_MASK);

	/* PD entry with PS=1 points directly to a 2MB physical frame. */
	pd[pd_i] = (paddr & ~LARGE_PAGE_MASK) | flags | PTE_PS;
}

/* Map a range using 2MB pages where aligned, 4KB pages for unaligned edges. */
static void
map_range_large(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
		uint64_t size, uint64_t flags)
{
	uint64_t va = vaddr;
	uint64_t pa = paddr;
	uint64_t end = vaddr + size;

	/* Leading unaligned 4KB pages. */
	while (va < end && (va & LARGE_PAGE_MASK)) {
		map_page(pml4_phys, va, pa, flags);
		va += PAGE_SIZE;
		pa += PAGE_SIZE;
	}

	/* Bulk 2MB-aligned pages. */
	while (va + LARGE_PAGE_SIZE <= end && !(pa & LARGE_PAGE_MASK)) {
		map_page_2mb(pml4_phys, va, pa, flags);
		va += LARGE_PAGE_SIZE;
		pa += LARGE_PAGE_SIZE;
	}

	/* Trailing unaligned 4KB pages. */
	while (va < end) {
		map_page(pml4_phys, va, pa, flags);
		va += PAGE_SIZE;
		pa += PAGE_SIZE;
	}
}

static inline void
write_cr3(uint64_t pml4_phys)
{
	__asm__ __volatile__("mov %0, %%cr3"::"r"(pml4_phys):"memory");
}

/*
 * Load a file from the ESP into allocated pages.
 * Returns 0 on success, sets *out_buf and *out_size.
 */
static int
load_file_from_esp(EFI_BOOT_SERVICES * bs, const uint16_t * path,
		   uint8_t ** out_buf, uint64_t * out_size)
{
	EFI_STATUS st;
	size_t handle_buf_size = 0;
	st = uefi_call5((void *)bs->LocateHandle,
			(uint64_t) 2 /* ByProtocol. */ ,
			(uint64_t) (uintptr_t) &
			EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
			(uint64_t) (uintptr_t) NULL,
			(uint64_t) (uintptr_t) & handle_buf_size,
			(uint64_t) (uintptr_t) NULL);
	if (handle_buf_size == 0)
		return 1;
	EFI_HANDLE *handles = (EFI_HANDLE *) alloc_pool(bs, handle_buf_size);
	if (!handles)
		return 1;
	st = uefi_call5((void *)bs->LocateHandle,
			(uint64_t) 2,
			(uint64_t) (uintptr_t) &
			EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
			(uint64_t) (uintptr_t) NULL,
			(uint64_t) (uintptr_t) & handle_buf_size,
			(uint64_t) (uintptr_t) handles);
	if (EFI_ERROR(st))
		return 1;

	*out_buf = NULL;
	*out_size = 0;
	size_t num_handles = handle_buf_size / sizeof(EFI_HANDLE);
	for (size_t i = 0; i < num_handles && !*out_buf; i++) {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
		st = uefi_call3((void *)bs->HandleProtocol,
				(uint64_t) (uintptr_t) handles[i],
				(uint64_t) (uintptr_t) &
				EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
				(uint64_t) (uintptr_t) & sfs);
		if (EFI_ERROR(st) || !sfs)
			continue;

		EFI_FILE_PROTOCOL *root = NULL;
		st = uefi_call2((void *)sfs->OpenVolume,
				(uint64_t) (uintptr_t) sfs,
				(uint64_t) (uintptr_t) & root);
		if (EFI_ERROR(st) || !root)
			continue;

		EFI_FILE_PROTOCOL *file = NULL;
		st = uefi_call5((void *)root->Open,
				(uint64_t) (uintptr_t) root,
				(uint64_t) (uintptr_t) & file,
				(uint64_t) (uintptr_t) path,
				(uint64_t) 0x0000000000000001ULL
				/* EFI_FILE_MODE_READ. */ ,
				(uint64_t) 0);
		if (EFI_ERROR(st) || !file) {
			uefi_call2((void *)root->Close,
				   (uint64_t) (uintptr_t) root, 0);
			continue;
		}

		uint8_t info_buf[128];
		uint64_t info_size = sizeof(info_buf);
		st = uefi_call4((void *)file->GetInfo,
				(uint64_t) (uintptr_t) file,
				(uint64_t) (uintptr_t) & EFI_FILE_INFO_GUID,
				(uint64_t) (uintptr_t) & info_size,
				(uint64_t) (uintptr_t) info_buf);
		if (!EFI_ERROR(st)) {
			EFI_FILE_INFO *finfo = (EFI_FILE_INFO *) info_buf;
			uint64_t file_size = finfo->FileSize;
			if (file_size > 0) {
				uint8_t *buf =
				    (uint8_t *) alloc_pages(bs, file_size);
				if (buf) {
					uint64_t read_size = file_size;
					st = uefi_call3((void *)file->Read,
							(uint64_t) (uintptr_t)
							file,
							(uint64_t) (uintptr_t) &
							read_size,
							(uint64_t) (uintptr_t)
							buf);
					if (!EFI_ERROR(st)
					    && read_size == file_size) {
						*out_buf = buf;
						*out_size = file_size;
					}
				}
			}
		}
		uefi_call2((void *)file->Close, (uint64_t) (uintptr_t) file, 0);
		uefi_call2((void *)root->Close, (uint64_t) (uintptr_t) root, 0);
	}

	return *out_buf ? 0 : 1;
}

/*
 * Load kernel ELF from ESP, allocate pages, copy segments.
 * Returns 0 on success.
 */
static int
load_kernel_elf(EFI_BOOT_SERVICES * bs, kernel_load_info_t * info)
{
	static const uint16_t kern_path[] = {
		'\\', 'b', 'o', 'o', 't', '\\', 'k', 'e', 'r', 'n', 'e', 'l',
		    '.', 'e', 'l', 'f', 0
	};
	EFI_STATUS st;

	/* Find filesystem handles. */
	size_t handle_buf_size = 0;
	st = uefi_call5((void *)bs->LocateHandle,
			(uint64_t) 2 /* ByProtocol. */ ,
			(uint64_t) (uintptr_t) &
			EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
			(uint64_t) (uintptr_t) NULL,
			(uint64_t) (uintptr_t) & handle_buf_size,
			(uint64_t) (uintptr_t) NULL);
	if (handle_buf_size == 0) {
		serial_write("no filesystem handles\n");
		return 1;
	}
	EFI_HANDLE *handles = (EFI_HANDLE *) alloc_pool(bs, handle_buf_size);
	if (!handles)
		return 1;
	st = uefi_call5((void *)bs->LocateHandle,
			(uint64_t) 2,
			(uint64_t) (uintptr_t) &
			EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
			(uint64_t) (uintptr_t) NULL,
			(uint64_t) (uintptr_t) & handle_buf_size,
			(uint64_t) (uintptr_t) handles);
	if (EFI_ERROR(st))
		return 1;

	/* Try each volume to find kernel.elf. */
	uint8_t *elf_buf = NULL;
	uint64_t elf_size = 0;
	size_t num_handles = handle_buf_size / sizeof(EFI_HANDLE);
	for (size_t i = 0; i < num_handles && !elf_buf; i++) {
		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
		st = uefi_call3((void *)bs->HandleProtocol,
				(uint64_t) (uintptr_t) handles[i],
				(uint64_t) (uintptr_t) &
				EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
				(uint64_t) (uintptr_t) & sfs);
		if (EFI_ERROR(st) || !sfs)
			continue;

		EFI_FILE_PROTOCOL *root = NULL;
		st = uefi_call2((void *)sfs->OpenVolume,
				(uint64_t) (uintptr_t) sfs,
				(uint64_t) (uintptr_t) & root);
		if (EFI_ERROR(st) || !root)
			continue;

		EFI_FILE_PROTOCOL *file = NULL;
		st = uefi_call5((void *)root->Open,
				(uint64_t) (uintptr_t) root,
				(uint64_t) (uintptr_t) & file,
				(uint64_t) (uintptr_t) kern_path,
				(uint64_t) 0x0000000000000001ULL
				/* EFI_FILE_MODE_READ. */ ,
				(uint64_t) 0);
		if (EFI_ERROR(st) || !file) {
			uefi_call2((void *)root->Close,
				   (uint64_t) (uintptr_t) root, 0);
			continue;
		}

		/* Get file size. */
		uint8_t info_buf[128];
		uint64_t info_size = sizeof(info_buf);
		st = uefi_call4((void *)file->GetInfo,
				(uint64_t) (uintptr_t) file,
				(uint64_t) (uintptr_t) & EFI_FILE_INFO_GUID,
				(uint64_t) (uintptr_t) & info_size,
				(uint64_t) (uintptr_t) info_buf);
		if (!EFI_ERROR(st)) {
			EFI_FILE_INFO *finfo = (EFI_FILE_INFO *) info_buf;
			elf_size = finfo->FileSize;
			elf_buf = (uint8_t *) alloc_pages(bs, elf_size);
			if (elf_buf) {
				uint64_t read_size = elf_size;
				st = uefi_call3((void *)file->Read,
						(uint64_t) (uintptr_t) file,
						(uint64_t) (uintptr_t) &
						read_size,
						(uint64_t) (uintptr_t) elf_buf);
				if (EFI_ERROR(st) || read_size != elf_size) {
					serial_write
					    ("kernel.elf read failed\n");
					elf_buf = NULL;
				}
			}
		}
		uefi_call2((void *)file->Close, (uint64_t) (uintptr_t) file, 0);
		uefi_call2((void *)root->Close, (uint64_t) (uintptr_t) root, 0);
	}

	if (!elf_buf) {
		serial_write("FATAL: kernel.elf not found on ESP\n");
		return 1;
	}

	serial_write("kernel.elf loaded, size=");
	serial_hex64(elf_size);
	serial_write("\n");

	/* Parse ELF header. */
	Elf64_Ehdr *ehdr = (Elf64_Ehdr *) elf_buf;
	if (ehdr->magic != ELF_MAGIC || ehdr->class != 2
	    || ehdr->e_machine != 0x3E) {
		serial_write("FATAL: kernel.elf bad ELF header\n");
		return 1;
	}

	info->entry = ehdr->e_entry;

	/* Scan PT_LOAD segments: find total virtual span. */
	Elf64_Phdr *phdrs = (Elf64_Phdr *) (elf_buf + ehdr->e_phoff);
	uint64_t vma_lo = ~0ULL, vma_hi = 0;
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;
		if (phdrs[i].p_vaddr < vma_lo)
			vma_lo = phdrs[i].p_vaddr;
		uint64_t seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
		if (seg_end > vma_hi)
			vma_hi = seg_end;
	}

	info->vma_base = vma_lo;
	info->total_memsz = vma_hi - vma_lo;

	/* Allocate contiguous physical pages for the entire kernel image. */
	uint64_t total_pages = (info->total_memsz + 0xFFF) >> 12;
	uint8_t *kphys = (uint8_t *) alloc_pages(bs, total_pages << 12);
	if (!kphys) {
		serial_write("FATAL: kernel page alloc failed\n");
		return 1;
	}
	/* Zero everything (covers BSS) */
	memset_local(kphys, 0, total_pages << 12);
	info->phys_base = (uint64_t) (uintptr_t) kphys;

	/* Copy PT_LOAD file data into allocated pages. */
	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;
		uint64_t off_in_image = phdrs[i].p_vaddr - vma_lo;
		uint8_t *dst = kphys + off_in_image;
		uint8_t *src = elf_buf + phdrs[i].p_offset;
		for (uint64_t j = 0; j < phdrs[i].p_filesz; j++)
			dst[j] = src[j];
	}

	serial_write("kernel loaded: vma=");
	serial_hex64(info->vma_base);
	serial_write(" phys=");
	serial_hex64(info->phys_base);
	serial_write(" size=");
	serial_hex64(info->total_memsz);
	serial_write(" entry=");
	serial_hex64(info->entry);
	serial_write("\n");

	return 0;
}

/* Find a UCS-2 keyword in load options. Returns index or -1. */
static int
cmdline_find(const uint16_t * opts, uint32_t len,
	     const uint16_t * needle, uint32_t nlen)
{
	for (uint32_t i = 0; i + nlen <= len; i++) {
		uint32_t j;
		for (j = 0; j < nlen; j++) {
			if (opts[i + j] != needle[j])
				break;
		}
		if (j == nlen)
			return (int)i;
	}
	return -1;
}

/* Parse a hex number from UCS-2, advancing *pos. Skips optional "0x" prefix. */
static uint64_t
parse_hex(const uint16_t * opts, uint32_t len, uint32_t * pos)
{
	uint32_t p = *pos;
	if (p + 1 < len && opts[p] == '0'
	    && (opts[p + 1] == 'x' || opts[p + 1] == 'X'))
		p += 2;
	uint64_t val = 0;
	while (p < len && opts[p] != ',' && opts[p] != ' ' && opts[p] != 0) {
		uint16_t c = opts[p++];
		val <<= 4;
		if (c >= '0' && c <= '9')
			val |= c - '0';
		else if (c >= 'a' && c <= 'f')
			val |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			val |= c - 'A' + 10;
		else
			break;
	}
	*pos = p;
	return val;
}

EFI_STATUS
efi_main_c(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE * SystemTable)
{
	EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
	EFI_STATUS st;

	serial_init();
	serial_write("Ember EFI loader start\n");
	(void)SystemTable;

	boot_info_v1_t *bi = (boot_info_v1_t *) alloc_pages(bs, sizeof(*bi));
	if (!bi) {
		serial_write("boot_info alloc failed\n");
		return 1;
	}
	memset_local(bi, 0, sizeof(*bi));
	bi->boot_info_version = BOOT_INFO_VERSION;

	/* Get loaded image info (used to reserve loader image range) */
	EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
	st = uefi_call3((void *)bs->HandleProtocol,
			(uint64_t) (uintptr_t) ImageHandle,
			(uint64_t) (uintptr_t) & EFI_LOADED_IMAGE_PROTOCOL_GUID,
			(uint64_t) (uintptr_t) & loaded);
	if (!EFI_ERROR(st) && loaded) {
		uintptr_t base = (uintptr_t) loaded->ImageBase;
		bi->kernel_phys_base = (uint64_t) base;
		bi->kernel_phys_end = (uint64_t) (base + loaded->ImageSize);
	}

	/* Load kernel ELF from ESP. */
	kernel_load_info_t kinfo;
	if (load_kernel_elf(bs, &kinfo)) {
		serial_write("kernel load failed\n");
		return 1;
	}
	bi->kernel_phys_base = kinfo.phys_base;
	bi->kernel_phys_end = kinfo.phys_base + kinfo.total_memsz;

	/* Locate GOP. */
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
	st = uefi_call3((void *)bs->LocateProtocol,
			(uint64_t) (uintptr_t) &
			EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
			(uint64_t) (uintptr_t) NULL,
			(uint64_t) (uintptr_t) & gop);
	if (!EFI_ERROR(st) && gop && gop->Mode && gop->Mode->Info) {
		EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
		bi->fb_phys_base = gop->Mode->FrameBufferBase;
		bi->fb_size = gop->Mode->FrameBufferSize;
		bi->fb_width = info->HorizontalResolution;
		bi->fb_height = info->VerticalResolution;
		bi->fb_stride_pixels = info->PixelsPerScanLine;
		bi->fb_bpp = 32;
		bi->fb_format = (uint32_t) info->PixelFormat;	/* Map later. */
	}

	bi->initrd_phys_base = 0;
	bi->initrd_size = 0;

	/*
	 * Parse load options for initrd arguments (UCS-2 in UEFI).
	 * initrd_address=0xADDR,0xSIZE   -- already in memory
	 * initrd=\path\to\file           -- load from ESP.
	 */
	if (loaded && loaded->LoadOptions && loaded->LoadOptionsSize > 0) {
		const uint16_t *opts = (const uint16_t *)loaded->LoadOptions;
		uint32_t opts_len = loaded->LoadOptionsSize / 2;

		static const uint16_t kw_ia[] = {
			'i', 'n', 'i', 't', 'r', 'd', '_', 'a', 'd', 'd', 'r',
			    'e', 's', 's', '='
		};
		static const uint16_t kw_ip[] =
		    { 'i', 'n', 'i', 't', 'r', 'd', '=' };
		int ia = cmdline_find(opts, opts_len, kw_ia, 15);
		int ip = cmdline_find(opts, opts_len, kw_ip, 7);

		if (ia >= 0) {
			/* initrd_address=0xADDR,0xSIZE. */
			uint32_t p = (uint32_t) ia + 15;
			uint64_t addr = parse_hex(opts, opts_len, &p);
			uint64_t size = 0;
			if (p < opts_len && opts[p] == ',') {
				p++;
				size = parse_hex(opts, opts_len, &p);
			}
			if (addr && size) {
				bi->initrd_phys_base = addr;
				bi->initrd_size = size;
				serial_write("initrd_address: addr=");
				serial_hex64(addr);
				serial_write(" size=");
				serial_hex64(size);
				serial_write("\n");
			}
		} else if (ip >= 0) {
			/* Initrd=\path\on\esp. */
			uint32_t p = (uint32_t) ip + 7;
			static uint16_t initrd_path[256];
			uint32_t pi = 0;
			while (p < opts_len && opts[p] != ' ' && opts[p] != 0
			       && pi < 255)
				initrd_path[pi++] = opts[p++];
			initrd_path[pi] = 0;
			if (pi > 0) {
				serial_write("initrd= loading from ESP\n");
				uint8_t *rd_buf = NULL;
				uint64_t rd_size = 0;
				if (load_file_from_esp
				    (bs, initrd_path, &rd_buf, &rd_size) == 0) {
					bi->initrd_phys_base =
					    (uint64_t) (uintptr_t) rd_buf;
					bi->initrd_size = rd_size;
					serial_write("initrd loaded at ");
					serial_hex64(bi->initrd_phys_base);
					serial_write(" size=");
					serial_hex64(bi->initrd_size);
					serial_write("\n");
				}
			}
		}
	}

	/* Try /boot/initrd.cpio. */
	if (bi->initrd_size == 0) {
		static const uint16_t cpio_path[] = {
			'\\', 'b', 'o', 'o', 't', '\\', 'i', 'n', 'i', 't', 'r',
			    'd', '.', 'c', 'p', 'i', 'o', 0
		};
		uint8_t *cpio_buf = NULL;
		uint64_t cpio_size = 0;
		if (load_file_from_esp(bs, cpio_path, &cpio_buf, &cpio_size) ==
		    0) {
			bi->initrd_phys_base = (uint64_t) (uintptr_t) cpio_buf;
			bi->initrd_size = cpio_size;
			serial_write("initrd.cpio loaded at ");
			serial_hex64(bi->initrd_phys_base);
			serial_write(" size=");
			serial_hex64(bi->initrd_size);
			serial_write("\n");
		}
	}

	/* Try /boot/ember.ext2. */
	if (bi->initrd_size == 0) {
		static const uint16_t ext2_path[] = {
			'\\', 'b', 'o', 'o', 't', '\\', 'e', 'm', 'b', 'e',
			    'r', '.', 'e', 'x', 't', '2', 0
		};
		uint8_t *ext2_buf = NULL;
		uint64_t ext2_size = 0;
		if (load_file_from_esp(bs, ext2_path, &ext2_buf, &ext2_size) ==
		    0) {
			bi->initrd_phys_base = (uint64_t) (uintptr_t) ext2_buf;
			bi->initrd_size = ext2_size;
			serial_write("ember.ext2 loaded at ");
			serial_hex64(bi->initrd_phys_base);
			serial_write("\n");
		}
	}

	if (bi->initrd_size == 0) {
		serial_write("no initrd found (ok)\n");
	}
#ifdef FAKE_LARGE_INITRD
	/*
	 * Simulate make_initrd: allocate 1.5 GiB as EfiLoaderData and write
	 * minimal ext2 superblock.  Use a non-page-aligned offset (0x30) to
	 * match what malloc returns under posix-runner.
	 */
	if (bi->initrd_size == 0) {
		uint64_t fake_size = 0x60000000ULL;	/* 1.5 GiB. */
		uint64_t fake_offset = 0x30;	/* Simulate malloc header. */
		void *fake_pages = alloc_pages(bs, fake_size + PAGE_SIZE);
		if (fake_pages) {
			uint8_t *fake_buf =
			    (uint8_t *) fake_pages + fake_offset;
			/* Zero the first 4 KiB to get clean superblock area. */
			for (uint64_t i = 0; i < 4096; i++)
				fake_buf[i] = 0;
			/* Write minimal ext2 superblock at byte 1024. */
			uint8_t *sb = fake_buf + 1024;
			/* s_inodes_count = 192*128 = 24576. */
			*(uint32_t *) (sb + 0) = 24576;
			/* s_blocks_count = 1572864 (0x180000) */
			*(uint32_t *) (sb + 4) = 1572864;
			/* s_free_blocks_count. */
			*(uint32_t *) (sb + 12) = 1500000;
			/* s_free_inodes_count. */
			*(uint32_t *) (sb + 16) = 24000;
			/* s_first_data_block = 1 (for 1024-byte blocks) */
			*(uint32_t *) (sb + 20) = 1;
			/* s_log_block_size = 0 (1024 bytes) */
			*(uint32_t *) (sb + 24) = 0;
			/* s_blocks_per_group = 8192. */
			*(uint32_t *) (sb + 32) = 8192;
			/* s_inodes_per_group = 128. */
			*(uint32_t *) (sb + 40) = 128;
			/* s_magic = 0xEF53 at offset 56. */
			*(uint16_t *) (sb + 56) = 0xEF53;
			/* s_rev_level = 0. */
			*(uint32_t *) (sb + 76) = 0;
			/* s_inode_size = 128 (at offset 88, only valid if rev >= 1) */

			bi->initrd_phys_base = (uint64_t) (uintptr_t) fake_buf;
			bi->initrd_size = fake_size;
			serial_write("FAKE_LARGE_INITRD: addr=");
			serial_hex64(bi->initrd_phys_base);
			serial_write(" size=");
			serial_hex64(bi->initrd_size);
			serial_write("\n");
		} else {
			serial_write("FAKE_LARGE_INITRD: alloc failed\n");
		}
	}
#endif

	bi->rsdp_phys = 0;

	/* Find ACPI RSDP in UEFI configuration table. */
	{
		/* ACPI 2.0 GUID: 8868e871-e4f1-11d3-bc22-0080c73c8881. */
		static const EFI_GUID acpi20_guid =
		    { 0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7,
						   0x3c, 0x88, 0x81} };
		EFI_CONFIGURATION_TABLE *ct = SystemTable->ConfigurationTable;
		for (size_t i = 0; i < SystemTable->NumberOfTableEntries; i++) {
			EFI_GUID *g = &ct[i].VendorGuid;
			if (g->Data1 == acpi20_guid.Data1 &&
			    g->Data2 == acpi20_guid.Data2 &&
			    g->Data3 == acpi20_guid.Data3 &&
			    g->Data4[0] == acpi20_guid.Data4[0] &&
			    g->Data4[1] == acpi20_guid.Data4[1] &&
			    g->Data4[2] == acpi20_guid.Data4[2] &&
			    g->Data4[3] == acpi20_guid.Data4[3] &&
			    g->Data4[4] == acpi20_guid.Data4[4] &&
			    g->Data4[5] == acpi20_guid.Data4[5] &&
			    g->Data4[6] == acpi20_guid.Data4[6] &&
			    g->Data4[7] == acpi20_guid.Data4[7]) {
				bi->rsdp_phys =
				    (uint64_t) (uintptr_t) ct[i].VendorTable;
				serial_write("ACPI 2.0 RSDP found at ");
				serial_hex64(bi->rsdp_phys);
				serial_write("\n");
				break;
			}
		}
	}

	/* Get memory map (copy to persistent buffer) */
	size_t mmap_size = 0;
	size_t map_key = 0;
	size_t desc_size = 0;
	uint32_t desc_ver = 0;

	st = uefi_call5((void *)bs->GetMemoryMap,
			(uint64_t) (uintptr_t) & mmap_size,
			(uint64_t) (uintptr_t) NULL,
			(uint64_t) (uintptr_t) & map_key,
			(uint64_t) (uintptr_t) & desc_size,
			(uint64_t) (uintptr_t) & desc_ver);
	(void)desc_size;
	if (st != EFI_BUFFER_TOO_SMALL) {
		serial_write("GetMemoryMap size failed\n");
		return 1;
	}

	/* Add slack for map growth. */
	mmap_size += desc_size * 8;
	EFI_MEMORY_DESCRIPTOR *mmap =
	    (EFI_MEMORY_DESCRIPTOR *) alloc_pages(bs, mmap_size);
	if (!mmap) {
		serial_write("mmap alloc failed\n");
		return 1;
	}

	/* Pre-allocate page table pool under 1GiB (no allocations after final GetMemoryMap) */
	pt_pool_pages = 16384;	/* 64 MiB -- must cover identity map + HHDM for all RAM. */
	uint64_t max_addr = 0x3fffffffULL;
	pt_pool_base =
	    (uint8_t *) alloc_pages_max(bs, pt_pool_pages, &max_addr);
	if (!pt_pool_base) {
		serial_write("pt pool alloc failed\n");
		return 1;
	}
	bi->pt_pool_phys_base = (uint64_t) (uintptr_t) pt_pool_base;
	bi->pt_pool_bytes = pt_pool_pages * PAGE_SIZE;

	/*
	 * Kernel BSS is already zeroed -- load_kernel_elf allocates full memsz
	 * and zeros it.  No separate BSS allocation needed.
	 */

	uint64_t loop_count = 0;
	uint64_t pml4 = 0;
	while (1) {
		loop_count++;
		size_t cur_size = mmap_size;
		st = uefi_call5((void *)bs->GetMemoryMap,
				(uint64_t) (uintptr_t) & cur_size,
				(uint64_t) (uintptr_t) mmap,
				(uint64_t) (uintptr_t) & map_key,
				(uint64_t) (uintptr_t) & desc_size,
				(uint64_t) (uintptr_t) & desc_ver);
		if (st == EFI_BUFFER_TOO_SMALL) {
			mmap_size = cur_size + desc_size * 8;
			EFI_MEMORY_DESCRIPTOR *new_map =
			    (EFI_MEMORY_DESCRIPTOR *) alloc_pages(bs,
								  mmap_size);
			if (!new_map) {
				serial_write("mmap realloc failed\n");
				return 1;
			}
			mmap = new_map;
			continue;
		}
		if (EFI_ERROR(st)) {
			serial_write("GetMemoryMap failed\n");
			return 1;
		}

		bi->efi_mmap = mmap;
		bi->efi_mmap_size = cur_size;
		bi->efi_desc_size = desc_size;
		bi->efi_desc_version = desc_ver;

		/* Build page tables using pre-allocated pool. */
		pt_pool_used = 0;
		pml4 = alloc_pt_page();
		if (!pml4) {
			serial_write("pml4 alloc failed\n");
			return 1;
		}

		/* Identity-map first 4 GiB using 2MB pages (all naturally aligned) */
		map_range_large(pml4, 0x0, 0x0, 0x100000000ULL,
				PTE_PRESENT | PTE_WRITABLE);

		/*
		 * Map entire kernel (code + data + BSS) at higher-half VMA.
		 * Use 2MB pages where phys_base is 2MB-aligned, else fall back.
		 */
		map_range_large(pml4, kinfo.vma_base, kinfo.phys_base,
				kinfo.total_memsz, PTE_PRESENT | PTE_WRITABLE);

		for (uint64_t off = 0; off < bi->efi_mmap_size;
		     off += bi->efi_desc_size) {
			EFI_MEMORY_DESCRIPTOR *d =
			    (EFI_MEMORY_DESCRIPTOR *) ((uint8_t *) bi->
						       efi_mmap + off);
			uint32_t t = d->Type;
			if (t != EfiConventionalMemory &&
			    t != EfiLoaderCode && t != EfiLoaderData &&
			    t != EfiBootServicesCode && t != EfiBootServicesData
			    && t != EfiACPIReclaimMemory
			    && t != EfiACPIMemoryNVS)
				continue;
			uint64_t base = d->PhysicalStart;
			uint64_t size = d->NumberOfPages * PAGE_SIZE;
			/* Use 2MB pages for HHDM RAM regions; 4KB fallback for unaligned edges. */
			map_range_large(pml4, HHDM_BASE + base, base, size,
					PTE_PRESENT | PTE_WRITABLE);
		}

		if (bi->fb_phys_base && bi->fb_size) {
			map_range(pml4, HHDM_BASE + bi->fb_phys_base,
				  bi->fb_phys_base, bi->fb_size,
				  PTE_PRESENT | PTE_WRITABLE);
		}

		/*
		 * Map LAPIC MMIO page into HHDM at boot so the page table entries
		 * exist from the start.  The TLB shootdown handler needs to access
		 * the LAPIC EOI register after a CR3 reload (full TLB flush).
		 * Without this, the hardware page walker fails on a cold walk to
		 * the LAPIC page (KVM shadow paging issue with uncacheable MMIO).
		 */
		{
			uint32_t lapic_lo, lapic_hi;
			__asm__ __volatile__("rdmsr":"=a"(lapic_lo),
					     "=d"(lapic_hi):"c"(0x1B));
			uint64_t lapic_phys =
			    ((uint64_t) lapic_hi << 32 | lapic_lo) &
			    0xFFFFF000ULL;
			if (lapic_phys) {
				map_range(pml4, HHDM_BASE + lapic_phys,
					  lapic_phys, 0x1000,
					  PTE_PRESENT | PTE_WRITABLE | PTE_PCD |
					  PTE_PWT);
			}
		}

		if (bi->initrd_phys_base && bi->initrd_size) {
			map_range(pml4, HHDM_BASE + bi->initrd_phys_base,
				  bi->initrd_phys_base, bi->initrd_size,
				  PTE_PRESENT | PTE_WRITABLE);
		}

		/* HHDM-map kernel pages (2MB where aligned) */
		map_range_large(pml4, HHDM_BASE + kinfo.phys_base,
				kinfo.phys_base, kinfo.total_memsz,
				PTE_PRESENT | PTE_WRITABLE);

		if (bi->pt_pool_phys_base && bi->pt_pool_bytes) {
			map_range_large(pml4, HHDM_BASE + bi->pt_pool_phys_base,
					bi->pt_pool_phys_base,
					bi->pt_pool_bytes,
					PTE_PRESENT | PTE_WRITABLE);
		}

		/* HHDM-map boot stack so it remains accessible after identity map removal. */
		{
			uint64_t boot_sp;
			__asm__ __volatile__("mov %%rsp, %0":"=r"(boot_sp));
			uint64_t sp_page = boot_sp & ~(PAGE_SIZE - 1);
			uint64_t sp_start =
			    (sp_page >=
			     256 * PAGE_SIZE) ? sp_page - 256 * PAGE_SIZE : 0;
			map_range(pml4, HHDM_BASE + sp_start, sp_start,
				  512 * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
		}

		/* Exit boot services using the map_key from the *current* map. */
		st = uefi_call2((void *)bs->ExitBootServices,
				(uint64_t) (uintptr_t) ImageHandle,
				(uint64_t) map_key);
		if (EFI_ERROR(st)) {
			/* Exit failed; try again (no allocations between map and exit) */
			continue;
		}
		break;
	}

	serial_write("ExitBootServices OK; switching to new page tables\n");
	__asm__ __volatile__("cli");
	write_cr3(pml4);

	/*
	 * Switch RSP/RBP from identity-mapped to HHDM-mapped addresses.
	 * Both map to the same physical page; HHDM survives user PML4 switches.
	 */
	__asm__
	    __volatile__("add %0, %%rsp"::"r"((uint64_t) HHDM_BASE):"memory");
	__asm__
	    __volatile__("add %0, %%rbp"::"r"((uint64_t) HHDM_BASE):"memory");

	void (*kmain_fn) (boot_info_v1_t *) =
	    (void (*)(boot_info_v1_t *))(uintptr_t) kinfo.entry;
	serial_write("jumping to kmain (higher-half)\n");
	kmain_fn(bi);

	for (;;) {
		__asm__ __volatile__("hlt");
	}

	return EFI_SUCCESS;
}
