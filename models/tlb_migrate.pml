/*
 * TLB Safety During Process Migration Under BKL
 *
 * Verifies that when a process migrates between CPUs, no CPU ever
 * accesses memory through stale TLB entries for that process.
 *
 * Model:
 *   - 2 CPUs (cpu0, cpu1), 2 processes (A=0, B=1)
 *   - Process A starts on CPU 0, migrates to CPU 1
 *   - Each CPU tracks which process's TLB entries it holds and
 *     whether those entries are valid (fresh after write_cr3)
 *   - schedule() performs write_cr3 which flushes the entire TLB
 *     on the local CPU, then loads the new process's page table
 *   - Safety property: whenever a CPU accesses pages belonging to
 *     the process it is running, the TLB is valid for that process
 *
 * The BKL serializes all schedule() calls, so only one CPU can be
 * in schedule() at a time.  In the real kernel, schedule() runs
 * with interrupts disabled on the local CPU and holds the BKL,
 * so the write_cr3 + running update is effectively atomic from
 * the local CPU's perspective (no local preemption can observe a
 * half-done context switch).  We model this atomicity.
 */

#define NCPU   2
#define NPROC  2

#define PROC_A 0
#define PROC_B 1

#define NONE   255   /* no process assigned */

/* Per-CPU state */
byte running[NCPU]   = NONE;   /* which process is running on this CPU */
byte tlb_owner[NCPU] = NONE;   /* which process's TLB entries are loaded */
bool tlb_valid[NCPU] = false;  /* are the TLB entries fresh? */

/* BKL: only one CPU in schedule() at a time */
bool bkl = false;

/* Track whether process A has migrated off CPU 0 */
bool a_migrated = false;

/*
 * schedule(cpu, next):
 *   Acquire BKL → write_cr3 (flush TLB, load new PML4) → update
 *   running → release BKL.
 *
 *   The BKL acquire blocks until available.  The body is atomic
 *   because the real kernel holds BKL + cli during the switch,
 *   so no local access can see a partial state.
 */
inline schedule(cpu, next) {
    atomic {
        !bkl -> bkl = true;
        /* write_cr3: full TLB flush + load new page tables */
        tlb_owner[cpu] = next;
        tlb_valid[cpu] = true;
        running[cpu]   = next;
    };
    /* Release BKL (can interleave with other CPUs now) */
    bkl = false;
}

/*
 * access_memory(cpu):
 *   Simulate a memory access on a CPU.  Assert that the TLB is
 *   valid and owned by the currently running process.
 */
inline access_memory(cpu) {
    assert(running[cpu] != NONE);
    assert(tlb_valid[cpu] == true);
    assert(tlb_owner[cpu] == running[cpu]);
}

/*
 * CPU 0 behavior:
 *   1. Schedule process A, do memory accesses
 *   2. Timer preempts A → schedule switches to B (write_cr3 flushes)
 *   3. Do memory accesses for B
 *   4. Signal that A is available for migration
 *   5. Optionally pick A back up later
 */
active proctype cpu0() {
    /* Boot: CPU 0 schedules process A */
    schedule(0, PROC_A);
    access_memory(0);
    access_memory(0);

    /* Timer fires → preempt A, switch to B */
    schedule(0, PROC_B);
    /* CPU 0 TLB flushed by write_cr3(B). Stale A entries gone. */
    access_memory(0);
    access_memory(0);

    /* A is off CPU 0, available for migration */
    a_migrated = true;

    /* CPU 0 continues running B */
    access_memory(0);

    /* Optionally switch back to A */
    if
    :: true ->
        schedule(0, PROC_A);
        access_memory(0);
    :: true -> skip
    fi
}

/*
 * CPU 1 behavior:
 *   1. Schedule process B initially
 *   2. Wait for A to be descheduled from CPU 0
 *   3. Pick up A via schedule (write_cr3 flushes TLB)
 *   4. Memory accesses for A — TLB is fresh
 *   5. Optionally switch back to B, then A again
 */
active proctype cpu1() {
    /* CPU 1 starts with B */
    schedule(1, PROC_B);
    access_memory(1);

    /* Wait until A is migrated off CPU 0 */
    a_migrated;

    /* Pick up process A */
    schedule(1, PROC_A);
    /* write_cr3(A) flushed TLB → fresh A entries */
    access_memory(1);
    access_memory(1);
    access_memory(1);

    /* Switch back to B */
    schedule(1, PROC_B);
    access_memory(1);

    /* Optionally pick up A again */
    if
    :: true ->
        schedule(1, PROC_A);
        access_memory(1);
    :: true -> skip
    fi
}
