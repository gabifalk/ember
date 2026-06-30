/* kernel/kernel.ld — Standalone kernel linked at higher-half VMA.
 *
 * The bootloader loads PT_LOAD segments at their p_paddr (physical)
 * addresses and maps them to p_vaddr (virtual) in the page tables.
 * We use AT() to set p_paddr to low addresses (starting at 0x100000)
 * so the ELF can be loaded into conventional memory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

ENTRY(kmain)

KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x100000;  /* 1 MiB — safe physical load address */

SECTIONS
{
    . = KERNEL_VMA;

    .text : AT(KERNEL_LMA) {
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : AT(KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA)) {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : AT(KERNEL_LMA + (ADDR(.data) - KERNEL_VMA)) {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    .bss : AT(KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA)) {
        *(.bss .bss.*)
        *(COMMON)
    }

    _kernel_end = .;
    _kernel_size = _kernel_end - KERNEL_VMA;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note.*)
    }
}
