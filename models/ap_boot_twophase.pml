/*
 * Two-phase AP boot protocol model for ember SMP.
 *
 * Models the SIPI → trampoline → park → wake → init sequence.
 *
 * Phase 1: BSP sends broadcast SIPI.  All N_PHYS APs wake, run
 *   trampoline (mode transition + lock xadd for slot), load stack
 *   from ap_info[slot], park on wake_flag spin loop.
 *
 * Phase 2: BSP polls ap_count until all APs parked, then sets
 *   wake_flag=1.  APs proceed to ap_entry_64 for full init.
 *   Excess APs (my_cpu >= cpu_count) halt.
 *
 * The bug without phase-1 wait: BSP sets wake_flag before all APs
 *   reach the park loop.  Late APs miss wake_flag=1 (they read it
 *   before BSP writes it), never wake, BSP times out.
 *
 * Worse: lapic_icr_wait polls LAPIC MMIO after SIPI.  With many APs
 *   each MMIO read causes a KVM VM exit.  BSP is starved at the poll
 *   and never reaches wake_flag=1.
 *
 * Code references:
 *   smp.c: smp_init() — SIPI, poll ap_count, set wake_flag
 *   ap_trampoline.c: lock xadd ap_count, spin on wake_flag
 *   smp.c: ap_entry_64() — full init after wake
 *
 * Verify:
 *   spin -a models/ap_boot_twophase.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -a -f -m200000
 */

#define N_PHYS     4    /* total physical APs (from broadcast SIPI) */
#define N_WANTED   2    /* APs we want (cpu_count - 1) */

/* Trampoline shared state */
byte ap_count = 0;          /* atomic counter (lock xadd) */
bool wake_flag = false;     /* BSP sets to release parked APs */

/* Per-AP state */
bool ap_parked[N_PHYS];     /* AP reached park loop */
bool ap_running[N_PHYS];    /* AP completed full init */
bool ap_halted[N_PHYS];     /* excess AP halted */

/* BSP state */
bool bsp_icr_stalled = false;  /* BSP stuck in lapic_icr_wait */
bool bsp_set_wake = false;     /* BSP set wake_flag */

/* Outcome tracking */
byte wanted_running = 0;       /* wanted APs that completed init */
bool wanted_stuck = false;     /* a wanted AP never woke from park */

/* Model the LAPIC ICR wait stall.
 * With many APs, each lapic_read causes a VM exit.
 * BSP may be stuck here for a long time. */
bool icr_wait_done = false;

/* ── AP trampoline + park + init ─────────────────────── */
proctype ap(byte id) {
    byte my_slot;
    bool is_wanted = (id < N_WANTED);

    /* Phase 1: trampoline — atomic slot grab */
    atomic {
        my_slot = ap_count;
        ap_count = ap_count + 1
    };

    /* Load stack from ap_info[slot] — all slots valid (model assumes fix) */

    /* Park: spin on wake_flag */
    ap_parked[id] = true;
    (wake_flag == true);    /* block until BSP sets wake_flag */

    /* Phase 2: ap_entry_64 */
    if
    :: !is_wanted ->
        /* Excess AP: my_cpu >= cpu_count, halt */
        ap_halted[id] = true
    :: is_wanted ->
        /* Wanted AP: full init (gdt, idt, lapic, signal ready) */
        ap_running[id] = true;
        wanted_running++
    fi
}

/* ── BSP SIPI + wait + wake ──────────────────────────── */
proctype bsp() {
    /* Send INIT + SIPI (APs start running) */
    /* lapic_send_init_all + lapic_send_sipi_all */

    /* lapic_icr_wait after SIPI — may stall under KVM.
     * Model: BSP is blocked here while APs run trampoline. */
    if
    :: true -> icr_wait_done = true       /* fast: no stall */
    :: true -> bsp_icr_stalled = true;    /* slow: KVM VM exit storm */
               icr_wait_done = true       /* eventually completes */
    fi;

    /* Poll ap_count until all APs parked (phase 1 wait).
     * This is regular memory — no VM exits. */
    (ap_count >= N_PHYS);

    /* Set wake_flag to release APs (phase 2) */
    wake_flag = true;
    bsp_set_wake = true;

    /* Wait for wanted APs to complete init */
    (wanted_running >= N_WANTED)
}

init {
    byte i = 0;
    do
    :: i < N_PHYS ->
        ap_parked[i] = false;
        ap_running[i] = false;
        ap_halted[i] = false;
        i++
    :: else -> break
    od;

    run bsp();

    /* All APs start concurrently (broadcast SIPI) */
    byte j = 0;
    do
    :: j < N_PHYS ->
        run ap(j);
        j++
    :: else -> break
    od
}

/* Liveness: all wanted APs eventually complete init */
ltl all_wanted_boot { <> (wanted_running >= N_WANTED) }

/* Safety: BSP doesn't set wake_flag before all APs parked */
ltl wake_after_park { [] (bsp_set_wake -> (ap_count >= N_PHYS)) }
