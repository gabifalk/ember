/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/kexec.h"
#include "ember/linux_boot.h"
#include "ember/elf.h"
#include "ember/vfs.h"
#include "ember/fd.h"
#include "ember/pmm.h"
#include "ember/paging.h"
#include "ember/mmu.h"
#include "ember/console.h"
#include "ember/heap.h"
#include "ember/vectors.h"
#include "boot_info.h"
#include "ember/acpi.h"
#include "ember/lapic.h"
#include "ember/cpu.h"
#include "ember/bkl.h"

/* Provided by kmain.c. */
extern boot_info_v1_t *g_boot_info;

/* Cooperative AP halt for kexec (see include/ember/kexec.h). */
volatile int kexec_halting;
volatile int kexec_ap_halted;

/* Assembly trampoline (kernel/arch/x86_64/kexec_entry.S) */
extern void kexec_jump(uint64_t entry, uint64_t boot_params_phys,
		       uint64_t pml4_phys);

static void
kexec_print_hex(uint64_t v)
{
	char buf[17];
	int i;
	for (i = 0; i < 16; i++)
		buf[i] = '0';
	buf[16] = 0;
	i = 15;
	while (v && i >= 0) {
		uint64_t d = v & 0xf;
		if (d < 10)
			buf[i] = (char)('0' + d);
		else
			buf[i] = (char)('a' + d - 10);
		v = v >> 4;
		i--;
	}
	console_write(buf);
}

static uint32_t
efi_type_to_e820(uint32_t efi_type)
{
	switch (efi_type) {
	case 1:		/* EfiLoaderCode. */
	case 2:		/* EfiLoaderData. */
	case 3:		/* EfiBootServicesCode. */
	case 4:		/* EfiBootServicesData. */
	case 7:		/* EfiConventionalMemory. */
		return E820_TYPE_RAM;
	case 9:		/* EfiACPIReclaimMemory. */
		return E820_TYPE_ACPI;
	case 10:		/* EfiACPIMemoryNVS. */
		return E820_TYPE_NVS;
	case 8:		/* EfiUnusableMemory. */
		return E820_TYPE_UNUSABLE;
	default:		/* Reserved, RuntimeServices*, MMIO, etc. */
		return E820_TYPE_RESERVED;
	}
}

static int
build_e820(uint8_t * bp)
{
	boot_info_v1_t *bi = g_boot_info;
	if (!bi || !bi->efi_mmap || bi->efi_mmap_size == 0)
		return -1;

	uint64_t mmap_bytes = bi->efi_mmap_size;
	uint64_t desc_sz = bi->efi_desc_size;
	uint8_t *mmap = (uint8_t *) bi->efi_mmap;

	e820_entry_t *table = (e820_entry_t *) (bp + BP_E820_TABLE_OFF);
	int count = 0;
	uint64_t off = 0;

	while (off < mmap_bytes && count < E820_MAX_ENTRIES) {
		efi_mem_desc_t *d = (efi_mem_desc_t *) (mmap + off);
		uint64_t addr = d->phys_start;
		uint64_t size = d->num_pages * 4096ULL;
		uint32_t e820_type = efi_type_to_e820(d->type);

		/* Merge with previous entry if contiguous and same type. */
		if (count > 0 && table[count - 1].type == e820_type &&
		    table[count - 1].addr + table[count - 1].size == addr) {
			table[count - 1].size = table[count - 1].size + size;
		} else {
			table[count].addr = addr;
			table[count].size = size;
			table[count].type = e820_type;
			count++;
		}

		off = off + desc_sz;
	}

	/* Write e820 count. */
	bp[BP_E820_ENTRIES_OFF] = (uint8_t) count;
	return 0;
}

