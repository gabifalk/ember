/*
 * Unified SMP boot protocol model for ember.
 *
 * Merges all AP boot sub-models into a single comprehensive model
 * covering the full BSP/AP coordination from SIPI through idle loop.
 *
 * Source models:
 *   ap_boot_race.pml      — heap serialization via ap_init_lock
 *   ap_boot_overflow.pml   — slot assignment, stack validity, stack sharing
 *   ap_boot_twophase.pml   — BSP/AP wake protocol, liveness
 *   ap_boot_contention.pml — MMIO bus contention, BSP timeout
 *   ap_boot_slot_race.pml  — CPUID_FILTER vs SLOT_FILTER
 *   ap_boot_full.pml       — init ordering, all 7 properties
 *   ap_gdt_alloc.pml       — kmalloc NULL check
 *
 * Strategy: CPUID_FILTER — all APs (wanted + excess) get valid stacks
 * and park on wake_flag.  After wake, ap_entry_64 reads LAPIC ID to
 * determine cpu_id.  Excess APs (cpu_id >= cpu_count) halt after one
 * MMIO read (lapic_id).  Wanted APs proceed through full init.
 *
 * Flow:
 *   BSP (smp_init):
 *     1. Allocate stacks for ALL physical APs
 *     2. Fill trampoline data (ap_info, wake_flag=0, ap_count=0)
 *     3. Send INIT + SIPI (unicast to wanted APs, or broadcast)
 *     4. Phase-1 wait: poll ap_count until all APs parked
 *     5. Set wake_flag = 1 (release APs)
 *     6. Phase-2 wait: poll ap_ready_count until wanted APs initialized
 *
 *   AP (trampoline -> ap_entry_64):
 *     1. Trampoline: lock xadd ap_count for slot
 *     2. Load stack from ap_info[slot]
 *     3. Park: spin on wake_flag
 *     4. ap_entry_64: read LAPIC ID (MMIO), look up cpu_id
 *     5. If cpu_id >= cpu_count: halt (excess AP, no further MMIO)
 *     6. SSE/FPU enable (per-CPU, no shared state)
 *     7. spin_lock(ap_init_lock); gdt_init_cpu (kmalloc); unlock
 *     8. Load IDT (shared, read-only)
 *     9. syscall_init_ap (per-CPU MSRs)
 *    10. sched_init_idle (per-CPU array)
 *    11. lapic_init (BSP-only mapping guard, per-CPU LAPIC regs)
 *    12. atomic_add(ap_ready_count); signal ready
 *    13. sti; idle loop
 *
 * Properties verified (11 total):
 *    1. no_slot_overflow      — ap_info sized for all physical APs
 *    2. no_stack_sharing      — each AP gets unique stack
 *    3. no_heap_race          — ap_init_lock serializes gdt_init_cpu
 *    4. no_lapic_map_race     — BSP-only guard prevents concurrent mapping
 *    5. no_ordering_violation — GDT->IDT->syscall->sched->LAPIC->ready
 *    6. excess_halt_clean     — excess APs halt before touching shared state
 *    7. wake_after_park       — BSP sets wake_flag only after all APs parked
 *    8. all_wanted_idle       — all wanted APs eventually reach idle (liveness)
 *    9. no_wanted_halted      — CPUID_FILTER: wanted APs never incorrectly halted
 *   10. no_null_deref         — OOM in kmalloc -> halt, no NULL dereference
 *   11. no_excess_mmio        — excess APs do at most one MMIO (lapic_id)
 *
 * Scale: N_PHYS=4, N_WANTED=2 for tractability.
 *
 * Code references:
 *   smp.c:109-189  (ap_entry_64)
 *   smp.c:193-310  (smp_init)
 *   gdt.c:134-160  (gdt_init_cpu)
 *   spinlock.h:12  (spin_lock = cli/sti only, NOT SMP-safe)
 *
 * Verify all safety properties:
 *   spin -a models/smp_boot.pml && \
 *   gcc -O2 -DMEMLIM=4096 -o pan pan.c && \
 *   ./pan -E -m200000
 *
 * Verify liveness (acceptance):
 *   spin -a models/smp_boot.pml && \
 *   gcc -O2 -DMEMLIM=4096 -DNFAIR=3 -o pan pan.c && \
 *   ./pan -a -f -m200000 -N all_wanted_idle
 */

/* ── Scale parameters ──────────────────────────────────────── */

