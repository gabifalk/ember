/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_BOOT_INFO_H
#define EMBER_BOOT_INFO_H

#include <stdint.h>

#define BOOT_INFO_VERSION 2u

typedef struct boot_info_v1 {
	uint32_t boot_info_version;

	void *efi_mmap;
	uint64_t efi_mmap_size;
	uint64_t efi_desc_size;
	uint32_t efi_desc_version;

	uint64_t kernel_phys_base;
	uint64_t kernel_phys_end;

	uint64_t initrd_phys_base;
	uint64_t initrd_size;

	uint64_t fb_phys_base;
	uint64_t fb_size;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t fb_stride_pixels;
	uint32_t fb_bpp;
	uint32_t fb_format;

	uint64_t pmm_bitmap_phys_base;
	uint64_t pmm_bitmap_bytes;

	/* Page table pool (allocated by loader) */
	uint64_t pt_pool_phys_base;
	uint64_t pt_pool_bytes;

	/* ACPI RSDP physical address (0 if not found) */
	uint64_t rsdp_phys;
} boot_info_v1_t;

#endif