static const char *
efi_type_to_iomem_label(uint32_t efi_type)
{
	switch (efi_type) {
	case 1:		/* EfiLoaderCode. */
	case 2:		/* EfiLoaderData. */
	case 3:		/* EfiBootServicesCode. */
	case 4:		/* EfiBootServicesData. */
	case 7:		/* EfiConventionalMemory. */
		return "System RAM";
	case 9:		/* EfiACPIReclaimMemory. */
		return "ACPI Tables";
	case 10:		/* EfiACPIMemoryNVS. */
		return "ACPI Non-volatile Storage";
	default:
		return "reserved";
	}
}

static void
hex16(char *out, uint64_t v)
{
	int i;
	for (i = 15; i >= 0; i--) {
		uint64_t d = v & 0xf;
		if (d < 10)
			out[i] = (char)('0' + d);
		else
			out[i] = (char)('a' + d - 10);
		v >>= 4;
	}
}

char *
kexec_generate_iomem(uint64_t * out_len)
{
	boot_info_v1_t *bi = g_boot_info;
	if (!bi || !bi->efi_mmap || bi->efi_mmap_size == 0)
		return 0;

	uint64_t mmap_bytes = bi->efi_mmap_size;
	uint64_t desc_sz = bi->efi_desc_size;
	uint8_t *mmap = (uint8_t *) bi->efi_mmap;

	/*
	 * Estimate max entries and allocate generously:
	 * each line is at most 2 + 16 + 1 + 16 + 3 + 30 + 1 = ~69 chars.
	 */
	uint64_t max_entries = mmap_bytes / desc_sz;
	uint64_t buf_size = max_entries * 80 + 1;
	char *buf = (char *)kmalloc(buf_size);
	if (!buf)
		return 0;

	uint64_t pos = 0;
	uint64_t off = 0;

	while (off < mmap_bytes) {
		efi_mem_desc_t *d = (efi_mem_desc_t *) (mmap + off);
		uint64_t start = d->phys_start;
		uint64_t size = d->num_pages * 4096ULL;
		if (size == 0) {
			off += desc_sz;
			continue;
		}
		if (pos + 80 >= buf_size)
			break;
		uint64_t end = start + size - 1;
		const char *label = efi_type_to_iomem_label(d->type);

		/* "  " Prefix. */
		buf[pos++] = ' ';
		buf[pos++] = ' ';

		/* 16-Hex start. */
		hex16(buf + pos, start);
		pos += 16;

		buf[pos++] = '-';

		/* 16-Hex end. */
		hex16(buf + pos, end);
		pos += 16;

		/* " : ". */
		buf[pos++] = ' ';
		buf[pos++] = ':';
		buf[pos++] = ' ';

		/* Label. */
		const char *lp = label;
		while (*lp)
			buf[pos++] = *lp++;

		buf[pos++] = '\n';

		off += desc_sz;
	}

	buf[pos] = '\0';
	if (out_len)
		*out_len = pos;
	return buf;
}

/* ---- Two-phase kexec state ---- */

#define KEXEC_MAX_SEGMENTS 16

typedef struct {
	uint64_t dest_paddr;	/* Destination physical address. */
	uint8_t *data;		/* Kmalloc'd source data. */
	uint64_t data_size;	/* Bytes of file data to copy. */
	uint64_t mem_size;	/* Total bytes at dest (zero-fill remainder) */
} kexec_segment_t;

static struct {
	uint64_t entry;
	uint64_t boot_params_phys;
	uint64_t pml4_phys;

	/* Deferred kernel segments. */
	kexec_segment_t segments[KEXEC_MAX_SEGMENTS];
	int seg_count;

	/* Deferred initrd. */
	uint8_t *initrd_data;	/* Kmalloc'd initrd content. */
	uint64_t initrd_size;
	uint64_t initrd_dest_phys;	/* Where to copy it. */

	/* ACPI RSDP physical address (to copy to legacy scan region) */
	uint64_t rsdp_phys;

	/* Command line physical address (may be above 4GB). */
	uint64_t cmdline_phys;

	int loaded;
} kexec_image;

int
kexec_is_loaded(void)
{
	return kexec_image.loaded;
}

