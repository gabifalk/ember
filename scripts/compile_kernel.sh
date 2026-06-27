#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# compile_kernel.sh -- compile and/or link kernel/boot sources
#
# Usage:
#   compile_kernel.sh compile <cc> <srcdir> <outdir> <boot.a> <kernel.a> [extra_cflags...]
#   compile_kernel.sh link    <cc> <srcdir> --boot-elf <out> --boot-a <a> --kernel-elf <out> --kernel-a <a>
set -eu

MODE="$1"; shift

BOOT_SRCS="
boot/efi_main.c boot/efi_entry.S boot/uefi_call.S
"

KERNEL_SRCS="
kernel/console/serial.c kernel/console/console.c kernel/console/fbcon.c
kernel/arch/x86_64/syscall_entry.S kernel/arch/x86_64/syscall.c
kernel/arch/x86_64/gdt.c kernel/arch/x86_64/gdt_entry.S
kernel/arch/x86_64/tss_entry.S
kernel/arch/x86_64/idt.c
kernel/arch/x86_64/isr.c kernel/arch/x86_64/isr_entry.S
kernel/arch/x86_64/user.S kernel/arch/x86_64/paging.c
kernel/syscall.c kernel/syscall_helpers.c kernel/syscall_pipe.c kernel/syscall_file.c kernel/syscall_file_io.c kernel/syscall_file_ops.c kernel/syscall_fs.c kernel/syscall_fs_stat.c kernel/syscall_fs_path.c kernel/syscall_fs_mod.c kernel/syscall_fs_perm.c kernel/syscall_proc.c kernel/syscall_proc_fork.c kernel/syscall_proc_exec.c kernel/syscall_proc_exit.c kernel/syscall_proc_wait.c kernel/syscall_proc_fd.c kernel/syscall_mm.c kernel/syscall_sig.c kernel/syscall_misc.c kernel/syscall_id.c kernel/syscall_time.c kernel/syscall_poll.c kernel/user/elf.c kernel/user/user.c
kernel/kmain.c kernel/klib.c
kernel/mm/pmm.c kernel/mm/heap.c
kernel/dev/pci.c kernel/dev/irq.c kernel/dev/blkdev.c kernel/dev/ahci.c kernel/dev/ata.c kernel/dev/blkcache.c kernel/dev/ramdisk.c kernel/dev/gpt.c
kernel/fs/cpio.c kernel/fs/vfs.c kernel/fs/vfs_cache.c kernel/fs/vfs_lookup.c kernel/fs/ext2.c kernel/fs/ext2_inode.c kernel/fs/ext2_block.c kernel/fs/ext2_io.c kernel/fs/ext2_dir.c kernel/fs/ext2_ops.c kernel/fs/fat32.c kernel/fs/fd.c
kernel/proc.c kernel/pipe.c
kernel/sched.c kernel/arch/x86_64/switch.S kernel/arch/x86_64/timer.c
kernel/arch/x86_64/acpi.c kernel/arch/x86_64/lapic.c kernel/arch/x86_64/ioapic.c kernel/arch/x86_64/ap_trampoline.c kernel/arch/x86_64/smp.c
kernel/kexec.c kernel/arch/x86_64/kexec_entry.S
"

case "$MODE" in
compile)
    CC="$1"; SRCDIR="$2"; OUTDIR="$3"; BOOT_A="$4"; KERNEL_A="$5"
    shift 5
    EXTRA_CFLAGS="$*"

    CFLAGS="-m64 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -mno-red-zone -I${SRCDIR}/include"

    # Detect compiler: gcc/clang need code model flags and no unwind tables;
    # tcc does not support these flags.
    IS_GCC=0
    case "$("$CC" --version 2>&1 || true)" in
        *gcc*|*GCC*|*clang*|*CLANG*) IS_GCC=1 ;;
    esac
    if [ "$IS_GCC" = 1 ]; then
        CFLAGS="$CFLAGS -fno-asynchronous-unwind-tables -fno-exceptions"
        BOOT_MODEL="-mcmodel=large"
        KERNEL_MODEL="-mcmodel=kernel"
    else
        BOOT_MODEL=""
        KERNEL_MODEL=""
    fi

    BOOT_OBJS=""
    for f in $BOOT_SRCS; do
        obj="${OUTDIR}/${f%.*}.o"
        mkdir -p "$(dirname "$obj")"
        "$CC" $CFLAGS $BOOT_MODEL $EXTRA_CFLAGS -c "${SRCDIR}/${f}" -o "$obj"
        BOOT_OBJS="$BOOT_OBJS $obj"
    done

    KERN_OBJS=""
    for f in $KERNEL_SRCS; do
        obj="${OUTDIR}/${f%.*}.o"
        mkdir -p "$(dirname "$obj")"
        "$CC" $CFLAGS $KERNEL_MODEL $EXTRA_CFLAGS -c "${SRCDIR}/${f}" -o "$obj"
        KERN_OBJS="$KERN_OBJS $obj"
    done

    ar rcsT "$BOOT_A" $BOOT_OBJS
    ar rcsT "$KERNEL_A" $KERN_OBJS
    ;;

link)
    EMBER_LD="$1"; SRCDIR="$2"
    shift 2
    case "$EMBER_LD" in /*) ;; *) EMBER_LD="$(pwd)/$EMBER_LD" ;; esac
    # Parse remaining args
    BOOT_ELF=""; KERNEL_ELF=""; BOOT_A=""; KERNEL_A=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --boot-elf)   BOOT_ELF="$2"; shift 2 ;;
            --kernel-elf) KERNEL_ELF="$2"; shift 2 ;;
            --boot-a)     BOOT_A="$2"; shift 2 ;;
            --kernel-a)   KERNEL_A="$2"; shift 2 ;;
            *)            shift ;;
        esac
    done

    # Link bootloader
    if [ -n "$BOOT_ELF" ] && [ -n "$BOOT_A" ]; then
        "$EMBER_LD" -e efi_main --base=0x401000 -o "$BOOT_ELF" -T "$BOOT_A"
    fi

    # Link kernel
    if [ -n "$KERNEL_ELF" ] && [ -n "$KERNEL_A" ]; then
        "$EMBER_LD" -e kmain --vma=0xffffffff80000000 --lma=0x100000 \
            -o "$KERNEL_ELF" -T "$KERNEL_A"
    fi
    ;;

*)
    echo "Usage: compile_kernel.sh {compile|link} ..." >&2
    exit 1
    ;;
esac
