# Ember - AMD64 UEFI-only Kernel

A tiny AMD64 (x86_64) UEFI-only kernel with a Linux-shaped syscall ABI
sufficient to run a real libc (musl, glibc-static), `tcc`, and basic
build utilities.

## Design

- **UEFI-only boot** -- no legacy BIOS. Single `BOOTX64.EFI` binary (loader + kernel).
- **No EFI Runtime Services** after `ExitBootServices()`.
- **4 KiB pages only**, 4-level paging (no LA57, no huge pages).
- **Bitmap PMM** with Higher-Half Direct Map (HHDM at `0xffff800000000000`).
- **Kernel at** `0xffffffff80000000`, user space in lower canonical half.
- **Static ELF64 only** (`ET_EXEC` and `ET_DYN`/PIE), no dynamic linking.
- **SMP support** with Big Kernel Lock (BKL). LAPIC timer on multi-CPU, PIC+PIT on single-CPU.
- **Copy-on-write fork** with PMM refcounting.
- **Linux x86_64 syscall ABI**: RAX=number, args in RDI/RSI/RDX/R10/R8/R9, return in RAX.
- **Formally verified** concurrency with 50+ SPIN/Promela models (see `models/`).

## Kernel Capabilities

### Boot and Memory

- UEFI boot via `BOOTX64.EFI` (custom `elf2efi` converter)
- Bitmap physical memory manager (PMM) with refcounting for COW
- Kernel heap (`kmalloc`/`kfree`)
- 4-level paging with NX bit support (EFER.NXE)
- Higher-Half Direct Map for kernel access to all physical memory

### CPU and Interrupts

- GDT with kernel and user segments (per-CPU on SMP)
- IDT with exception handlers (divide-by-zero, page fault, GPF, etc.)
- TSS for kernel stack switching on privilege transitions
- PIC + PIT timer at 100 Hz (single-CPU) or LAPIC timer (SMP)
- `SYSCALL`/`SYSRET` entry/exit with full register save/restore
- ACPI table parsing (MADT, I/O APIC)
- SMP boot via SIPI with two-phase AP initialization

### Consoles

- Serial output (COM1 16550 UART) with ring buffer
- Framebuffer console (GOP-based, with basic text rendering)

### Processes and Scheduling

- Preemptive round-robin scheduler with context switching
- Process table (up to 256 processes)
- `fork` with copy-on-write page sharing
- `vfork` with address space sharing
- `execve` with ELF64 loading into new address space
- `wait4`/`waitid` with blocking sleep/wakeup
- `exit`/`exit_group` with zombie reaping
- `clone` with CLONE_VM thread support
- Per-process kernel stacks (32 KiB)
- Per-CPU idle stacks for SMP scheduling

### Memory Management (User Space)

- `brk` for heap expansion
- `mmap` -- anonymous and file-backed (CPIO and ext2), with proper permission flags
- `munmap` -- unmaps pages and frees physical frames
- `mprotect` -- page table permission updates, preserves COW bit
- `mremap` -- resize mappings
- Per-process VMA tracking (64 slots), copied on fork, cleared on execve
- TLB shootdown via IPI for CLONE_VM threads

### Signals

- `rt_sigaction` -- install signal handlers per process
- `rt_sigprocmask` -- block/unblock signals (SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK)
- `rt_sigreturn` -- restore context from signal trampoline
- `rt_sigpending`, `rt_sigsuspend`, `tkill`, `tgkill`, `sigaltstack`
- Signal delivery on syscall return and timer preemption
- Default actions: SIGCHLD (ignore), SIGPIPE/SIGTERM/SIGKILL (terminate)
- SA_RESTORER support for glibc-static

### File Descriptors and IPC

- Per-process fd table (up to 256 fds)
- `pipe`/`pipe2` with blocking reads and writes
- `dup`/`dup2`/`dup3` and `fcntl` (F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL)
- `close`, `close_range` with pipe refcount management
- `O_CLOEXEC` support on `execve`
- `select`, `poll`, `ppoll`
- `sendfile`
- `memfd_create`

### Filesystems

- **cpio newc** -- read-only initrd parsed at boot
- **ext2** -- read-write support for 1 KiB block size:
  - `open`/`openat`, `read`, `write`, `close`
  - `creat`, `truncate`/`ftruncate`, `fsync`
  - `mkdir`, `rmdir`, `unlink`, `rename`
  - `link`, `symlink`, `readlink`
  - `getdents64` for directory listing
  - `fstat`, `newfstatat`, `lseek`, `pread64`, `pwrite64`
  - Single and double indirect block mapping
- **FAT32** -- ESP filesystem access
- VFS layer with LRU cache (512 entries)
- `chdir`, `fchdir`, `getcwd`

### Block Devices

- **AHCI** -- PCI scan, SATA port bring-up, READ/WRITE DMA EXT (48-bit LBA)
- **ATA PIO** -- primary channel, LBA28 read/write (fallback when no AHCI)
- Block device abstraction (AHCI preferred, ATA PIO fallback)
- Block cache (4096 entries, write-back)
- Ramdisk driver for in-memory ext2

### kexec

- `kexec_file_load` -- load and boot Linux kernels from Ember
- Builds Linux boot_params with e820 table from EFI memory map

## Build

Requires `meson`, `ninja`, `gcc`, and `mtools`:

```sh
meson setup build
ninja -C build
```

To build with `tcc` as the kernel compiler:

```sh
PATH="/path/to/tcc/bin:$PATH" meson setup build -Dkernel_cc=tcc
PATH="/path/to/tcc/bin:$PATH" ninja -C build
```

## Run (QEMU)

Requires QEMU with OVMF firmware:

```sh
ninja -C build run       # serial-only (nographic)
ninja -C build run-fb    # with framebuffer
```

Set `OVMF_CODE` and `OVMF_VARS` env vars if OVMF is not at the default paths.

## Tests

80 tests covering syscalls, memory management, signals, filesystems,
SMP concurrency, and more:

```sh
meson test -C build -v
```

## Project Structure

```
boot/              EFI loader entry, linker script
kernel/
  arch/x86_64/     GDT, IDT, ISR, TSS, syscall entry, paging, timer,
                   ACPI, LAPIC, I/O APIC, SMP boot
  console/         Serial (COM1 16550), framebuffer console
  dev/             AHCI, ATA PIO, block cache, ramdisk, GPT
  fs/              cpio, ext2, FAT32, VFS, fd table
  mm/              PMM (bitmap), kernel heap
  user/            ELF loader, user process setup
  kmain.c          Kernel main
  syscall*.c       Syscall dispatch and handlers
  proc.c           Process table
  pipe.c           Pipe buffers
  sched.c          Preemptive scheduler
include/ember/     All kernel headers
tools/             elf2efi, ember-ld (custom static linker)
scripts/           Build, link, ESP creation, QEMU runners
tests/             Test init binaries and shared helpers
models/            SPIN/Promela concurrency verification models
```