/*
 * Fixed low-memory addresses for boot_params and cmdline.
 * Linux's startup_64 decompressor only identity-maps the first 4GB,
 * so these structures must be below 4GB.  After hours of building,
 * all sub-4GB PMM pages may be exhausted, so we can't allocate low
 * pages.  Instead, we relocate to fixed addresses in the conventional
 * memory area (below 640K) at execute time — this memory is about to
 * be overwritten by Linux anyway.
 */
#define KEXEC_BP_LOW_PHYS   0x7000ULL	/* boot_params (4K page) */
#define KEXEC_CL_LOW_PHYS   0x8000ULL	/* cmdline (4K page) */

int
kexec_execute(void)
{
	if (!kexec_image.loaded)
		return -1;

	/*
	 * Cooperatively halt all APs.  We set kexec_halting and send a
	 * kick IPI to wake APs from HLT.  Each AP sees the flag in its
	 * idle loop, increments kexec_ap_halted, and does CLI; HLT
	 * forever.  We wait for all APs to confirm before proceeding.
	 *
	 * This is better than sending INIT because INIT triggers the
	 * UEFI firmware's AP reset path, which may crash if Ember
	 * overwrote firmware data structures.  With cooperative halt,
	 * APs stay in place with interrupts disabled — Linux's own
	 * INIT+SIPI during SMP bringup handles the reset cleanly.
	 */
	if (cpu_count > 1 && lapic_enabled) {
		int expected = cpu_count - 1;
		console_write("kexec: halting ");
		kexec_print_hex((uint64_t) expected);
		console_write(" APs\n");

		kexec_halting = 1;
		__asm__ __volatile__("":::"memory");	/* Compiler barrier. */
		lapic_send_ipi_all_excl_self(VEC_SCHED_KICK);	/* Kick to wake from HLT. */

		/* Wait for all APs. */
		volatile int timeout = 0;
		while (kexec_ap_halted < expected && timeout < 100000000)
			timeout++;

		console_write("kexec: ");
		kexec_print_hex((uint64_t) kexec_ap_halted);
		console_write(" APs halted\n");
	}

	/* Disable interrupts on BSP for the rest of kexec. */
	__asm__ __volatile__("cli");

	console_write("kexec: copying segments to final addresses\n");

	/* Copy kernel segments to their physical destinations. */
	int i;
	for (i = 0; i < kexec_image.seg_count; i++) {
		kexec_segment_t *seg = &kexec_image.segments[i];
		uint8_t *dest = (uint8_t *) phys_to_virt(seg->dest_paddr);
		uint64_t j;
		/* Zero the full region. */
		for (j = 0; j < seg->mem_size; j++)
			dest[j] = 0;
		/* Copy file data. */
		for (j = 0; j < seg->data_size; j++)
			dest[j] = seg->data[j];
	}

	/* Copy initrd to its physical destination. */
	if (kexec_image.initrd_data && kexec_image.initrd_size > 0) {
		uint8_t *dest =
		    (uint8_t *) phys_to_virt(kexec_image.initrd_dest_phys);
		uint64_t j;
		for (j = 0; j < kexec_image.initrd_size; j++)
			dest[j] = kexec_image.initrd_data[j];
	}

	/*
	 * Relocate boot_params to a fixed low address.  Linux's compressed
	 * kernel decompressor builds 4GB identity-mapped page tables, so
	 * boot_params must be below 4GB.  We copy to conventional memory
	 * (below 640K) which is always identity-mapped.
	 */
	uint8_t *bp_src = (uint8_t *) phys_to_virt(kexec_image.boot_params_phys);
	uint8_t *bp_dst = (uint8_t *) phys_to_virt(KEXEC_BP_LOW_PHYS);
	{
		uint64_t j;
		for (j = 0; j < PAGE_SIZE; j++)
			bp_dst[j] = bp_src[j];
	}

	/* Relocate cmdline to low memory if present. */
	if (kexec_image.cmdline_phys) {
		uint8_t *cl_src = (uint8_t *) phys_to_virt(kexec_image.cmdline_phys);
		uint8_t *cl_dst = (uint8_t *) phys_to_virt(KEXEC_CL_LOW_PHYS);
		uint64_t j;
		for (j = 0; j < PAGE_SIZE; j++)
			cl_dst[j] = cl_src[j];
		uint32_t *cmd_ptr = (uint32_t *) (bp_dst + BP_CMD_LINE_PTR_OFF);
		*cmd_ptr = (uint32_t) KEXEC_CL_LOW_PHYS;
	}

	uint64_t final_bp = KEXEC_BP_LOW_PHYS;

	console_write("kexec: executing, entry=0x");
	kexec_print_hex(kexec_image.entry);
	console_write(" boot_params=0x");
	kexec_print_hex(final_bp);
	console_write("\n");
	serial_flush();
	kexec_jump(kexec_image.entry, final_bp, kexec_image.pml4_phys);
	return -1;
}

