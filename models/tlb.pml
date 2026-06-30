/*
 * TLB coherency model for ember SMP.
 *
 * Models the COW fork scenario:
 *   1. Parent has writable page P (TLB entry: writable)
 *   2. Fork: PTE for P changed to read-only COW (both parent + child)
 *   3. Parent returns to userspace — TLB still has writable entry!
 *   4. Parent writes to P — no page fault (stale TLB) — writes to
 *      shared physical page — child sees corrupted data
 *
 * TLB is flushed on:
 *   - write_cr3() in schedule() context_switch
 *   - invlpg for single page
 *   - explicit CR3 reload
 *
 * The question: after fork changes PTEs, does the parent's TLB
 * get flushed before returning to userspace?
 *
 * Also models COW fault resolution:
 *   - Process A faults on COW page → handler copies page, makes
 *     A's PTE writable. B's PTE stays read-only.
 *   - If B's TLB has the old (pre-COW) writable entry → stale
 *     But B was never scheduled with that PTE (fork created B
 *     with COW PTEs, schedule does write_cr3 flushing TLB).
 *     So B's TLB is clean. Only the parent's TLB is stale.
 */

#define P_RUNNING  2

/* Page state */
#define PAGE_WRITABLE   1
#define PAGE_COW_RO     2  /* read-only, COW bit set */

/* Physical pages */
#define PHYS_SHARED   1    /* original shared page */
#define PHYS_COPY_A   2    /* COW copy for proc A */

/* Per-process PTE (what the page table says) */
byte pte[2];           /* pte[proc] = PAGE_WRITABLE or PAGE_COW_RO */
byte pte_phys[2];      /* pte_phys[proc] = which physical page */

/* Per-CPU TLB cache (what the CPU thinks the PTE is) */
byte tlb_perm[2];      /* tlb_perm[cpu] = cached permission */
byte tlb_phys[2];      /* tlb_phys[cpu] = cached physical page */
bool tlb_valid[2];     /* tlb_valid[cpu] = TLB entry present */

/* Data on physical pages (to detect corruption) */
byte page_data[3];     /* page_data[phys_page] */

bool data_corrupted = false;

/* ── TLB operations ──────────────────────────────────── */

/* TLB flush: invalidate all entries on this CPU.
 * Happens on write_cr3 (schedule context_switch). */
inline TLB_FLUSH(cpu) {
    tlb_valid[cpu] = false
}

/* TLB fill: on memory access, if TLB miss, load from PTE.
 * On TLB hit, use cached value. */
inline TLB_LOOKUP(cpu, proc) {
    if
    :: tlb_valid[cpu] -> skip  /* hit: use cached */
    :: !tlb_valid[cpu] ->
        /* miss: load from PTE */
        tlb_perm[cpu] = pte[proc];
        tlb_phys[cpu] = pte_phys[proc];
        tlb_valid[cpu] = true
    fi
}

/* ── Memory write via TLB ────────────────────────────── */
inline MEM_WRITE(cpu, proc, value) {
    TLB_LOOKUP(cpu, proc);
    if
    :: tlb_perm[cpu] == PAGE_WRITABLE ->
        /* TLB says writable — write goes through */
        page_data[tlb_phys[cpu]] = value
    :: tlb_perm[cpu] == PAGE_COW_RO ->
        /* TLB says read-only COW — page fault.
         * Handled under BKL in kernel. */
        skip  /* fault handled separately */
    fi
}

/* ═══════════════════════════════════════════════════════
 * Scenario: fork then parent writes
 *
 * BUGGY: fork changes PTE but doesn't flush parent's TLB
 * FIXED: fork reloads CR3 (flushes TLB) after PTE changes
 * ═══════════════════════════════════════════════════════ */

proctype scenario_fork_write() {
    /* Initial state: proc 0 (parent) has writable page */
    atomic {
        pte[0] = PAGE_WRITABLE;
        pte_phys[0] = PHYS_SHARED;
        page_data[PHYS_SHARED] = 42;

        /* Parent's TLB has writable entry (from before fork) */
        tlb_perm[0] = PAGE_WRITABLE;
        tlb_phys[0] = PHYS_SHARED;
        tlb_valid[0] = true
    };

    /* Fork: change both PTEs to COW read-only (under BKL) */
    atomic {
        pte[0] = PAGE_COW_RO;
        pte_phys[0] = PHYS_SHARED;
        pte[1] = PAGE_COW_RO;
        pte_phys[1] = PHYS_SHARED
    };

    /* FIXED: flush parent's TLB after fork changes PTEs */
    TLB_FLUSH(0);

    /* Child scheduled on CPU 1 — schedule does write_cr3 = TLB flush */
    TLB_FLUSH(1);
    tlb_valid[1] = false;

    /* Parent returns to userspace on CPU 0.
     * Parent's TLB still has: writable, PHYS_SHARED */

    /* Parent writes to the page */
    MEM_WRITE(0, 0, 99);

    /* Child reads the page (TLB miss, loads COW PTE) */
    TLB_LOOKUP(1, 1);

    /* Check: did parent's write go to the SHARED page?
     * If parent's TLB was stale (writable), the write went to
     * PHYS_SHARED directly. Child reads PHYS_SHARED and sees 99
     * instead of 42. DATA CORRUPTION. */
    if
    :: page_data[PHYS_SHARED] != 42 ->
        /* Parent wrote to shared page via stale TLB! */
        data_corrupted = true
    :: else -> skip
    fi;

    /* Assert: shared page must not be modified */
    assert(!data_corrupted)
}

init {
    page_data[0] = 0;
    page_data[1] = 0;
    page_data[2] = 0;
    tlb_valid[0] = false;
    tlb_valid[1] = false;
    run scenario_fork_write()
}
