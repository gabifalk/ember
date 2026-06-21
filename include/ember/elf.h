/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_ELF_H
#define EMBER_ELF_H

#include <stdint.h>

typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} elf64_phdr_t;

#define ET_EXEC 2
#define ET_DYN  3

#define PT_LOAD 1
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ELF_MAX_SEGS 8

typedef struct {
	uint64_t vaddr;		/* page-aligned segment start */
	uint64_t len;		/* page-aligned segment length */
	uint8_t prot;		/* PROT_* bits for the region's VMA */
} elf_seg_t;

typedef struct {
	uint64_t entry;		/* e_entry. */
	uint64_t phdr_vaddr;	/* VA of PHDR table (within first PT_LOAD) */
	uint16_t phentsize;	/* e_phentsize (56) */
	uint16_t phnum;		/* e_phnum. */
	uint64_t brk_base;	/* Page-aligned end of last PT_LOAD memsz. */
	elf_seg_t segs[ELF_MAX_SEGS];	/* PT_LOAD regions, for VMA creation. */
	int nsegs;
} elf_info_t;

#endif
