/*
 * AP boot slot assignment race model for ember SMP.
 *
 * The trampoline assigns slots via atomic counter (lock xadd).
 * Slot assignment is non-deterministic: any physical AP can get
 * any slot.  If we filter by slot (max_aps check in trampoline),
 * wanted APs can get high slots and be incorrectly halted.
 *
 * This model explores three strategies:
 *
 * Strategy A (SLOT_FILTER): halt in trampoline if slot >= max_aps.
 *   Bug: wanted AP gets slot >= max_aps, halts incorrectly.
 *
 * Strategy B (CPUID_FILTER): all APs get stacks, halt in ap_entry_64
 *   after checking LAPIC ID / cpu_id.  No MMIO contention issue if
 *   excess APs halt quickly (only one lapic_id MMIO read each).
 *
 * Strategy C (TWO_PASS): trampoline does NOT filter.  All APs park.
 *   BSP sets wake_flag.  ap_entry_64 filters by LAPIC ID.  But all
 *   APs need valid stacks.
 *
 * Verify:
 *   spin -DSLOT_FILTER -a models/ap_boot_slot_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000 -N all_wanted_boot
 *
 *   spin -DCPUID_FILTER -a models/ap_boot_slot_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000 -N all_wanted_boot
 */

#define N_WANTED   2
#define N_EXCESS   3
#define N_PHYS     (N_WANTED + N_EXCESS)

byte ap_count = 0;
bool wake_flag = false;

/* Per-AP: did this AP complete init? */
bool ap_booted[N_PHYS];
byte booted_count = 0;

/* Error tracking */
bool wanted_halted = false;    /* wanted AP incorrectly halted */

/* ── Strategy A: slot-based filter in trampoline ── */
#ifdef SLOT_FILTER

proctype ap(byte id) {
    byte my_slot;
    bool wanted = (id < N_WANTED);

    atomic { my_slot = ap_count; ap_count = ap_count + 1 };

    /* Trampoline max_aps check: slot >= N_WANTED → halt */
    if
    :: my_slot >= N_WANTED ->
        if
        :: wanted -> wanted_halted = true  /* BUG: wanted AP killed */
        :: else -> skip
        fi
    :: my_slot < N_WANTED ->
        /* Proceed to park + init */
        (wake_flag == true);
        if
        :: wanted ->
            ap_booted[id] = true;
            atomic { booted_count = booted_count + 1 }
        :: !wanted ->
            skip  /* excess AP with low slot — does full init unnecessarily */
        fi
    fi
}

#endif

/* ── Strategy B: filter by cpu_id in ap_entry_64 ── */
#ifdef CPUID_FILTER

proctype ap(byte id) {
    byte my_slot;
    bool wanted = (id < N_WANTED);

    atomic { my_slot = ap_count; ap_count = ap_count + 1 };

    /* All APs get valid stacks, park, wake */
    (wake_flag == true);

    /* ap_entry_64: check LAPIC ID → cpu_id.
     * wanted = cpu_id < cpu_count.
     * Each AP does ONE MMIO read (lapic_id), then excess halt. */
    if
    :: wanted ->
        ap_booted[id] = true;
        atomic { booted_count = booted_count + 1 }
    :: !wanted ->
        skip  /* halt */
    fi
}

#endif

/* ── Strategy C: no filter (default) — same as B but explicit ── */
#if !defined(SLOT_FILTER) && !defined(CPUID_FILTER)

proctype ap(byte id) {
    byte my_slot;
    bool wanted = (id < N_WANTED);

    atomic { my_slot = ap_count; ap_count = ap_count + 1 };
    (wake_flag == true);

    if
    :: wanted ->
        ap_booted[id] = true;
        atomic { booted_count = booted_count + 1 }
    :: !wanted -> skip
    fi
}

#endif

proctype bsp() {
    (ap_count >= N_PHYS);
    wake_flag = true;
    (booted_count >= N_WANTED)
}

init {
    run bsp();
    byte j = 0;
    do
    :: j < N_PHYS -> run ap(j); j++
    :: else -> break
    od
}

/* All wanted APs must boot */
ltl all_wanted_boot { <> (booted_count >= N_WANTED) }

/* No wanted AP incorrectly halted */
ltl no_wanted_halted { [] !wanted_halted }
