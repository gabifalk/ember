/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

/* TCC's linker requires _start; alias it to kmain. */
__asm__(".globl _start\n_start = kmain");

#include <stdint.h>

#include "ember/console.h"
#include "ember/pmm.h"
#include "ember/heap.h"
#include "ember/vfs.h"
#include "ember/syscall.h"
#include "ember/paging.h"
#include "ember/user.h"
#include "ember/fd.h"
#include "ember/proc.h"
#include "ember/blkdev.h"
#include "ember/blkcache.h"
#include "ember/ext2.h"
#include "ember/gpt.h"
#include "ember/fat32.h"
#include "ember/ramdisk.h"
#include "ember/sched.h"
#include "ember/acpi.h"
#include "ember/lapic.h"
#include "ember/ioapic.h"
#include "ember/smp.h"
#include "ember/cpu.h"
#include "ember/io.h"

boot_info_v1_t *g_boot_info;

void gdt_init(void);
void idt_init(void);
void tss_init(void);
void pic_init(void);
void timer_init(void);

/* Check if initrd looks like ext2 (magic at byte 1024+56 = 0xEF53) */
static int
initrd_is_ext2(uint64_t phys_base, uint64_t size)
{
	if (size < 2048)
		return 0;
	const uint8_t *p = (const uint8_t *)phys_to_virt(phys_base);
	uint16_t magic = *(const uint16_t *)(p + 1024 + 56);
	return magic == 0xEF53;
}

void
kmain(boot_info_v1_t * bi)
{
	bi = (boot_info_v1_t *) phys_to_virt((uint64_t) bi);
	bi->efi_mmap = (void *)phys_to_virt((uint64_t) bi->efi_mmap);
	g_boot_info = bi;

	{
		uint64_t cr3;
		__asm__ __volatile__("mov %%cr3, %0":"=r"(cr3));
		extern uint64_t kernel_idle_cr3;
		kernel_idle_cr3 = cr3 & 0x000FFFFFFFFFF000ULL;
	}

	console_init(bi);

	{
		uint64_t cr3_val;
		__asm__ __volatile__("mov %%cr3, %0":"=r"(cr3_val));
		uint64_t *pml4 =
		    (uint64_t *) phys_to_virt(cr3_val & 0x000FFFFFFFFFF000ULL);
		for (int i = 0; i < 256; i++)
			pml4[i] = 0;
		/* Flush TLB. */
		__asm__ __volatile__("mov %0, %%cr3"::"r"(cr3_val):"memory");
	}

	pmm_init(bi);
	heap_init();
	acpi_init(bi->rsdp_phys);
	lapic_init();
	ioapic_init();

	uint64_t saved_initrd_phys = bi->initrd_phys_base;
	uint64_t saved_initrd_size = bi->initrd_size;

	int initrd_is_ext2_img = 0;
	if (saved_initrd_phys && saved_initrd_size) {
		if (initrd_is_ext2(saved_initrd_phys, saved_initrd_size)) {
			initrd_is_ext2_img = 1;
			console_write("initrd is ext2 image\n");
		} else {
			vfs_init_from_cpio(saved_initrd_phys,
					   saved_initrd_size);
		}
	}

	vfs_node_t *n = vfs_lookup("/init");
	if (n) {
		console_write("/init found\n");
	} else {
		console_write("/init not found yet (will check disks)\n");
	}

	gdt_init();
	idt_init();
	tss_init();
	syscall_init();
	proc_init();
	sched_init();
	sched_init_idle(0);	/* BSP idle stack for schedule() idle path. */

	smp_init();

	if (cpu_count > 1) {
		/* SMP: disable PIC, use per-CPU LAPIC timer on BSP. */
		outb(0x21, 0xFF);	/* Mask all PIC master IRQs. */
		outb(0xA1, 0xFF);	/* Mask all PIC slave IRQs. */
		lapic_timer_init(100);	/* 100 Hz, same as PIT. */
	} else {
		/* Single CPU: use PIC + PIT. */
		pic_init();
		timer_init();
	}
	fd_init();
	blkdev_init();
	blkcache_init();

	if (initrd_is_ext2_img) {
		int rd = ramdisk_init(saved_initrd_phys, saved_initrd_size);
		if (rd >= 0) {
			if (ext2_init(rd) == 0) {
				console_write("ext2 mounted on ramdisk\n");
				n = vfs_lookup("/init");
				if (n) {
					console_write("/init found on ext2\n");
				} else {
					console_write
					    ("FATAL: ramdisk has no /init\n");
					for (;;)
						__asm__ __volatile__("hlt");
				}
			}
		}
	}

	if (!initrd_is_ext2_img) {
		int disk_base = blkdev_count();
		blkdev_probe_all();

		for (int d = disk_base; d < blkdev_count(); d++) {
			console_write("blkdev: ");
			console_write(blkdev_name(d));
			console_write("\n");

			if (!ext2_is_ready()) {
				uint64_t esp_start = 0, esp_size = 0;
				if (gpt_find_esp(d, &esp_start, &esp_size) == 0) {
					if (fat32_init(d, esp_start) == 0) {
						console_write
						    ("FAT32 ESP mounted\n");
						if (!n) {
							n = vfs_lookup("/init");
							if (n)
								console_write
								    ("/init found on FAT32\n");
						}
					}
				}
			}

			if (!ext2_is_ready()) {
				if (ext2_init(d) == 0) {
					console_write("ext2 mounted\n");
					if (!n) {
						n = vfs_lookup("/init");
						if (n)
							console_write
							    ("/init found on ext2\n");
					}
				}
			}
		}
	}

	user_run_init();

	for (;;) {
		__asm__ __volatile__("hlt");
	}
}