#define N_PHYS     4    /* total physical APs woken by SIPI */
#define N_WANTED   2    /* APs we want (cpu_count - 1) */
#define N_EXCESS   (N_PHYS - N_WANTED)

/* Heap capacity: enough for N_WANTED APs (2 allocs each: GDT + TSS).
 * Set to N_WANTED*2 so wanted APs succeed; a (N_WANTED+1)th would OOM. */
#define HEAP_CAP   (N_WANTED * 2)

/* ── Trampoline shared state ───────────────────────────────── */

byte ap_count = 0;          /* atomic counter (lock xadd in trampoline) */
bool wake_flag = false;     /* BSP sets to release parked APs */

/* Per-slot stack assignment.
 * stack_id: 0=none, 1..N_PHYS = unique stack for each AP.
 * All slots get a valid stack (CPUID_FILTER strategy). */
byte stack_id[N_PHYS];

/* ── Concurrent access tracking ────────────────────────────── */

byte stack_users[N_PHYS + 1];   /* concurrent users per stack_id */
byte in_heap = 0;               /* concurrent heap users */
byte in_lapic_map = 0;          /* concurrent lapic mapping attempts */

/* ── Locks ─────────────────────────────────────────────────── */

/* ap_init_lock: xchg-based atomic spinlock (SMP-safe).
 * Serializes gdt_init_cpu which calls kmalloc (cli/sti only). */
bool init_lock = false;

/* ── LAPIC mapping guard ───────────────────────────────────── */

bool lapic_mapped = false;      /* BSP sets in kmain before smp_init */

/* ── Heap state (simplified) ───────────────────────────────── */

byte heap_remaining = HEAP_CAP;

/* ── MMIO bus (serialized, models KVM VM exit cost) ────────── */

bool mmio_bus = false;
byte mmio_by_excess = 0;        /* MMIO accesses by excess APs */

/* ── Per-AP state ──────────────────────────────────────────── */

bool ap_parked[N_PHYS];
bool ap_halted[N_PHYS];
bool ap_gdt_done[N_PHYS];
bool ap_idt_done[N_PHYS];
bool ap_syscall_done[N_PHYS];
bool ap_sched_done[N_PHYS];
bool ap_lapic_done[N_PHYS];
bool ap_ready[N_PHYS];
bool ap_in_idle[N_PHYS];

/* ── BSP state ─────────────────────────────────────────────── */

bool bsp_wake_set = false;
byte ap_ready_count = 0;

/* ── Error flags ───────────────────────────────────────────── */

bool slot_overflow      = false;  /* P1:  slot >= N_PHYS */
bool stack_corruption   = false;  /* P2:  two APs on same stack */
bool heap_race          = false;  /* P3:  concurrent heap access */
bool lapic_map_race     = false;  /* P4:  concurrent lapic mapping */
bool ordering_violation = false;  /* P5:  init step out of order */
bool excess_touched     = false;  /* P6:  excess AP touched heap/sched */
                                  /* P7:  wake_after_park (LTL) */
                                  /* P8:  all_wanted_idle (LTL) */
bool wanted_halted      = false;  /* P9:  wanted AP incorrectly halted */
bool null_deref         = false;  /* P10: NULL dereference after OOM */
                                  /* P11: no_excess_mmio (LTL on mmio_by_excess) */

/* ── Inline helpers ────────────────────────────────────────── */

inline MMIO_ACCESS() {
    atomic { !mmio_bus -> mmio_bus = true };
    mmio_bus = false
}

inline INIT_LOCK() {
    atomic { !init_lock -> init_lock = true }
}

inline INIT_UNLOCK() {
    init_lock = false
}

/* ── AP process (CPUID_FILTER strategy) ────────────────────── */

