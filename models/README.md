# Ember Concurrency Models

Promela models verified with SPIN before implementing in C.
Every model supports compile-time bug injection (`-DBUGGY` or similar)
to confirm the property actually catches the bug it targets.

## Running

Install SPIN (https://github.com/nimble-code/Spin), then:

```bash
cd models
spin -a unified_smp.pml && gcc -O2 -DMEMLIM=8192 -o pan pan.c && ./pan
```

For liveness checks (requires weak fairness):

```bash
./pan -a -f -N progress_cpu0 -m100000
```

To inject a known bug and confirm the model catches it:

```bash
spin -a -DBUGGY_MPROTECT unified_smp.pml && gcc -O2 -o pan pan.c && ./pan
```

## Unified model

`unified_smp.pml` is the comprehensive SMP correctness model that
subsumes most earlier single-purpose models.  It checks 10 properties
at every transition:

| Property | Invariant                                          | Kernel test          |
|----------|----------------------------------------------------|----------------------|
| P1       | cpu_cr3[c] refcount > 0 (no dangling CR3)          | smp-fork             |
| P2       | Writable page refcount == 1 (no COW aliasing)      | smp-cow              |
| P3       | Freed page not in any TLB (no stale TLB)           | --                   |
| P4       | Each process on <= 1 CPU (no double schedule)      | smp-fork             |
| P5       | BKL held during kernel ops (serialization)         | --                   |
| P6       | vfork parent blocked while child shares AS         | vfork                |
| P7       | Signal delivery targets correct stack               | signal, signal-adv   |
| P8       | mprotect preserves COW bit                         | mprotect-cow         |
| P9       | After thread munmap, no sibling TLB caches freed   | --                   |
| P10      | No process in user mode with undelivered signals   | signal-preempt       |

## All models

### Core SMP

| Model              | Verifies                                                          | Status   |
|--------------------|-------------------------------------------------------------------|----------|
| unified_smp.pml    | All 10 properties above                                          | Verified |
| bkl.pml            | BKL mutual exclusion, deadlock freedom, liveness                  | Verified |
| bkl_sched.pml      | BKL + scheduler, timer ISR without BKL                            | Verified |
| bkl_sched_fixed.pml| Timer ISR kernel-mode safety (EOI + atomic tick only)             | Verified |
| bkl_sched_v3.pml   | Five schedule() gaps (context_switch, AP prev, sleep, fork, wake) | Verified |
| bkl_sched_v4.pml   | Execution context tracking (CTX_IDLE, CTX_SYSCALL, etc.)         | Verified |
| bkl_sched_v5.pml   | sched_sleep gap and lost wakeup                                   | Verified |
| bkl_sched_v6.pml   | Stale prev state after BKL reacquire (dual-run bug)               | Verified |
| bkl_sched_v7.pml   | Fix for stale-prev dual-run (prev->state check placement)        | Verified |
| bkl_sched_v8.pml   | Kernel stack ownership, idle stack separation                     | Verified |
| bkl_sched_v9.pml   | Recursive schedule() and idle_loop reentry                        | Verified |
| fpu.pml            | FPU state save/restore across context_switch and migration        | Verified |

### TLB coherency

| Model                       | Verifies                                                   | Status   |
|-----------------------------|------------------------------------------------------------|----------|
| tlb.pml                     | TLB coherency with COW fork and fault resolution           | Verified |
| tlb_cow.pml                 | TLB flush after fork COW (write_cr3 before return to user) | Verified |
| tlb_migrate.pml             | TLB validity when process migrates between CPUs            | Verified |
| tlb_mprotect.pml            | TLB safety during mprotect (invlpg + write_cr3 sufficient)| Verified |
| tlb_ipi_deadlock.pml        | TLB shootdown IPI vs BKL spin deadlock (sti in spin loop)  | Verified |
| tlb_deferred.pml            | Deferred page freeing with TLB generation counters         | Verified |
| munmap_tlb_race.pml         | munmap with IPI shootdown (stale TLB after free)           | Verified |
| mmap_fixed_tlb.pml          | MAP_FIXED IPI shootdown before page free (shared PML4)     | Verified |
| fork_cow_tlb_multipage.pml  | Multi-page TLB flush with IPI shootdown in paging_clone    | Verified |

### Process lifecycle

| Model                | Verifies                                                | Status   |
|----------------------|---------------------------------------------------------|----------|
| exec.pml             | Fork + exec PML4 lifecycle and refcount sharing         | Verified |
| exec_signal_race.pml | Exec + signal delivery atomicity under BKL              | Verified |
| exit_stop.pml        | Exit/wait4 with SIGSTOP/SIGCONT                         | Verified |
| exit_isr_bkl_bypass.pml | do_exit_from_isr BKL safety                         | Verified |
| wait_lost_wakeup.pml | wait4/exit sleep/wakeup (no lost wakeup)                | Verified |
| zombie_reap_race.pml | Zombie reap vs reparent-auto-reap (no double-free)      | Verified |
| fd_refcount_race.pml | File descriptor refcount with fork/close                | Verified |
| elf_overlap.pml      | ELF loader overlapping PT_LOAD segments                 | Verified |
| exec_pml4_leak.pml   | Exec frees new PML4 on elf_load/stack failure (no leak) | Verified |

### Signals and timers

| Model                    | Verifies                                                  | Status   |
|--------------------------|-----------------------------------------------------------|----------|
| signal.pml               | Signal delivery to sleeping process                       | Verified |
| sigint_pgid.pml          | SIGINT delivery vs process groups (TIOCSPGRP)             | Verified |
| timer_alarm_race.pml     | Alarm cancellation vs expiry under BKL                    | Verified |
| ticks_race.pml           | kernel_ticks non-atomic RMW safety, nanosleep liveness    | Verified |
| nanosleep.pml            | Nanosleep BKL livelock, signal interruption               | Verified |
| nanosleep_v2.pml         | Tick wakeup in timer_handler vs timer_eoi_kernel          | Draft    |
| syscall_rdi_preempt.pml  | Syscall RDI corruption via timer preempt                  | Verified |
| syscall_timer_rip.pml    | Kstack RIP corruption (slot reuse, signal, exec, idle)    | Verified |
| pipe.pml                 | Pipe sleep/wakeup (no lost wakeup)                        | Verified |

### SMP boot

| Model                  | Verifies                                                   | Status   |
|------------------------|------------------------------------------------------------|----------|
| smp_boot.pml           | Unified boot protocol (merged from 7 sub-models)           | Verified |
| ap_boot_race.pml       | AP startup heap corruption (cli/sti not SMP-safe)          | Verified |
| ap_boot_twophase.pml   | Two-phase AP boot (park then wake) protocol                | Verified |
| ap_boot_overflow.pml   | AP slot overflow, stack allocation, shared halt stack      | Verified |
| ap_boot_full.pml       | Complete BSP/AP coordination with all guards               | Verified |
| ap_boot_contention.pml | MMIO bus contention and BSP timeout                        | Verified |
| ap_boot_slot_race.pml  | Slot assignment race (SLOT_FILTER vs CPUID_FILTER)         | Verified |
| ap_gdt_alloc.pml       | AP GDT allocation and NULL check after kmalloc             | Verified |

### Hardware and memory

| Model                  | Verifies                                                   | Status   |
|------------------------|------------------------------------------------------------|----------|
| tss_rsp0_stale.pml     | TSS.RSP0 stale stack (no multi-CPU kstack sharing)         | Verified |
| tss_rsp0_full.pml      | Complete TSS.RSP0 lifecycle with IPI and frame overlap     | Verified |
| gs_kstack_race.pml     | Per-CPU gs:0 staleness during process migration            | Verified |
| gs_kstack_full.pml     | Full gs:0/TSS.RSP0 lifecycle through all schedule() paths | Verified |
| gs_segment_load.pml    | GS segment load zeroes GS base in enter_user              | Verified |
| nested_isr_swapgs.pml  | Nested ISR between swapgs and iretq (gs:0 corruption)     | Verified |
| pmm_hhdm_coverage.pml  | PMM/HHDM coverage (HHDM maps all allocatable pages)       | Verified |
| blkcache_evict_race.pml| Block cache lock held during entire read path              | Verified |
| partial_alloc_rollback.pml | Rollback of partial page alloc in mmap/mremap/clone   | Verified |
| bucket_drain.pml         | Bucket cache cap prevents heap fragmentation               | Verified |

### VFS

| Model                       | Verifies                                                   | Status   |
|-----------------------------|------------------------------------------------------------|----------|
| vfs_node_lifecycle.pml      | VFS node kfree on evict+unref: no UAF, no double-free, no leak | Verified |
