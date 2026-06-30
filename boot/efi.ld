/* boot/efi.ld — Bootloader-only linker script.
 * Boot code runs identity-mapped at low addresses (vaddr == paddr).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

ENTRY(efi_main)

SECTIONS
{
    . = 0x401000;

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note.*)
    }
}
