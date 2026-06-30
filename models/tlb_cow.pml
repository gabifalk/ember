/*
 * TLB safety model for ember Copy-on-Write page fault handler under SMP+BKL.
 *
 * Scenario: after fork(), parent (CPU 0) and child (CPU 1) share a page
 * marked COW read-only. Each has its own PML4 / page tables. The BKL
 * serializes kernel entry so only one COW handler runs at a time. We
 * verify that after both processes COW-fault, they have independent copies
 * and no data corruption occurs.
 *
 * Physical page model:
 *   page 1 = original shared page  (initial data = DATA_ORIG)
 *   page 2 = parent's private copy (after parent COW fault)
 *   page 3 = child's private copy  (after child COW fault)
 *
 * Each process has: pte (physical page number), pte_cow flag, pte_writable flag.
 * TLB caches the pte; tlb_valid tracks whether the cache is fresh.
 *
 * Verify with:
 *   spin -a models/tlb_cow.pml && gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

/* Data values */
#define DATA_ORIG    10
#define DATA_PARENT  20
#define DATA_CHILD   30

/* Physical pages: content */
byte page[4];          /* page[1..3], index 0 unused */

/* Per-process page table entry (index 0=parent, 1=child) */
byte pte_page[2];      /* which physical page the PTE points to */
bool pte_cow[2];       /* PTE has COW bit set */
bool pte_writable[2];  /* PTE is writable */

/* Per-CPU TLB cache */
byte tlb_page[2];      /* cached physical page mapping */
bool tlb_valid[2];     /* TLB entry is valid */

/* BKL: mutual exclusion for kernel entry */
bool bkl = false;

/* Completion flags for sequencing assertions */
bool parent_wrote = false;
bool child_read_done = false;
bool child_wrote = false;

/* Value child observed on read */
byte child_observed;

/*
 * invlpg: flush local TLB (only the faulting CPU's).
 */
inline invlpg(cpu) {
    tlb_valid[cpu] = false;
}

/*
 * tlb_lookup: if TLB is valid, use cached page; else load from PTE.
 * Returns the resolved physical page in 'result'.
 */
inline tlb_resolve(cpu, result) {
    if
    :: tlb_valid[cpu] -> result = tlb_page[cpu]
    :: else ->
        tlb_page[cpu] = pte_page[cpu];
        tlb_valid[cpu] = true;
        result = tlb_page[cpu]
    fi
}

/*
 * write_cr3: full TLB flush (used on context switch / after fork).
 */
inline write_cr3(cpu) {
    tlb_valid[cpu] = false;
}

/*
 * COW fault handler (runs under BKL on the faulting CPU).
 * Mirrors paging_handle_cow(): allocate new page, copy, update PTE, invlpg.
 * 'cpu' is the faulting process index, 'new_pg' is the fresh physical page.
 */
inline cow_handler(cpu, new_pg) {
    byte old_pg;
    old_pg = pte_page[cpu];

    /* Allocate new physical page and copy data */
    page[new_pg] = page[old_pg];

    /* Update PTE: point to new page, clear COW, set writable */
    pte_page[cpu] = new_pg;
    pte_cow[cpu] = false;
    pte_writable[cpu] = true;

    /* invlpg: flush local TLB only */
    invlpg(cpu);
}

/* ---- Parent process (CPU 0) ---- */
proctype Parent() {
    byte resolved;

    /* Parent attempts to write -> page fault (PTE is COW, not writable) */
    tlb_resolve(0, resolved);

    /* Fault: PTE is COW read-only -> enter kernel, acquire BKL */
    atomic {
        !bkl -> bkl = true;
    }

    /* Run COW handler: allocate page 2 for parent */
    cow_handler(0, 2);

    /* Release BKL */
    bkl = false;

    /* Now write to the resolved page (re-resolve after invlpg) */
    tlb_resolve(0, resolved);
    assert(resolved == 2);          /* Must see new private page */
    assert(pte_writable[0]);
    page[resolved] = DATA_PARENT;

    parent_wrote = true;

    /* Original shared page must be untouched */
    assert(page[1] == DATA_ORIG);
}

/* ---- Child process (CPU 1) ---- */
proctype Child() {
    byte resolved;

    /* Wait for parent to finish writing so we can verify isolation */
    parent_wrote;

    /* Child reads via its own TLB/PTE - should still see original page */
    tlb_resolve(1, resolved);
    assert(resolved == 1);          /* Child PTE still points to shared page */
    child_observed = page[resolved];
    assert(child_observed == DATA_ORIG);  /* Parent's write is on page 2, not page 1 */
    child_read_done = true;

    /* Child attempts to write -> page fault (PTE is COW, not writable) */
    atomic {
        !bkl -> bkl = true;
    }

    /* Run COW handler: allocate page 3 for child */
    cow_handler(1, 3);

    /* Release BKL */
    bkl = false;

    /* Write to child's private copy */
    tlb_resolve(1, resolved);
    assert(resolved == 3);          /* Must see child's new private page */
    assert(pte_writable[1]);
    page[resolved] = DATA_CHILD;

    child_wrote = true;
}

init {
    /* Initialize shared physical page */
    page[1] = DATA_ORIG;
    page[2] = 0;
    page[3] = 0;

    /* After fork: both PTEs point to shared page 1, COW read-only */
    pte_page[0] = 1;
    pte_cow[0] = true;
    pte_writable[0] = false;

    pte_page[1] = 1;
    pte_cow[1] = true;
    pte_writable[1] = false;

    /* write_cr3 on parent (flush after fork set up COW PTEs) */
    write_cr3(0);

    /* write_cr3 on child (schedule gives child clean TLB) */
    write_cr3(1);

    /* Launch both CPUs */
    run Parent();
    run Child();

    /* Wait for both to complete */
    parent_wrote && child_wrote;

    /* Final invariants: complete isolation */
    assert(page[1] == DATA_ORIG);    /* Shared page untouched */
    assert(page[2] == DATA_PARENT);  /* Parent's private copy */
    assert(page[3] == DATA_CHILD);   /* Child's private copy */

    /* PTEs are independent */
    assert(pte_page[0] == 2);
    assert(pte_page[1] == 3);
    assert(!pte_cow[0]);
    assert(!pte_cow[1]);
    assert(pte_writable[0]);
    assert(pte_writable[1]);
}
