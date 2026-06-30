/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

#include <stdint.h>
#include "ember/mmu.h"
#include "ember/smp.h"

/*
 * AP trampoline: hand-assembled 16-bit -> 32-bit -> 64-bit transition code.
 * Loaded at physical address 0x8000 (SIPI vector 0x08).
 *
 * The trampoline data block lives at 0x8F00 (offset 0xF00 from base).
 */

#define TRAMPOLINE_DATA_OFFSET 0xF00

/*
 * Hand-assembled trampoline blob.
 *
 * 16-bit real mode (starts at 0x8000, CS:IP = 0x800:0x0000):
 * 0x00: cli
 * 0x01: xor ax, ax
 * 0x03: mov ds, ax
 * 0x05: lgdt [0x8F40]        ; load temp GDT from data area
 * 0x0B: mov eax, cr0
 * 0x0E: or al, 1
 * 0x10: mov cr0, eax         ; enable PE
 * 0x13: far jmp 0x08:0x8018  ; jump to 32-bit segment (GDT entry 1 = code32)
 *
 * 32-bit protected mode (at 0x8018):
 * 0x18: mov ax, 0x10         ; data segment (not strictly needed but safe)
 * 0x1C: mov ds, ax
 * 0x1E: mov es, ax
 * 0x20: mov ss, ax
 * 0x22: mov eax, cr4
 * 0x25: or eax, 0xA0         ; PAE (bit 5) + PGE (bit 7)
 * 0x2A: mov cr4, eax
 * 0x2D: mov eax, [0x8F00]    ; load CR3 from data block
 * 0x33: mov cr3, eax
 * 0x36: mov ecx, 0xC0000080  ; IA32_EFER
 * 0x3B: rdmsr
 * 0x3D: or eax, 0x100        ; LME (bit 8)
 * 0x42: wrmsr
 * 0x44: mov eax, cr0
 * 0x47: or eax, 0x80000000   ; PG (bit 31)
 * 0x4C: mov cr0, eax
 * 0x4F: far jmp 0x10:0x8058  ; jump to 64-bit segment (GDT entry 2 = code64)
 *
 * 64-bit long mode (at 0x8058):
 * 0x58: load stack from [0x8F10]
 * 0x63: load entry point from [0x8F08]
 * 0x6E: load cpu_local_ptr from [0x8F18] into RDI (first arg)
 * 0x79: jmp to entry point.
 */
static const uint8_t trampoline_blob[] = {
	/* === 16-Bit real mode === */
				/* 0X00. */ 0xFA,
				/* Cli. */
					/* 0X01. */ 0x66, 0x31, 0xC0,
					/* Xor eax, eax (clear for segment) */
				/* 0X04. */ 0x8E, 0xD8,
				/* Mov ds, ax. */
							/* 0X06. */ 0x0F, 0x01, 0x16, 0x40, 0x8F,
							/* Lgdt [0x8F40]. */
					/* 0X0B. */ 0x0F, 0x20, 0xC0,
					/* Mov eax, cr0. */
				/* 0X0E. */ 0x0C, 0x01,
				/* Or al, 1. */
					/* 0X10. */ 0x0F, 0x22, 0xC0,
					/* Mov cr0, eax. */
				/* 0X13. */ 0x66, 0xEA,
				/* Far jmp (32-bit offset follows) */
						/* 0X15. */ 0x18, 0x80, 0x00, 0x00,
						/* Offset = 0x00008018. */
				/* 0X19. */ 0x08, 0x00,
				/* Selector = 0x0008 (code32) */

	/* Pad to 0x1B -- we're at 0x1B, need to reach 0x1B. */
	/* Actually we're at offset 0x1B. Let's recalculate. */
				/* 0X1B. */ 0x90,
				/* Nop (padding to align) */
				/* 0X1C. */ 0x90,
				/* Nop. */
				/* 0X1D. */ 0x90,
				/* Nop. */

	/* === 32-Bit protected mode (offset 0x1E in blob = phys 0x801E) === */
	/* Wait -- let me recalculate offsets carefully. */
};

/*
 * Actually, let's build the trampoline more carefully with exact byte counts.
 * Since the jmp offsets depend on the exact physical addresses, we need to be
 * very precise. Let me redo this with a flat blob.
 */

