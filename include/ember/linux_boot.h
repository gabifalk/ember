/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_LINUX_BOOT_H
#define EMBER_LINUX_BOOT_H

#include <stdint.h>

/* E820 entry (20 bytes) */
typedef struct {
	uint64_t addr;
	uint64_t size;
	uint32_t type;		/* 1=RAM, 2=reserved, 3=ACPI, ... */
} __attribute__ ((packed)) e820_entry_t;

/* boot_params field offsets (from Linux Documentation/x86/boot.rst) */
#define BP_E820_ENTRIES_OFF  0x1e8
#define BP_E820_TABLE_OFF    0x2d0
#define BP_CMD_LINE_PTR_OFF  0x228
#define BP_CMDLINE_SIZE_OFF  0x238

/* setup_header fields (offsets relative to boot_params start) */
#define BP_HDR_TYPE_OF_LOADER  0x210
#define BP_HDR_LOADFLAGS       0x211

/* Ramdisk (initrd) fields. */
#define BP_RAMDISK_IMAGE_OFF      0x218
#define BP_RAMDISK_SIZE_OFF       0x21c

/* ext_ramdisk (for >4GB initrd, protocol 2.12+) */
#define BP_EXT_RAMDISK_IMAGE_OFF  0x0c0
#define BP_EXT_RAMDISK_SIZE_OFF   0x0c4

/* ACPI RSDP address (protocol 2.14+, offset 0x070) */
#define BP_ACPI_RSDP_ADDR_OFF     0x070

#define E820_MAX_ENTRIES 128
#define E820_TYPE_RAM       1
#define E820_TYPE_RESERVED  2
#define E820_TYPE_ACPI      3
#define E820_TYPE_NVS       4
#define E820_TYPE_UNUSABLE  5

/* Minimal EFI memory descriptor layout. */
typedef struct {
	uint32_t type;
	uint32_t pad;
	uint64_t phys_start;
	uint64_t virt_start;
	uint64_t num_pages;
	uint64_t attribute;
} efi_mem_desc_t;

#endif