static uint8_t *
read_fd_contents(int fd_num, uint64_t * out_size)
{
	fd_entry_t *entry = fd_get(fd_num);
	if (!entry || !entry->desc)
		return 0;
	file_desc_t *desc = entry->desc;
	if (!desc->node || desc->type != FD_TYPE_FILE)
		return 0;
	vfs_node_t *node = desc->node;
	uint64_t size = node->size;
	if (size == 0)
		return 0;
	uint8_t *buf = (uint8_t *) kmalloc(size);
	if (!buf)
		return 0;
	int64_t nread = vfs_read(node, 0, buf, size);
	if (nread < 0 || (uint64_t) nread < size) {
		kfree(buf);
		return 0;
	}
	*out_size = size;
	return buf;
}

int
kexec_load_from_fds(int kernel_fd, int initrd_fd,
		    const char *cmdline, uint64_t cmdline_len)
{
	console_write("kexec: loading from fds (kernel_fd=");
	kexec_print_hex((uint64_t) kernel_fd);
	console_write(")\n");

	/* Issue 3: Free old pages if reloading. */
	if (kexec_image.loaded) {
		pmm_free_page(kexec_image.boot_params_phys);
		pmm_free_page(kexec_image.pml4_phys);
		/* Free deferred segment buffers. */
		int si;
		for (si = 0; si < kexec_image.seg_count; si++) {
			if (kexec_image.segments[si].data)
				kfree(kexec_image.segments[si].data);
		}
		/* Free deferred initrd buffer. */
		if (kexec_image.initrd_data)
			kfree(kexec_image.initrd_data);
		kexec_image.loaded = 0;
	}

	/* 1. Read kernel ELF from fd. */
	uint64_t kernel_size = 0;
	uint8_t *kernel_buf = read_fd_contents(kernel_fd, &kernel_size);
	if (!kernel_buf) {
		console_write("kexec: failed to read kernel fd\n");
		return -1;
	}

	/* 2. Detect format and load kernel. */
	uint64_t entry = 0;
	uint64_t seg_base[16], seg_end[16];
	int seg_count = 0;
	int is_bzimage = 0;
	uint64_t setup_size = 0;

	if (kernel_size >= sizeof(elf64_ehdr_t) &&
	    kernel_buf[0] == 0x7f && kernel_buf[1] == 'E' &&
	    kernel_buf[2] == 'L' && kernel_buf[3] == 'F') {
		/* ---- Vmlinux (ELF) path ---- */
		elf64_ehdr_t *ehdr = (elf64_ehdr_t *) kernel_buf;

		if (ehdr->e_ident[4] != 2) {
			console_write("kexec: not ELF64\n");
			kfree(kernel_buf);
			return -1;
		}
		if (ehdr->e_machine != 0x3e) {
			console_write("kexec: not x86_64\n");
			kfree(kernel_buf);
			return -1;
		}
		if (ehdr->e_type != 2) {
			console_write("kexec: not ET_EXEC\n");
			kfree(kernel_buf);
			return -1;
		}

		entry = ehdr->e_entry;
		uint16_t phnum = ehdr->e_phnum;
		uint16_t phentsize = ehdr->e_phentsize;

		/* Issue 9: validate phentsize. */
		if (phentsize < sizeof(elf64_phdr_t)) {
			console_write("kexec: invalid phentsize\n");
			kfree(kernel_buf);
			return -1;
		}

		/* 3. Store PT_LOAD segments for deferred copy during kexec_execute. */
		uint16_t i;
		for (i = 0; i < phnum; i++) {
			uint64_t ph_off =
			    ehdr->e_phoff + (uint64_t) i * phentsize;
			if (ph_off + sizeof(elf64_phdr_t) > kernel_size)
				break;
			elf64_phdr_t *ph =
			    (elf64_phdr_t *) (kernel_buf + ph_off);
			if (ph->p_type != PT_LOAD)
				continue;

			uint64_t paddr = ph->p_paddr;
			uint64_t filesz = ph->p_filesz;
			uint64_t memsz = ph->p_memsz;
			uint64_t file_off = ph->p_offset;

			/* Issue 7: clamp malformed ELF where filesz > memsz. */
			if (filesz > memsz)
				filesz = memsz;

			if (seg_count < KEXEC_MAX_SEGMENTS) {
				seg_base[seg_count] = paddr;
				seg_end[seg_count] = paddr + memsz;

				/* Store segment data for deferred copy. */
				kexec_image.segments[seg_count].dest_paddr =
				    paddr;
				kexec_image.segments[seg_count].mem_size =
				    memsz;
				if (filesz > 0
				    && file_off + filesz <= kernel_size) {
					uint8_t *seg_data =
					    (uint8_t *) kmalloc(filesz);
					if (!seg_data) {
						console_write
						    ("kexec: alloc segment failed\n");
						kfree(kernel_buf);
						return -1;
					}
					uint64_t j;
					for (j = 0; j < filesz; j++)
						seg_data[j] =
						    kernel_buf[file_off + j];
					kexec_image.segments[seg_count].data =
					    seg_data;
					kexec_image.segments[seg_count].
					    data_size = filesz;
				} else {
					kexec_image.segments[seg_count].data =
					    0;
					kexec_image.segments[seg_count].
					    data_size = 0;
				}
				seg_count++;
			}

			console_write("kexec: PT_LOAD paddr=0x");
			kexec_print_hex(paddr);
			console_write(" memsz=0x");
			kexec_print_hex(memsz);
			console_write("\n");
		}

		kfree(kernel_buf);

	} else if (kernel_size >= 0x290 &&
		   *(uint16_t *) (kernel_buf + 0x1FE) == 0xAA55 &&
		   *(uint32_t *) (kernel_buf + 0x202) == 0x53726448) {
		/* ---- BzImage path ---- */
		is_bzimage = 1;

		/* Parse setup header. */
		uint8_t setup_sects = kernel_buf[0x1F1];
		if (setup_sects == 0)
			setup_sects = 4;	/* Default per protocol. */
		setup_size = ((uint64_t) setup_sects + 1) * 512;

		/* Protocol version at 0x206 (2 bytes, little-endian) */
		uint16_t protocol = *(uint16_t *) (kernel_buf + 0x206);
		if (protocol < 0x0206) {
			console_write
			    ("kexec: bzImage protocol too old (need >= 2.06)\n");
			kfree(kernel_buf);
			return -1;
		}

		/* Xloadflags at 0x236 -- need XLF_KERNEL_64 (bit 0) */
		uint16_t xloadflags = *(uint16_t *) (kernel_buf + 0x236);
		if (!(xloadflags & 1)) {
			console_write
			    ("kexec: bzImage lacks 64-bit entry support\n");
			kfree(kernel_buf);
			return -1;
		}

		/* pref_address at 0x258 (8 bytes) -- where to load protected-mode kernel. */
		uint64_t pref_address = *(uint64_t *) (kernel_buf + 0x258);

		/* Protected-mode kernel starts after setup sectors. */
		if (setup_size >= kernel_size) {
			console_write
			    ("kexec: bzImage setup_size >= file size\n");
			kfree(kernel_buf);
			return -1;
		}
		uint64_t pm_offset = setup_size;
		uint64_t pm_size = kernel_size - setup_size;

		console_write("kexec: bzImage protocol=0x");
		kexec_print_hex(protocol);
		console_write(" pref_addr=0x");
		kexec_print_hex(pref_address);
		console_write(" pm_size=0x");
		kexec_print_hex(pm_size);
		console_write("\n");

		/* Store protected-mode kernel for deferred copy. */
		{
			uint8_t *pm_data = (uint8_t *) kmalloc(pm_size);
			if (!pm_data) {
				console_write
				    ("kexec: alloc bzImage segment failed\n");
				kfree(kernel_buf);
				return -1;
			}
			uint64_t j;
			for (j = 0; j < pm_size; j++)
				pm_data[j] = kernel_buf[pm_offset + j];
			kexec_image.segments[0].dest_paddr = pref_address;
			kexec_image.segments[0].data = pm_data;
			kexec_image.segments[0].data_size = pm_size;
			kexec_image.segments[0].mem_size = pm_size;
		}

		seg_base[0] = pref_address;
		seg_end[0] = pref_address + pm_size;
		seg_count = 1;

		/* Entry point: startup_64 is at +0x200 from start of protected-mode code. */
		entry = pref_address + 0x200;

		/* DON'T free kernel_buf yet -- need it for setup header copy. */

	} else {
		console_write
		    ("kexec: unknown kernel format (not ELF or bzImage)\n");
		kfree(kernel_buf);
		return -1;
	}

	/* 4. Handle initrd if provided. */
	uint64_t initrd_phys = 0;
	uint64_t initrd_size = 0;

	if (initrd_fd >= 0) {
		uint8_t *initrd_buf = read_fd_contents(initrd_fd, &initrd_size);
		if (!initrd_buf) {
			console_write("kexec: failed to read initrd fd\n");
			if (is_bzimage)
				kfree(kernel_buf);
			return -1;
		}

		/* Find largest RAM block under 4GB from EFI mmap. */
		boot_info_v1_t *bi = g_boot_info;
		uint64_t best_base = 0, best_size = 0;

		if (bi && bi->efi_mmap && bi->efi_mmap_size > 0) {
			uint64_t mmap_bytes = bi->efi_mmap_size;
			uint64_t desc_sz = bi->efi_desc_size;
			uint8_t *mmap = (uint8_t *) bi->efi_mmap;
			uint64_t off = 0;

			while (off < mmap_bytes) {
				efi_mem_desc_t *d =
				    (efi_mem_desc_t *) (mmap + off);
				uint32_t e820_type = efi_type_to_e820(d->type);
				if (e820_type == E820_TYPE_RAM) {
					uint64_t rbase = d->phys_start;
					uint64_t rsize = d->num_pages * 4096ULL;
					uint64_t rend = rbase + rsize;
					/* Clamp to 4GB. */
					if (rend > 0x100000000ULL)
						rend = 0x100000000ULL;
					if (rend > rbase) {
						uint64_t clamped_size =
						    rend - rbase;
						if (clamped_size > best_size) {
							best_base = rbase;
							best_size =
							    clamped_size;
						}
					}
				}
				off += desc_sz;
			}
		}

		if (best_size < initrd_size) {
			console_write
			    ("kexec: no suitable RAM region for initrd\n");
			kfree(initrd_buf);
			if (is_bzimage)
				kfree(kernel_buf);
			return -1;
		}

		/* Place initrd at top of best region, page-aligned. */
		initrd_phys =
		    (best_base + best_size -
		     initrd_size) & ~(uint64_t) (PAGE_SIZE - 1);

		/* Issue 2: verify initrd doesn't overlap any loaded kernel segment. */
		{
			int si;
			for (si = 0; si < seg_count; si++) {
				if (initrd_phys < seg_end[si] &&
				    initrd_phys + initrd_size > seg_base[si]) {
					console_write
					    ("kexec: initrd overlaps kernel segment\n");
					kfree(initrd_buf);
					if (is_bzimage)
						kfree(kernel_buf);
					return -1;
				}
			}
		}

		console_write("kexec: initrd at phys=0x");
		kexec_print_hex(initrd_phys);
		console_write(" size=0x");
		kexec_print_hex(initrd_size);
		console_write("\n");

		/* Store initrd for deferred copy (don't copy to physical yet) */
		kexec_image.initrd_data = initrd_buf;
		kexec_image.initrd_size = initrd_size;
		kexec_image.initrd_dest_phys = initrd_phys;
		/* Don't kfree initrd_buf -- ownership transferred to kexec_image. */
	}

	/* 5. Build boot_params (zero page). */
	uint64_t bp_phys = pmm_alloc_page();
	if (bp_phys == UINT64_MAX) {
		console_write("kexec: alloc boot_params failed\n");
		if (is_bzimage)
			kfree(kernel_buf);
		return -1;
	}
	uint8_t *bp = (uint8_t *) phys_to_virt(bp_phys);
	{
		uint64_t k;
		for (k = 0; k < PAGE_SIZE; k++)
			bp[k] = 0;
	}

	/* For bzImage: copy setup header into boot_params before we override fields. */
	if (is_bzimage) {
		uint64_t hdr_copy_end = setup_size;
		if (hdr_copy_end > 0x290)
			hdr_copy_end = 0x290;
		if (hdr_copy_end > 0x1F1) {
			uint64_t ci;
			for (ci = 0x1F1; ci < hdr_copy_end; ci++)
				bp[ci] = kernel_buf[ci];
		}
		kfree(kernel_buf);
	}

	/* type_of_loader = 0xFF (undefined) */
	bp[BP_HDR_TYPE_OF_LOADER] = 0xFF;
	/* Loadflags |= LOADED_HIGH (bit 0) */
	bp[BP_HDR_LOADFLAGS] = bp[BP_HDR_LOADFLAGS] | 1;

	/*
	 * Build e820 table from EFI memory map -- fatal: without e820, identity
	 * page tables will be empty and the jump will fault.
	 */
	if (build_e820(bp) < 0) {
		console_write("kexec: failed to build e820\n");
		pmm_free_page(bp_phys);
		return -1;
	}

	/* ACPI RSDP passthrough (boot_params field for kernels >= 5.2) */
	if (g_boot_info && g_boot_info->rsdp_phys) {
		uint64_t *rsdp_ptr = (uint64_t *) (bp + BP_ACPI_RSDP_ADDR_OFF);
		*rsdp_ptr = g_boot_info->rsdp_phys;
		kexec_image.rsdp_phys = g_boot_info->rsdp_phys;
	}

	/* Initrd fields. */
	if (initrd_size > 0) {
		uint32_t *rd_image = (uint32_t *) (bp + BP_RAMDISK_IMAGE_OFF);
		uint32_t *rd_size = (uint32_t *) (bp + BP_RAMDISK_SIZE_OFF);
		*rd_image = (uint32_t) initrd_phys;
		*rd_size = (uint32_t) initrd_size;
		/* Ext fields = 0 (already zeroed) */
	}

	/* 6. Copy command line, appending acpi_rsdp= for older kernels. */
	/* Issue 8: declare cl_phys in outer scope for identity mapping later. */
	uint64_t cl_phys = 0;
	if (cmdline && cmdline_len > 0) {
		cl_phys = pmm_alloc_page();
		if (cl_phys != UINT64_MAX) {
			uint8_t *cl = (uint8_t *) phys_to_virt(cl_phys);
			uint64_t ci;
			for (ci = 0; ci < PAGE_SIZE; ci++)
				cl[ci] = 0;

			uint64_t clen = cmdline_len;
			if (clen > 4095)
				clen = 4095;
			for (ci = 0; ci < clen; ci++)
				cl[ci] = (uint8_t) cmdline[ci];

			/* Find actual string end (cmdline_len may include null) */
			while (clen > 0 && cl[clen - 1] == 0)
				clen--;

			/* Append acpi_rsdp=0x<hex> so kernels < 5.2 can find RSDP. */
			if (g_boot_info && g_boot_info->rsdp_phys
			    && clen + 30 < 4095) {
				const char *prefix = " acpi_rsdp=0x";
				uint64_t pi;
				for (pi = 0; prefix[pi]; pi++)
					cl[clen++] = (uint8_t) prefix[pi];
				/* Write 16-digit hex of rsdp_phys. */
				uint64_t rval = g_boot_info->rsdp_phys;
				int hi;
				for (hi = 15; hi >= 0; hi--) {
					uint64_t d = (rval >> (hi * 4)) & 0xf;
					cl[clen++] =
					    (uint8_t) (d <
						       10 ? '0' + d : 'a' + d -
						       10);
				}
			}
			cl[clen] = 0;

			uint32_t *cmd_ptr =
			    (uint32_t *) (bp + BP_CMD_LINE_PTR_OFF);
			*cmd_ptr = (uint32_t) cl_phys;
			uint32_t *cmd_sz =
			    (uint32_t *) (bp + BP_CMDLINE_SIZE_OFF);
			*cmd_sz = (uint32_t) clen;
		} else {
			cl_phys = 0;
		}
	}

	/* 7. Build identity-mapped page tables. */
	uint64_t id_pml4_phys = pmm_alloc_page();
	if (id_pml4_phys == UINT64_MAX) {
		console_write("kexec: alloc PML4 failed\n");
		pmm_free_page(bp_phys);	/* Issue 4: don't leak boot_params page. */
		return -1;
	}
	uint64_t *id_pml4 = (uint64_t *) phys_to_virt(id_pml4_phys);
	{
		uint64_t z;
		for (z = 0; z < 512; z++)
			id_pml4[z] = 0;
	}

	uint64_t cur_cr3 = read_cr3();
	uint64_t *cur_pml4 = (uint64_t *) phys_to_virt(cur_cr3);
	id_pml4[256] = cur_pml4[256];	/* HHDM. */
	id_pml4[511] = cur_pml4[511];	/* Kernel text. */

	/* Identity-map all e820 RAM regions. */
	{
		e820_entry_t *table = (e820_entry_t *) (bp + BP_E820_TABLE_OFF);
		int count = (int)bp[BP_E820_ENTRIES_OFF];
		int ei;
		for (ei = 0; ei < count; ei++) {
			if (table[ei].type != E820_TYPE_RAM)
				continue;
			uint64_t base = table[ei].addr;
			uint64_t size = table[ei].size;
			paging_map_range(id_pml4_phys, base, base, size,
					 PTE_PRESENT | PTE_WRITABLE);
		}
	}

	/* Also identity-map boot_params page. */
	paging_map_range(id_pml4_phys, bp_phys, bp_phys, PAGE_SIZE,
			 PTE_PRESENT | PTE_WRITABLE);

	/* Issue 8: identity-map cmdline page if one was allocated. */
	if (cmdline && cmdline_len > 0 && cl_phys != 0) {
		paging_map_range(id_pml4_phys, cl_phys, cl_phys, PAGE_SIZE,
				 PTE_PRESENT | PTE_WRITABLE);
	}

	/* 8. Store in kexec_image (do NOT jump) */
	kexec_image.entry = entry;
	kexec_image.boot_params_phys = bp_phys;
	kexec_image.cmdline_phys = cl_phys;
	kexec_image.pml4_phys = id_pml4_phys;
	kexec_image.seg_count = seg_count;
	kexec_image.loaded = 1;

	console_write("kexec: image loaded, entry=0x");
	kexec_print_hex(entry);
	console_write(" boot_params=0x");
	kexec_print_hex(bp_phys);
	console_write("\n");

	return 0;
}