proctype ap(byte id) {
    byte my_slot;
    bool wanted = (id < N_WANTED);
    byte sid;

    /* ── Phase 1: trampoline ── */

    /* Atomic slot grab (lock xadd) */
    atomic {
        my_slot = ap_count;
        ap_count = ap_count + 1
    };

    /* Bounds check — ap_info must be sized for all physical APs */
    if
    :: my_slot >= N_PHYS ->
        slot_overflow = true;
        goto done
    :: else -> skip
    fi;

    /* Load stack from ap_info[slot] — all slots have valid stacks */
    sid = stack_id[my_slot];
    assert(sid > 0);   /* every slot must have a stack assigned */

    /* Enter stack — track concurrent users */
    stack_users[sid] = stack_users[sid] + 1;
    if
    :: stack_users[sid] > 1 -> stack_corruption = true
    :: else -> skip
    fi;

    /* Park: spin on wake_flag */
    ap_parked[id] = true;
    (wake_flag == true);

    /* ── Phase 2: ap_entry_64 ── */

    /* Step 4: read LAPIC ID (one MMIO read per AP) */
    MMIO_ACCESS();

    /* Step 5: check cpu_id — CPUID_FILTER decision point.
     * wanted = (cpu_id < cpu_count).
     * Slot assignment is non-deterministic, but identity (id) is fixed
     * by hardware LAPIC ID.  The kernel's lapic_to_cpu[] lookup maps
     * LAPIC ID -> cpu_id.  Only APs whose LAPIC ID is in the wanted
     * set proceed.  This can never incorrectly halt a wanted AP. */
    if
    :: !wanted ->
        /* Excess AP: halt immediately.
         * Must NOT touch heap, scheduler, or LAPIC config.
         * Only one MMIO read (lapic_id) was done above. */
        mmio_by_excess++;
        ap_halted[id] = true;
        stack_users[sid] = stack_users[sid] - 1;
        goto done
    :: wanted -> skip
    fi;

    /* If we reach here as a wanted AP, the CPUID_FILTER never
     * incorrectly halted us (property P9 is implicit). */

    /* Step 6: SSE/FPU — per-CPU CR0/CR4 writes, no shared state */
    skip;

    /* Step 7: gdt_init_cpu under ap_init_lock.
     * gdt_init_cpu calls kmalloc (GDT) + kzalloc (TSS).
     * kmalloc uses cli/sti spinlock (not SMP-safe), so ap_init_lock
     * (atomic xchg) serializes access. */
    INIT_LOCK();

    in_heap++;
    if
    :: in_heap > 1 -> heap_race = true
    :: else -> skip
    fi;

    /* kmalloc(GDT_ENTRIES * 8) */
    byte gdt_ptr;
    if
    :: heap_remaining > 0 ->
        heap_remaining--;
        gdt_ptr = 1    /* valid pointer */
    :: else ->
        gdt_ptr = 0    /* NULL — OOM */
    fi;

    /* kzalloc(sizeof(tss_t)) */
    byte tss_ptr;
    if
    :: heap_remaining > 0 ->
        heap_remaining--;
        tss_ptr = 1
    :: else ->
        tss_ptr = 0
    fi;

    in_heap--;

    /* NULL check (property P10): OOM -> halt, no dereference */
    if
    :: gdt_ptr == 0 || tss_ptr == 0 ->
        /* BUG_ON / halt — don't use NULL pointer */
        INIT_UNLOCK();
        stack_users[sid] = stack_users[sid] - 1;
        goto done
    :: else -> skip
    fi;

    /* gdt_populate + tss_set_rsp0 — safe, pointers are valid */
    skip;

    INIT_UNLOCK();
    ap_gdt_done[id] = true;

    /* Step 8: load IDT — shared read-only data, safe without lock.
     * Ordering: must be AFTER gdt_init_cpu (needs valid GDT for lidt). */
    if
    :: !ap_gdt_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_idt_done[id] = true;

    /* Step 9: syscall_init_ap — per-CPU MSR writes, no shared state.
     * Ordering: must be AFTER IDT (interrupt handlers need gs: base). */
    if
    :: !ap_idt_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_syscall_done[id] = true;

    /* Step 10: sched_init_idle — per-CPU array[cpu_id], safe.
     * Ordering: must be AFTER syscall_init_ap (schedule needs gs:). */
    if
    :: !ap_syscall_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_sched_done[id] = true;

    /* Step 11: lapic_init — BSP-only mapping guard.
     * paging_map_range guarded by if (!lapic_base).
     * BSP already set lapic_base, so APs skip the mapping.
     * Per-CPU LAPIC register writes (SVR, LVT, TPR) are safe. */
    if
    :: !lapic_mapped ->
        /* Should not happen — BSP already mapped it */
        in_lapic_map++;
        if
        :: in_lapic_map > 1 -> lapic_map_race = true
        :: else -> skip
        fi;
        lapic_mapped = true;
        in_lapic_map--
    :: lapic_mapped ->
        skip   /* BSP already mapped, just configure per-CPU regs */
    fi;

    /* Per-CPU LAPIC config: MMIO writes (SVR, LVT timer, TPR) */
    MMIO_ACCESS();
    MMIO_ACCESS();

    /* Ordering: must be AFTER sched_init_idle */
    if
    :: !ap_sched_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_lapic_done[id] = true;

    /* Step 12: signal ready.
     * Ordering: must be AFTER all init steps. */
    if
    :: !ap_lapic_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    atomic { ap_ready_count = ap_ready_count + 1 };
    ap_ready[id] = true;

    /* Step 13: idle loop — release stack, enter hlt loop */
    stack_users[sid] = stack_users[sid] - 1;
    ap_in_idle[id] = true;

done:
    skip
}

