/*
 * TLB race model for MAP_FIXED mmap under ember SMP.
 *
 * Models the bug: do_mmap MAP_FIXED unmaps old pages and frees them
 * with only a local invlpg — no IPI shootdown to sibling CPUs sharing
 * the same PML4 (CLONE_VM threads).  A sibling CPU can write through
 * a stale TLB entry to the freed page after it has been reallocated
 * to another process, causing data corruption.
 *
 * Contrast with do_munmap which correctly calls smp_flush_tlb() before
 * pmm_free_page() when pml4_is_shared().
 *
 * Proposed fix: in do_mmap MAP_FIXED, call smp_flush_tlb() after
 * unmapping when pml4_is_shared(), before freeing pages.
 *
 * Actors:
 *   cpu0_mmap_fixed — CPU0 runs mmap(MAP_FIXED) under BKL
 *   cpu1_user_write — CPU1 runs sibling thread in user mode, same PML4
 *   reallocator     — another process allocates the freed page
 *
 * Properties:
 *   P1: no data corruption (sibling never writes to reallocated page)
 *
 * Verify (fixed — with IPI):
 *   spin -a models/mmap_fixed_tlb.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 *
 * Bug injection (current code — no IPI):
 *   spin -a -DBUGGY_NO_IPI models/mmap_fixed_tlb.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

/*
 * Physical page ownership.
 * PA 1 = original page mapped at the target VA.
 * PA 2 = new page that mmap(MAP_FIXED) maps in its place.
 */
#define OWNER_PROC   1   /* Owned by the process doing mmap */
#define OWNER_FREE   0   /* In PMM free pool */
#define OWNER_OTHER  2   /* Reallocated to another process */

byte owner[3];        /* owner[pa] — who owns physical page pa */
byte page_data[3];    /* page_data[pa] — data content */

/* Page table entry for the target VA (shared PML4). */
byte pte_phys = 1;    /* Physical page mapped at the VA */
bool pte_valid = true;

/* Per-CPU TLB entry for the target VA. */
byte tlb_phys[2];
bool tlb_valid[2];

/* Synchronization / state. */
bool bkl = false;
bool mmap_done = false;
bool corruption = false;

inline invlpg_local(cpu) {
    tlb_valid[cpu] = false
}

inline tlb_shootdown_ipi(remote_cpu) {
    tlb_valid[remote_cpu] = false
}

/*
 * TLB resolution: if TLB valid, use cached PA; otherwise refill from PTE.
 * Returns 0 if PTE is invalid (page fault).
 */
inline tlb_resolve(cpu, result) {
    if
    :: tlb_valid[cpu] ->
        result = tlb_phys[cpu]
    :: !tlb_valid[cpu] ->
        if
        :: pte_valid ->
            tlb_phys[cpu] = pte_phys;
            tlb_valid[cpu] = true;
            result = pte_phys
        :: !pte_valid ->
            result = 0   /* page fault */
        fi
    fi
}

/*
 * CPU0: mmap(MAP_FIXED) under BKL.
 *
 * Current buggy code:
 *   paging_unmap_page → pmm_free_page → invlpg (local only)
 *
 * Fixed code:
 *   paging_unmap_page → invlpg (local) → smp_flush_tlb (IPI) → pmm_free_page
 */
proctype cpu0_mmap_fixed() {
    atomic { !bkl -> bkl = true };

    /* Step 1: paging_unmap_page — clear PTE. */
    byte old_pa = pte_phys;
    pte_valid = false;
    pte_phys = 0;

    /* Step 2: invlpg (local). */
    invlpg_local(0);

#ifndef BUGGY_NO_IPI
    /* Step 3 (fix): IPI shootdown to flush remote TLBs. */
    tlb_shootdown_ipi(1);
#endif

    /* Step 4: pmm_free_page — free the old page. */
    owner[old_pa] = OWNER_FREE;

    /* Step 5: allocate + map new page. */
    pte_phys = 2;
    pte_valid = true;
    owner[2] = OWNER_PROC;
    page_data[2] = 50;

    bkl = false;
    mmap_done = true
}

/*
 * Reallocator: another process grabs the freed page from PMM.
 * This can happen as soon as the page is freed, before the IPI
 * reaches CPU1.
 */
proctype reallocator() {
    (owner[1] == OWNER_FREE);
    owner[1] = OWNER_OTHER;
    page_data[1] = 77
}

/*
 * CPU1: sibling thread (same PML4, CLONE_VM) in user mode.
 * Tries to write to the same VA after mmap(MAP_FIXED) runs.
 *
 * If TLB still has the stale entry pointing to old PA (now freed
 * and reallocated), the write corrupts the other process's data.
 */
proctype cpu1_user_write() {
    byte resolved;

    /* Wait for mmap to complete and page to be reallocated. */
    mmap_done;

    /* Try to access the VA. */
    tlb_resolve(1, resolved);

    if
    :: (resolved != 0) ->
        if
        :: (owner[resolved] == OWNER_OTHER) ->
            /* Writing to another process's page! Corruption! */
            corruption = true
        :: (owner[resolved] == OWNER_FREE) ->
            /* Writing to freed page! Corruption! */
            corruption = true
        :: (owner[resolved] == OWNER_PROC) ->
            /* Writing to our own new page — fine. */
            skip
        fi
    :: (resolved == 0) ->
        skip   /* Page fault — correct, will refault to new page. */
    fi;

    assert(!corruption)
}

init {
    /* PA 1: original page, owned by our process. */
    owner[1] = OWNER_PROC;
    page_data[1] = 42;

    /* PA 2: not yet allocated. */
    owner[2] = OWNER_FREE;
    page_data[2] = 0;

    /* PTE maps VA → PA 1. */
    pte_phys = 1;
    pte_valid = true;

    /* Both CPUs have TLB entries for VA → PA 1. */
    tlb_phys[0] = 1;
    tlb_valid[0] = true;
    tlb_phys[1] = 1;
    tlb_valid[1] = true;

    run cpu0_mmap_fixed();
    run reallocator();
    run cpu1_user_write()
}
