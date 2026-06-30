/*
 * TLB safety model for mprotect/munmap under SMP with BKL.
 *
 * Scenario:
 *   - 2 CPUs, 2 processes (proc 0 and proc 1) with different pml4s.
 *   - CPU 0 holds the BKL, runs mprotect on proc 0's PTE, does invlpg locally.
 *   - CPU 1 runs proc 1 in userspace (different address space, no conflict).
 *   - CPU 1 later enters kernel, acquires BKL, schedule() switches to proc 0,
 *     which does write_cr3 (full TLB flush on CPU 1).
 *   - After the switch, CPU 1 accesses proc 0's page and must see the NEW
 *     permission (the mprotect result), not a stale TLB entry.
 *
 * Property verified:
 *   invlpg on the modifying CPU + write_cr3 on the switching CPU is sufficient.
 *   No cross-CPU TLB shootdown IPI is needed under BKL.
 */

/* ---------- shared page-table state ---------- */

/* Permission stored in the actual PTE (ground truth). */
/* 0 = read-only (initial), 1 = read-write (after mprotect) */
byte pte_perm = 0;

/* ---------- per-CPU TLB model ---------- */

/* Each CPU's TLB caches the permission for proc 0's page.
 * Value 255 means "TLB entry invalid / not cached".
 * Otherwise it holds the cached permission value.
 */
byte tlb_cpu0 = 255;
byte tlb_cpu1 = 255;

/* ---------- per-CPU CR3 (which pml4 is loaded) ---------- */
/* 0 = proc 0's pml4, 1 = proc 1's pml4 */
byte cr3_cpu0 = 0;   /* CPU 0 runs proc 0 initially */
byte cr3_cpu1 = 1;   /* CPU 1 runs proc 1 initially */

/* ---------- BKL ---------- */
bool bkl_held = false;

/* ---------- progress flags ---------- */
bool mprotect_done = false;
bool cpu1_switched = false;

/* ---------- CPU 0: does mprotect on proc 0 ---------- */
proctype cpu0_mprotect()
{
    /* Acquire BKL (CPU 0 enters kernel) */
    atomic {
        !bkl_held -> bkl_held = true;
    };

    /* --- mprotect syscall on proc 0 --- */

    /* Step 1: Modify the PTE (change permission to read-write) */
    pte_perm = 1;

    /* Step 2: invlpg on local CPU 0 — flush our own TLB entry */
    tlb_cpu0 = 255;

    /* Mark mprotect as complete */
    mprotect_done = true;

    /* Release BKL (return to userspace) */
    bkl_held = false;
}

/* ---------- CPU 1: runs proc 1 in userspace, then switches to proc 0 --- */
proctype cpu1_switch()
{
    /* CPU 1 is running proc 1 in userspace.
     * It might have a stale TLB entry for proc 0's page from a previous
     * run — but since cr3_cpu1 == 1 (proc 1's pml4), those entries are
     * for a DIFFERENT address space.  Model this by allowing CPU 1 to
     * have cached something for proc 0 previously (worst case).
     *
     * Simulate: CPU 1 had proc 0's page cached with OLD permission.
     */
    tlb_cpu1 = 0;  /* stale: old read-only permission */

    /* Wait until mprotect is done (CPU 0 released BKL) */
    mprotect_done;

    /* CPU 1 enters kernel (e.g., timer interrupt) — acquire BKL */
    atomic {
        !bkl_held -> bkl_held = true;
    };

    /* --- schedule() decides to switch CPU 1 to proc 0 --- */

    /* schedule() does write_cr3(proc0->pml4) which flushes entire TLB */
    cr3_cpu1 = 0;      /* now running proc 0's address space */
    tlb_cpu1 = 255;    /* write_cr3 flushes all TLB entries on this CPU */

    /* Release BKL (return to userspace as proc 0) */
    bkl_held = false;

    /* Mark switch as done */
    cpu1_switched = true;
}

/* ---------- CPU 1 accesses proc 0's page after the switch ---------- */
proctype cpu1_access()
{
    byte observed;

    /* Wait until CPU 1 has switched to proc 0 */
    cpu1_switched;

    /* TLB lookup on CPU 1: if TLB is invalid, load from PTE */
    if
    :: (tlb_cpu1 == 255) ->
        /* TLB miss: load from actual PTE (ground truth) */
        tlb_cpu1 = pte_perm;
        observed = pte_perm;
    :: (tlb_cpu1 != 255) ->
        /* TLB hit: use cached value */
        observed = tlb_cpu1;
    fi;

    /* SAFETY ASSERTION:
     * The observed permission MUST be the new permission (1 = read-write).
     * If this fails, it means CPU 1 saw a stale TLB entry — a bug that
     * would require an IPI-based TLB shootdown to fix.
     */
    assert(observed == 1);
}

init
{
    atomic {
        run cpu0_mprotect();
        run cpu1_switch();
        run cpu1_access();
    }
}