/* Total blob size -- we have room up to 0xF00 (3840 bytes) */
static uint8_t ap_trampoline_code[256];

void
trampoline_build(void)
{
	uint8_t *p = ap_trampoline_code;
	int i = 0;

	/* === 16-Bit real mode (runs at phys 0x8000) === */

	/* Cli. */
	p[i++] = 0xFA;

	/* Xor ax, ax. */
	p[i++] = 0x31;
	p[i++] = 0xC0;

	/* Mov ds, ax. */
	p[i++] = 0x8E;
	p[i++] = 0xD8;

	/*
	 * Lgdt [0x8F48] -- operand size override + lgdt m16&32
	 * tmp_gdtr is at data_base + 0x48 = 0x8F48.
	 */
	p[i++] = 0x66;		/* Operand size override (load 32-bit base) */
	p[i++] = 0x0F;
	p[i++] = 0x01;
	p[i++] = 0x16;		/* Mod=00 r/m=110 (disp16) */
	p[i++] = 0x48;
	p[i++] = 0x8F;		/* Disp16 = 0x8F48. */

	/* Mov eax, cr0 -- needs 0x66 prefix in 16-bit mode for 32-bit operand. */
	p[i++] = 0x66;
	p[i++] = 0x0F;
	p[i++] = 0x20;
	p[i++] = 0xC0;

	/* Or al, 1 (set PE) */
	p[i++] = 0x0C;
	p[i++] = 0x01;

	/* Mov cr0, eax. */
	p[i++] = 0x66;
	p[i++] = 0x0F;
	p[i++] = 0x22;
	p[i++] = 0xC0;

	/*
	 * Far jmp 0x08:pm32_entry
	 * In 16-bit mode with 0x66 prefix, this is a 32-bit far jump.
	 * Encoding: 0x66 0xEA <32-bit offset> <16-bit selector>.
	 */
	int pm32_patch = i + 2;	/* Where to write the 32-bit offset. */
	p[i++] = 0x66;
	p[i++] = 0xEA;
	p[i++] = 0;
	p[i++] = 0;
	p[i++] = 0;
	p[i++] = 0;		/* Offset (patched) */
	p[i++] = 0x08;
	p[i++] = 0x00;		/* Selector = 0x0008. */

	/* === 32-Bit protected mode === */
	int pm32_start = i;
	/* Patch the far jump target. */
	uint32_t pm32_phys = 0x8000 + (uint32_t) pm32_start;
	ap_trampoline_code[pm32_patch + 0] = (uint8_t) (pm32_phys);
	ap_trampoline_code[pm32_patch + 1] = (uint8_t) (pm32_phys >> 8);
	ap_trampoline_code[pm32_patch + 2] = (uint8_t) (pm32_phys >> 16);
	ap_trampoline_code[pm32_patch + 3] = (uint8_t) (pm32_phys >> 24);

	/* Mov ax, 0x10; mov ds, ax; mov es, ax; mov ss, ax. */
	p[i++] = 0x66;
	p[i++] = 0xB8;
	p[i++] = 0x10;
	p[i++] = 0x00;		/* Mov ax, 0x10. */
	p[i++] = 0x8E;
	p[i++] = 0xD8;		/* Mov ds, ax. */
	p[i++] = 0x8E;
	p[i++] = 0xC0;		/* Mov es, ax. */
	p[i++] = 0x8E;
	p[i++] = 0xD0;		/* Mov ss, ax. */

	/* Mov eax, cr4. */
	p[i++] = 0x0F;
	p[i++] = 0x20;
	p[i++] = 0xE0;
	/* Or eax, 0xA0 (PAE=bit5 + PGE=bit7) */
	p[i++] = 0x0D;
	p[i++] = 0xA0;
	p[i++] = 0x00;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Mov cr4, eax. */
	p[i++] = 0x0F;
	p[i++] = 0x22;
	p[i++] = 0xE0;

	/* Mov eax, [0x8F00] -- load CR3 from data block (low 32 bits) */
	p[i++] = 0xA1;
	p[i++] = 0x00;
	p[i++] = 0x8F;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Mov cr3, eax. */
	p[i++] = 0x0F;
	p[i++] = 0x22;
	p[i++] = 0xD8;

	/* Mov ecx, 0xC0000080 (IA32_EFER) */
	p[i++] = 0xB9;
	p[i++] = 0x80;
	p[i++] = 0x00;
	p[i++] = 0x00;
	p[i++] = 0xC0;
	/* Rdmsr. */
	p[i++] = 0x0F;
	p[i++] = 0x32;
	/* Or eax, 0x100 (LME) */
	p[i++] = 0x0D;
	p[i++] = 0x00;
	p[i++] = 0x01;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Wrmsr. */
	p[i++] = 0x0F;
	p[i++] = 0x30;

	/* Mov eax, cr0. */
	p[i++] = 0x0F;
	p[i++] = 0x20;
	p[i++] = 0xC0;
	/* Or eax, 0x80000000 (PG) */
	p[i++] = 0x0D;
	p[i++] = 0x00;
	p[i++] = 0x00;
	p[i++] = 0x00;
	p[i++] = 0x80;
	/* Mov cr0, eax. */
	p[i++] = 0x0F;
	p[i++] = 0x22;
	p[i++] = 0xC0;

	/*
	 * Far jmp 0x10:lm64_entry (switch to 64-bit code segment)
	 * In 32-bit mode: 0xEA <32-bit offset> <16-bit selector>.
	 */
	int lm64_patch = i + 1;
	p[i++] = 0xEA;
	p[i++] = 0;
	p[i++] = 0;
	p[i++] = 0;
	p[i++] = 0;		/* Offset (patched) */
	p[i++] = 0x18;
	p[i++] = 0x00;		/* Selector = 0x0018 (code64, GDT[3]) */

	/* === 64-Bit long mode === */
	int lm64_start = i;
	uint32_t lm64_phys = 0x8000 + (uint32_t) lm64_start;
	ap_trampoline_code[lm64_patch + 0] = (uint8_t) (lm64_phys);
	ap_trampoline_code[lm64_patch + 1] = (uint8_t) (lm64_phys >> 8);
	ap_trampoline_code[lm64_patch + 2] = (uint8_t) (lm64_phys >> 16);
	ap_trampoline_code[lm64_patch + 3] = (uint8_t) (lm64_phys >> 24);

	/*
	 * Now in 64-bit mode. Self-identify by atomically grabbing a slot
	 * from a counter at [0x8F20] (ap_count field in trampoline data).
	 * Each AP gets a unique index to look up its stack/cpu_local.
	 */

	/* --- Atomic fetch-and-add on ap_count to get our slot --- */
	/* Mov eax, 1. */
	p[i++] = 0xB8;
	p[i++] = 0x01;
	p[i++] = 0x00;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Mov ebx, 0x8F20 -- address of ap_count. */
	p[i++] = 0xBB;
	p[i++] = 0x20;
	p[i++] = 0x8F;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Lock xadd [rbx], eax -- atomically: old=*rbx, *rbx+=eax, eax=old. */
	p[i++] = 0xF0;
	p[i++] = 0x0F;
	p[i++] = 0xC1;
	p[i++] = 0x03;
	/* Eax now has our slot index (0 for first AP, 1 for second, etc.) */

	/* --- Load ap_info array pointer --- */
	/* Mov ebx, 0x8F10 -- address of ap_info_phys in data block. */
	p[i++] = 0xBB;
	p[i++] = 0x10;
	p[i++] = 0x8F;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Mov rbx, [rbx] -- load ap_info physical address. */
	p[i++] = 0x48;
	p[i++] = 0x8B;
	p[i++] = 0x1B;

	/* --- Index into array: eax = slot, each entry is 16 bytes --- */
	/* Shl eax, 4 -- slot * 16. */
	p[i++] = 0xC1;
	p[i++] = 0xE0;
	p[i++] = 0x04;
	/* Add rbx, rax -- rbx = &ap_info[slot] (phys addr) */
	p[i++] = 0x48;
	p[i++] = 0x01;
	p[i++] = 0xC3;

	/* --- Load per-AP stack --- */
	/* Mov rsp, [rbx] -- stack_top. */
	p[i++] = 0x48;
	p[i++] = 0x8B;
	p[i++] = 0x23;

	/* --- Load per-AP cpu_local into RDI (first arg to ap_entry_64) --- */
	/* Mov rdi, [rbx + 8] -- cpu_local_ptr. */
	p[i++] = 0x48;
	p[i++] = 0x8B;
	p[i++] = 0x7B;
	p[i++] = 0x08;

	/* --- Park: spin on wake_flag at [0x8F18] until BSP sets it --- */
	/* Mov ebx, 0x8F18 -- address of wake_flag. */
	p[i++] = 0xBB;
	p[i++] = 0x18;
	p[i++] = 0x8F;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Spin: mov eax, [rbx]. */
	p[i++] = 0x8B;
	p[i++] = 0x03;
	/* Test eax, eax. */
	p[i++] = 0x85;
	p[i++] = 0xC0;
	/* Jnz wake (+4) */
	p[i++] = 0x75;
	p[i++] = 0x04;
	/* Pause. */
	p[i++] = 0xF3;
	p[i++] = 0x90;
	/* Jmp spin (-10) */
	p[i++] = 0xEB;
	p[i++] = 0xF6;
	/* Wake: */

	/* --- Jump to entry point --- */
	/* Mov eax, 0x8F08 -- address of entry_64 in data block. */
	p[i++] = 0xB8;
	p[i++] = 0x08;
	p[i++] = 0x8F;
	p[i++] = 0x00;
	p[i++] = 0x00;
	/* Mov rax, [rax]. */
	p[i++] = 0x48;
	p[i++] = 0x8B;
	p[i++] = 0x00;
	/* Jmp rax. */
	p[i++] = 0xFF;
	p[i++] = 0xE0;
}

