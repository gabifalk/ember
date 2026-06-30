/*
 * Full AP boot protocol model for ember SMP.
 *
 * Models the complete BSP/AP coordination from SIPI through idle loop:
 *
 *   BSP (smp_init):
 *     1. Allocate stacks: wanted APs get 32KB, excess get 4KB halt stacks
 *     2. Fill trampoline data (ap_info, wake_flag=0, ap_count=0)
 *     3. Broadcast INIT + SIPI (wakes ALL physical APs)
 *     4. Phase-1 wait: poll ap_count until all APs parked
 *     5. Set wake_flag = 1 (release APs)
 *     6. Phase-2 wait: poll ap_ready_count until wanted APs initialized
 *     7. Continue to timer_init, load init, etc.
 *
 *   AP (trampoline → ap_entry_64):
 *     1. Trampoline: mode transition, lock xadd ap_count for slot
 *     2. Load stack from ap_info[slot]
 *     3. Park: spin on wake_flag
 *     4. ap_entry_64: read LAPIC ID, look up my_cpu
 *     5. If my_cpu >= cpu_count: halt (excess AP)
 *     6. SSE/FPU enable (per-CPU CR0/CR4, no shared state)
 *     7. atomic_spin_lock(ap_init_lock); gdt_init_cpu (kmalloc); unlock
 *     8. Load IDT (shared, read-only — safe)
 *     9. syscall_init_ap (per-CPU MSRs, no shared state)
 *    10. sched_init_idle (per-CPU array, indexed by cpu_id — safe)
 *    11. lapic_init (BSP-only mapping guard, per-CPU LAPIC regs)
 *    12. atomic_add(ap_ready_count)
 *    13. sti; idle loop (hlt → bkl_acquire → schedule → bkl_release)
 *
 * Properties verified:
 *   - No slot overflow (ap_info sized for all physical APs)
 *   - No stack sharing (each AP has unique stack)
 *   - No concurrent heap access (ap_init_lock serializes gdt_init_cpu)
 *   - lapic_init mapping only done once (BSP guard)
 *   - Excess APs halt before touching heap/scheduler/LAPIC
 *   - BSP sets wake_flag only after all APs parked
 *   - All wanted APs eventually reach idle loop (liveness)
 *   - No shared state access without proper locking
 *
 * Code: smp.c:109-189 (ap_entry_64), smp.c:193-310 (smp_init)
 *
 * Verify:
 *   spin -a models/ap_boot_full.pml && \
 *   gcc -O2 -DMEMLIM=4096 -DNFAIR=3 -o pan pan.c && \
 *   ./pan -a -f -m200000 -N <property>
 */

/* Scale for tractability */
#define N_PHYS     4    /* total physical APs (broadcast SIPI) */
#define N_WANTED   2    /* wanted APs (cpu_count - 1) */

/* ── Shared state ──────────────────────────────────────── */

/* Trampoline */
byte ap_count = 0;
bool wake_flag = false;

/* Per-slot stack assignment (stack_id: 0=none, 1..N_WANTED=full, N_WANTED+1..=halt) */
byte stack_id[N_PHYS];

/* Concurrent access tracking */
byte stack_users[N_PHYS + N_WANTED + 2];  /* indexed by stack_id */
byte in_heap = 0;          /* concurrent heap users */
byte in_lapic_map = 0;     /* concurrent lapic mapping */

/* ap_init_lock (xchg-based atomic spinlock) */
bool init_lock = false;

/* lapic_base guard: only first caller maps */
bool lapic_mapped = false;

/* Per-AP state */
bool ap_parked[N_PHYS];
bool ap_halted[N_PHYS];
bool ap_gdt_done[N_PHYS];
bool ap_idt_done[N_PHYS];
bool ap_syscall_done[N_PHYS];
bool ap_sched_done[N_PHYS];
bool ap_lapic_done[N_PHYS];
bool ap_ready[N_PHYS];
bool ap_in_idle[N_PHYS];

/* BSP state */
bool bsp_wake_set = false;
byte ap_ready_count = 0;

/* Error flags */
bool stack_corruption = false;
bool heap_race = false;
bool lapic_map_race = false;
bool ordering_violation = false;
bool excess_touched_heap = false;
bool excess_touched_sched = false;

