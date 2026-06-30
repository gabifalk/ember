/*
 * AP boot trampoline slot overflow model for ember SMP.
 *
 * Models broadcast SIPI + atomic slot counter + stack allocation:
 *   - ACPI reports TOTAL_APS physical APs
 *   - Kernel allocates WANTED_APS full stacks (32KB each)
 *   - Excess APs get halt stacks (1 page each, or shared)
 *   - Broadcast SIPI wakes ALL TOTAL_APS
 *   - Each AP does lock xadd for a slot, loads stack, runs ap_entry_64
 *
 * Bugs found:
 *   1. ap_info sized MAX_CPUS — excess slots read garbage (overflow)
 *   2. Non-deterministic slot assignment — wanted AP can get excess slot
 *   3. Shared halt stack — N excess APs overflow one 4KB page
 *
 * Fix requirements (all verified below):
 *   - ap_info sized for all physical APs
 *   - Every slot has a stack (wanted: 32KB, excess: 1 page each)
 *   - No two APs share a stack page concurrently
 *
 * Verify:
 *   spin -a models/ap_boot_overflow.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define TOTAL_APS   5    /* physical APs woken by broadcast SIPI */
#define WANTED_APS  2    /* APs we allocated full stacks for */
#define MAX_SLOTS   5    /* ap_info array size (must be >= TOTAL_APS) */

/* Stack allocation: each slot has a stack_id.
 * Wanted slots (0..WANTED_APS-1): unique stack_id 1..WANTED_APS
 * Excess slots: each gets its own stack_id (WANTED_APS+1..) if fixed,
 *               or all share stack_id 99 if buggy (shared halt stack). */
byte stack_id[MAX_SLOTS];

/* Track concurrent users per stack_id.
 * If two APs use the same stack_id simultaneously, stack corrupts. */
byte stack_users[TOTAL_APS + WANTED_APS + 1];  /* indexed by stack_id */

/* Atomic slot counter */
byte ap_count = 0;

/* Per-AP state */
bool ap_in_init[TOTAL_APS];   /* AP is executing ap_entry_64 (using stack) */

/* Detect failures */
bool stack_corruption = false;   /* two APs on same stack simultaneously */
bool wanted_ap_got_garbage = false;
bool overflow_detected = false;

proctype ap_boot(byte id; bool wanted) {
    byte my_slot;

    /* Atomic fetch-and-add (trampoline lock xadd) */
    atomic {
        my_slot = ap_count;
        ap_count = ap_count + 1
    };

    /* Bounds check */
    if
    :: my_slot >= MAX_SLOTS ->
        overflow_detected = true
    :: else ->
        /* Load stack from ap_info[slot] */
        byte sid = stack_id[my_slot];
        if
        :: sid == 0 ->
            /* No stack assigned — garbage */
            if
            :: wanted -> wanted_ap_got_garbage = true
            :: else -> skip
            fi
        :: else ->
            /* Enter ap_entry_64 — using this stack */
            ap_in_init[id] = true;
            stack_users[sid] = stack_users[sid] + 1;

            /* Check for concurrent stack usage */
            if
            :: stack_users[sid] > 1 -> stack_corruption = true
            :: else -> skip
            fi;

            /* AP does init work (or halts if excess) */
            if
            :: wanted -> skip   /* full init: gdt, idt, lapic, etc. */
            :: !wanted -> skip  /* check my_cpu >= cpu_count, halt */
            fi;

            stack_users[sid] = stack_users[sid] - 1;
            ap_in_init[id] = false
        fi
    fi
}

init {
    /* Assign stacks: wanted slots get unique stacks,
     * excess slots get individual stacks (one page each). */
    byte i = 0;
    do
    :: i < MAX_SLOTS ->
        if
        :: i < WANTED_APS ->
            stack_id[i] = i + 1              /* unique full stack */
        :: else ->
            stack_id[i] = WANTED_APS + 1 + (i - WANTED_APS)  /* unique halt stack */
        fi;
        i++
    :: else -> break
    od;

    /* Launch all APs */
    byte j = 0;
    do
    :: j < TOTAL_APS ->
        if
        :: j < WANTED_APS -> run ap_boot(j, true)
        :: else -> run ap_boot(j, false)
        fi;
        j++
    :: else -> break
    od
}

/* Safety: no stack corruption (no two APs on same stack) */
ltl no_stack_corruption { [] !stack_corruption }

/* Safety: no wanted AP gets garbage */
ltl no_wanted_garbage { [] !wanted_ap_got_garbage }

/* Safety: no slot overflow */
ltl no_overflow { [] !overflow_detected }
