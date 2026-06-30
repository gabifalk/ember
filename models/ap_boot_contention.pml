/*
 * AP boot MMIO bus contention model for ember SMP.
 *
 * Models the LAPIC MMIO bus as a serialized shared resource.
 * Each MMIO access (VM exit under KVM) advances a global clock.
 * BSP has a deadline — if the clock exceeds it before BSP finishes
 * smp_init, the BSP times out.
 *
 * BUGGY: excess APs call lapic_id() (MMIO) in ap_entry_64 after wake.
 *   N excess MMIO accesses push the clock past BSP's deadline.
 *
 * FIXED: excess APs halt in trampoline (check slot >= max_aps),
 *   never call ap_entry_64, no MMIO.  Only wanted APs do MMIO.
 *
 * Verify:
 *   spin -a models/ap_boot_contention.pml && \
 *   gcc -O2 -DNFAIR=3 -o pan pan.c && \
 *   ./pan -a -f -m500000 -N bsp_completes
 *
 *   BUGGY (default): acceptance cycle (BSP timeout)
 *   FIXED (-DFIXED): passes (BSP completes)
 */

#define N_WANTED    2
#define N_EXCESS    4
#define N_PHYS      (N_WANTED + N_EXCESS)

/* Global clock: advances on each MMIO access.
 * Models KVM VM exit serialization — only one at a time. */
byte clock = 0;
bool mmio_bus = false;

/* BSP deadline: must finish before clock reaches this */
#define DEADLINE    12

/* Trampoline state */
byte ap_count = 0;
bool wake_flag = false;

/* AP state */
bool ap_parked[N_PHYS];
bool ap_halted[N_PHYS];
byte ap_ready_count = 0;

/* BSP outcome */
bool bsp_done = false;
bool bsp_timed_out = false;

inline MMIO(user) {
    atomic { !mmio_bus -> mmio_bus = true };
    clock = clock + 1;
    mmio_bus = false
}

/* ── Wanted AP ── */
proctype wanted_ap(byte id) {
    byte my_slot;

    atomic { my_slot = ap_count; ap_count = ap_count + 1 };
    ap_parked[id] = true;
    (wake_flag == true);

    /* ap_entry_64: lapic_id (MMIO) */
    MMIO(id);

    /* gdt_init_cpu, sched_init_idle — memory only, no MMIO */
    skip;

    /* lapic_init: per-CPU LAPIC config (MMIO writes) */
    MMIO(id);
    MMIO(id);

    atomic { ap_ready_count = ap_ready_count + 1 }
}

/* ── Excess AP: BUGGY — does MMIO before halting ── */
proctype excess_ap_buggy(byte id) {
    byte my_slot;

    atomic { my_slot = ap_count; ap_count = ap_count + 1 };
    ap_parked[id] = true;
    (wake_flag == true);

    /* ap_entry_64: lapic_id (MMIO) — this is the contention */
    MMIO(id);

    /* check my_cpu >= cpu_count, halt */
    ap_halted[id] = true
}

/* ── Excess AP: FIXED — halts in trampoline, no MMIO ── */
proctype excess_ap_fixed(byte id) {
    byte my_slot;

    atomic { my_slot = ap_count; ap_count = ap_count + 1 };

    /* Trampoline checks slot >= max_aps, halts.
     * No park wait, no ap_entry_64, no MMIO. */
    ap_parked[id] = true;
    ap_halted[id] = true
}

/* ── BSP ── */
proctype bsp_proc() {
    /* INIT broadcast (MMIO) */
    MMIO(99);

    /* SIPI broadcast (MMIO) */
    MMIO(99);

    /* Phase 1: poll ap_count (memory, no MMIO) */
    do
    :: ap_count >= N_PHYS -> break
    :: else ->
        if
        :: clock >= DEADLINE -> bsp_timed_out = true; goto done
        :: else -> skip
        fi
    od;

    /* Set wake_flag */
    wake_flag = true;

    /* Phase 2: poll ap_ready_count (memory, no MMIO) */
    do
    :: ap_ready_count >= N_WANTED -> break
    :: else ->
        if
        :: clock >= DEADLINE -> bsp_timed_out = true; goto done
        :: else -> skip
        fi
    od;

    bsp_done = true;
done:
    skip
}

init {
    byte i;
    run bsp_proc();

    i = 0;
    do
    :: i < N_WANTED -> run wanted_ap(i); i++
    :: else -> break
    od;

    i = N_WANTED;
    do
    :: i < N_PHYS ->
#ifdef FIXED
        run excess_ap_fixed(i);
#else
        run excess_ap_buggy(i);
#endif
        i++
    :: else -> break
    od
}

/* BSP completes without timeout */
ltl bsp_completes { <> bsp_done }

/* If timeout, clock was exhausted */
ltl timeout_means_overloaded { [] (bsp_timed_out -> (clock >= DEADLINE)) }