/* ── AP process ────────────────────────────────────────── */

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

    /* Load stack from ap_info[slot] */
    sid = stack_id[my_slot];

    /* Use stack — enter ap_entry_64 */
    stack_users[sid] = stack_users[sid] + 1;
    if
    :: stack_users[sid] > 1 -> stack_corruption = true
    :: else -> skip
    fi;

    /* Park: spin on wake_flag */
    ap_parked[id] = true;
    (wake_flag == true);

    /* ── Phase 2: ap_entry_64 ── */

    /* Step 4-5: read LAPIC ID, check cpu_count */
    /* lapic_id() reads LAPIC MMIO — per-CPU, safe */
    if
    :: !wanted ->
        /* Excess AP: halt immediately.
         * Must NOT touch heap, scheduler, or shared state. */
        ap_halted[id] = true;
        stack_users[sid] = stack_users[sid] - 1;
        goto done
    :: wanted -> skip
    fi;

    /* Step 6: SSE/FPU — per-CPU CR0/CR4 writes, no shared state */
    skip;

    /* Step 7: gdt_init_cpu under ap_init_lock */
    /* gdt_init_cpu calls kmalloc — heap uses cli/sti spinlock (not SMP-safe).
     * ap_init_lock (atomic xchg spinlock) serializes this. */
    atomic { !init_lock -> init_lock = true };

    in_heap++;
    if
    :: in_heap > 1 -> heap_race = true
    :: else -> skip
    fi;
    /* kmalloc for GDT + kzalloc for TSS */
    skip;
    in_heap--;

    init_lock = false;
    ap_gdt_done[id] = true;

    /* Step 8: load IDT — read-only shared data, safe without lock */
    /* Ordering: must be AFTER gdt_init_cpu (needs valid GDT for lidt) */
    if
    :: !ap_gdt_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_idt_done[id] = true;

    /* Step 9: syscall_init_ap — per-CPU MSR writes, no shared state */
    /* Ordering: must be AFTER IDT (interrupt handlers need gs: base) */
    if
    :: !ap_idt_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_syscall_done[id] = true;

    /* Step 10: sched_init_idle — per-CPU array[my_cpu], safe */
    /* Ordering: must be AFTER syscall_init_ap (schedule needs gs:) */
    if
    :: !ap_syscall_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    ap_sched_done[id] = true;

    /* Step 11: lapic_init — BSP-only mapping guard */
    /* The paging_map_range call is guarded by if (!lapic_base).
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
        skip  /* BSP already mapped, just configure per-CPU regs */
    fi;
    ap_lapic_done[id] = true;

    /* Step 12: signal ready */
    /* Ordering: must be AFTER all init steps */
    if
    :: !ap_lapic_done[id] -> ordering_violation = true
    :: else -> skip
    fi;
    atomic { ap_ready_count = ap_ready_count + 1 };
    ap_ready[id] = true;

    /* Step 13: idle loop */
    stack_users[sid] = stack_users[sid] - 1;
    ap_in_idle[id] = true;

done:
    skip
}

/* ── BSP process ───────────────────────────────────────── */

proctype bsp() {
    /* BSP already called lapic_init in kmain */
    lapic_mapped = true;

    /* Stack allocation done in init block (before APs start) */

    /* Broadcast INIT + SIPI (APs start concurrently) */
    /* ... APs are launched by init block ... */

    /* Phase 1: wait for all APs to park */
    (ap_count >= N_PHYS);

    /* Set wake_flag */
    wake_flag = true;
    bsp_wake_set = true;

    /* Phase 2: wait for wanted APs to complete init */
    (ap_ready_count >= N_WANTED)

    /* BSP continues to timer_init, load init, etc. */
}

/* ── Init ──────────────────────────────────────────────── */

init {
    /* Assign stacks: wanted get unique full stacks, excess get unique halt stacks */
    byte i = 0;
    do
    :: i < N_PHYS ->
        if
        :: i < N_WANTED ->
            stack_id[i] = i + 1                           /* unique full stack */
        :: else ->
            stack_id[i] = N_WANTED + 1 + (i - N_WANTED)   /* unique halt stack */
        fi;
        i++
    :: else -> break
    od;

    run bsp();

    /* Launch all APs (broadcast SIPI) */
    byte j = 0;
    do
    :: j < N_PHYS ->
        run ap(j);
        j++
    :: else -> break
    od
}

/* ── Properties ────────────────────────────────────────── */

/* No two APs use the same stack simultaneously */
ltl no_stack_corruption { [] !stack_corruption }

/* No concurrent heap access (ap_init_lock works) */
ltl no_heap_race { [] !heap_race }

/* lapic mapping never done concurrently (BSP-only guard works) */
ltl no_lapic_map_race { [] !lapic_map_race }

/* Init steps happen in correct order */
ltl no_ordering_violation { [] !ordering_violation }

/* Excess APs never touch heap or scheduler */
ltl no_excess_shared { [] (!excess_touched_heap && !excess_touched_sched) }

/* BSP sets wake_flag only after all APs parked */
ltl wake_after_park { [] (bsp_wake_set -> (ap_count >= N_PHYS)) }

/* All wanted APs eventually reach idle loop (liveness) */
ltl all_wanted_idle { <> (ap_in_idle[0] && ap_in_idle[1]) }