void
trampoline_install(uint64_t phys_base)
{
	struct ap_trampoline_data *data;
	uint8_t *dest = (uint8_t *) phys_to_virt(phys_base);

	/* Build the code blob (patches far-jump targets) */
	trampoline_build();

	/* Copy code to trampoline page. */
	for (int i = 0; i < (int)sizeof(ap_trampoline_code); i++)
		dest[i] = ap_trampoline_code[i];

	/* Set up the temporary GDT in the data area. */
	data = (struct ap_trampoline_data *)(dest + TRAMPOLINE_DATA_OFFSET);

	/* Null descriptor. */
	data->tmp_gdt[0] = 0;
	/* [1] Code32: base=0, limit=4G, 32-bit, execute/read, present, DPL=0. */
	data->tmp_gdt[1] = 0x00CF9A000000FFFFULL;
	/* [2] Data32: base=0, limit=4G, 32-bit, read/write, present, DPL=0. */
	data->tmp_gdt[2] = 0x00CF92000000FFFFULL;
	/* [3] Code64: base=0, limit=0, 64-bit (L=1), execute/read, present, DPL=0. */
	data->tmp_gdt[3] = 0x00209A0000000000ULL;

	/* Write packed GDTR: 2-byte limit + 4-byte base. */
	uint16_t gdt_limit = (4 * 8) - 1;
	/* tmp_gdt is at offset 0x28 in the struct (8+8+8+8+4+4 = 40) */
	uint32_t gdt_base =
	    (uint32_t) (phys_base + TRAMPOLINE_DATA_OFFSET + 0x28);
	data->tmp_gdtr[0] = (uint8_t) (gdt_limit & 0xFF);
	data->tmp_gdtr[1] = (uint8_t) ((gdt_limit >> 8) & 0xFF);
	data->tmp_gdtr[2] = (uint8_t) (gdt_base & 0xFF);
	data->tmp_gdtr[3] = (uint8_t) ((gdt_base >> 8) & 0xFF);
	data->tmp_gdtr[4] = (uint8_t) ((gdt_base >> 16) & 0xFF);
	data->tmp_gdtr[5] = (uint8_t) ((gdt_base >> 24) & 0xFF);
}

struct ap_trampoline_data *
trampoline_data(uint64_t phys_base)
{
	uint8_t *dest = (uint8_t *) phys_to_virt(phys_base);
	return (struct ap_trampoline_data *)(dest + TRAMPOLINE_DATA_OFFSET);
}