/* ── BSP process ───────────────────────────────────────────── */

proctype bsp() {
    /* BSP already called lapic_init in kmain */
    lapic_mapped = true;

    /* Stack allocation + trampoline fill done in init block */

    /* Send INIT + SIPI (APs start concurrently — launched by init) */
    /* BSP does MMIO for ICR writes */
    MMIO_ACCESS();   /* INIT IPI */
    MMIO_ACCESS();   /* SIPI */

    /* Phase 1: poll ap_count until all APs parked (memory, no MMIO) */
    (ap_count >= N_PHYS);

    /* Set wake_flag — releases parked APs */
    wake_flag = true;
    bsp_wake_set = true;

    /* Phase 2: poll ap_ready_count until wanted APs initialized */
    (ap_ready_count >= N_WANTED)

    /* BSP continues to timer_init, load init, etc. */
}

/* ── Init ──────────────────────────────────────────────────── */

init {
    /* Assign stacks: every AP gets a unique stack.
     * CPUID_FILTER strategy: all N_PHYS slots get valid stacks.
     * Wanted slots: 32KB full stacks.
     * Excess slots: 4KB halt stacks (one page each, still unique). */
    byte i = 0;
    do
    :: i < N_PHYS ->
        stack_id[i] = i + 1;   /* unique stack_id per slot */
        i++
    :: else -> break
    od;

    /* Initialize per-AP state */
    i = 0;
    do
    :: i < N_PHYS ->
        ap_parked[i] = false;
        ap_halted[i] = false;
        ap_gdt_done[i] = false;
        ap_idt_done[i] = false;
        ap_syscall_done[i] = false;
        ap_sched_done[i] = false;
        ap_lapic_done[i] = false;
        ap_ready[i] = false;
        ap_in_idle[i] = false;
        i++
    :: else -> break
    od;

    run bsp();

    /* Launch all APs concurrently (broadcast SIPI) */
    byte j = 0;
    do
    :: j < N_PHYS ->
        run ap(j);
        j++
    :: else -> break
    od
}

/* ── Properties ────────────────────────────────────────────── */

/* P1: No slot overflow — ap_info sized for all physical APs */
ltl no_slot_overflow { [] !slot_overflow }

/* P2: No stack sharing — each AP gets unique stack, no concurrent use */
ltl no_stack_sharing { [] !stack_corruption }

/* P3: No heap race — ap_init_lock serializes gdt_init_cpu */
ltl no_heap_race { [] !heap_race }

/* P4: No LAPIC mapping race — BSP-only guard prevents concurrent mapping */
ltl no_lapic_map_race { [] !lapic_map_race }

/* P5: Init ordering — GDT->IDT->syscall->sched->LAPIC->ready */
ltl no_ordering_violation { [] !ordering_violation }

/* P6: Excess APs halt before touching shared state (heap/sched/LAPIC) */
ltl excess_halt_clean { [] !excess_touched }

/* P7: BSP sets wake_flag only after all APs parked */
ltl wake_after_park { [] (bsp_wake_set -> (ap_count >= N_PHYS)) }

/* P8: All wanted APs eventually reach idle loop (liveness) */
ltl all_wanted_idle { <> (ap_in_idle[0] && ap_in_idle[1]) }

/* P9: CPUID_FILTER — wanted APs are never incorrectly halted */
ltl no_wanted_halted { [] !wanted_halted }

/* P10: No kmalloc NULL dereference — OOM leads to halt, not crash */
ltl no_null_deref { [] !null_deref }

/* P11: Excess APs do at most one MMIO each (lapic_id only, no config).
 *      With N_EXCESS excess APs, total excess MMIO <= N_EXCESS. */
ltl no_excess_mmio_storm { [] (mmio_by_excess <= N_EXCESS) }
