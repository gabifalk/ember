# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
project('ember', default_options: ['warning_level=0'])

srcdir = meson.project_source_root()
cc_prog = find_program(get_option('kernel_cc'), 'gcc', 'tcc')
user_cc_prog = find_program(get_option('user_cc'), 'gcc')

# --- Native host tools ---
elf2efi = custom_target('elf2efi',
  output: 'elf2efi',
  input: 'tools/elf2efi.c',
  command: ['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-o', '@OUTPUT@', '@INPUT@'],
)

ember_ld = custom_target('ember-ld',
  output: 'ember-ld',
  input: 'tools/ember-ld.c',
  command: ['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-o', '@OUTPUT@', '@INPUT@'],
)

# --- Kernel scripts ---
compile_kernel = find_program('scripts/compile_kernel.sh')

# Collect all source files as inputs for dependency tracking
all_src = files(
  'boot/efi_main.c',
  'boot/efi_entry.S',
  'boot/uefi_call.S',
  'kernel/console/serial.c',
  'kernel/console/console.c',
  'kernel/console/fbcon.c',
  'kernel/arch/x86_64/syscall_entry.S',
  'kernel/arch/x86_64/syscall.c',
  'kernel/arch/x86_64/gdt.c',
  'kernel/arch/x86_64/gdt_entry.S',
  'kernel/arch/x86_64/tss_entry.S',
  'kernel/arch/x86_64/idt.c',
  'kernel/arch/x86_64/isr.c',
  'kernel/arch/x86_64/isr_entry.S',
  'kernel/arch/x86_64/user.S',
  'kernel/arch/x86_64/paging.c',
  'kernel/syscall.c',
  'kernel/syscall_file.c',
  'kernel/syscall_fs.c',
  'kernel/syscall_proc.c',
  'kernel/syscall_mm.c',
  'kernel/syscall_sig.c',
  'kernel/syscall_misc.c',
  'kernel/syscall_id.c',
  'kernel/syscall_time.c',
  'kernel/syscall_poll.c',
  'kernel/user/elf.c',
  'kernel/user/user.c',
  'kernel/kmain.c',
  'kernel/mm/pmm.c',
  'kernel/mm/heap.c',
  'kernel/dev/pci.c',
  'kernel/dev/blkdev.c',
  'kernel/dev/ahci.c',
  'kernel/dev/ata.c',
  'kernel/dev/blkcache.c',
  'kernel/dev/ramdisk.c',
  'kernel/dev/gpt.c',
  'kernel/fs/cpio.c',
  'kernel/fs/vfs.c',
  'kernel/fs/ext2.c',
  'kernel/fs/fat32.c',
  'kernel/fs/fd.c',
  'kernel/proc.c',
  'kernel/pipe.c',
  'kernel/sched.c',
  'kernel/arch/x86_64/switch.S',
  'kernel/arch/x86_64/timer.c',
  'kernel/kexec.c',
  'kernel/arch/x86_64/kexec_entry.S',
  'tools/ember-ld.c',
)

# --- Kernel archives ---
kernel_archives = custom_target('kernel-archives',
  output: ['boot.a', 'kernel.a'],
  input: all_src,
  command: [compile_kernel, 'compile', cc_prog, srcdir,
            meson.project_build_root(), '@OUTPUT0@', '@OUTPUT1@'],
)

# --- Boot and kernel ELFs ---
boot_elf = custom_target('boot.elf',
  output: 'boot.elf',
  input: kernel_archives,
  depends: [ember_ld],
  command: [compile_kernel, 'link', ember_ld, srcdir,
            '--boot-elf', '@OUTPUT@',
            '--boot-a', '@INPUT0@'],
)

kernel_elf = custom_target('kernel.elf',
  output: 'kernel.elf',
  input: kernel_archives,
  depends: [ember_ld],
  command: [compile_kernel, 'link', ember_ld, srcdir,
            '--kernel-elf', '@OUTPUT@',
            '--kernel-a', '@INPUT1@'],
  build_by_default: true,
)

# --- BOOTX64.EFI ---
bootx64 = custom_target('BOOTX64.EFI',
  output: 'BOOTX64.EFI',
  input: boot_elf,
  depends: [elf2efi],
  command: [elf2efi, '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
)

# --- Test artifacts and run targets ---
subdir('tests')
