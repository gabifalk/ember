/*
 * Fork COW TLB model for ember SMP (multi-page).
 *
 * Models paging_clone_user_pml4() (paging.c:76-141):
 *   for each page: mark COW read-only in parent + child, bump refcount
 *   write_cr3(read_cr3())  — local TLB flush (paging.c:138)
 *   smp_flush_tlb()        — IPI shootdown to all other CPUs (paging.c:139)
 *
 * CPU1 runs the parent in user mode with stale TLB entries from
 * before fork.  The IPI shootdown invalidates CPU1's TLB so that
 * post-fork writes trigger COW page faults instead of corrupting
 * shared pages.
 *
 * Verify:
 *   spin -a models/fork_cow_tlb_multipage.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define NCPU       2
#define NPAGE      2

#define DATA_ORIG_0     10
#define DATA_ORIG_1     20
#define DATA_STALE      99

#define PERM_WRITABLE   1
#define PERM_COW_RO     2

#define PHYS_PAGE0  1
#define PHYS_PAGE1  2

byte page_data[3];
byte pte_perm[2 * NPAGE];
byte pte_phys[2 * NPAGE];
byte tlb_perm[NCPU * NPAGE];
byte tlb_phys[NCPU * NPAGE];
bool tlb_valid[NCPU * NPAGE];
byte refcount[3];

bool bkl = false;
bool fork_done = false;
bool corruption_detected = false;

inline tlb_flush_all(cpu) {
    byte flpg = 0;
    do
    :: flpg < NPAGE ->
        tlb_valid[cpu * NPAGE + flpg] = false;
        flpg++
    :: else -> break
    od
}

inline tlb_resolve(cpu, proc, pg, out_perm, out_phys) {
    if
    :: tlb_valid[cpu * NPAGE + pg] ->
        out_perm = tlb_perm[cpu * NPAGE + pg];
        out_phys = tlb_phys[cpu * NPAGE + pg]
    :: !tlb_valid[cpu * NPAGE + pg] ->
        tlb_perm[cpu * NPAGE + pg] = pte_perm[proc * NPAGE + pg];
        tlb_phys[cpu * NPAGE + pg] = pte_phys[proc * NPAGE + pg];
        tlb_valid[cpu * NPAGE + pg] = true;
        out_perm = pte_perm[proc * NPAGE + pg];
        out_phys = pte_phys[proc * NPAGE + pg]
    fi
}

/* CPU0: fork (BKL held).  Models paging_clone_user_pml4(). */
proctype cpu0_fork() {
    byte pg;
    atomic { !bkl -> bkl = true };

    pg = 0;
    do
    :: pg < NPAGE ->
        pte_perm[0 * NPAGE + pg] = PERM_COW_RO;
        pte_perm[1 * NPAGE + pg] = PERM_COW_RO;
        pte_phys[1 * NPAGE + pg] = pte_phys[0 * NPAGE + pg];
        refcount[pte_phys[0 * NPAGE + pg]] =
            refcount[pte_phys[0 * NPAGE + pg]] + 1;
        pg++
    :: pg >= NPAGE -> break
    od;

    /* paging.c:138 — local TLB flush */
    tlb_flush_all(0);
    /* paging.c:139 — smp_flush_tlb() IPI shootdown */
    tlb_flush_all(1);

    bkl = false;
    fork_done = true
}

/* CPU1: parent in user mode, writes after fork returns. */
proctype cpu1_user_writes() {
    byte perm, phys;
    fork_done;

    if
    :: true ->
        tlb_resolve(1, 0, 0, perm, phys);
        if
        :: perm == PERM_WRITABLE -> page_data[phys] = DATA_STALE
        :: perm == PERM_COW_RO -> skip
        fi
    :: true ->
        tlb_resolve(1, 0, 1, perm, phys);
        if
        :: perm == PERM_WRITABLE -> page_data[phys] = DATA_STALE
        :: perm == PERM_COW_RO -> skip
        fi
    :: true ->
        tlb_resolve(1, 0, 0, perm, phys);
        if
        :: perm == PERM_WRITABLE -> page_data[phys] = DATA_STALE
        :: perm == PERM_COW_RO -> skip
        fi;
        tlb_resolve(1, 0, 1, perm, phys);
        if
        :: perm == PERM_WRITABLE -> page_data[phys] = DATA_STALE
        :: perm == PERM_COW_RO -> skip
        fi
    fi
}

proctype checker() {
    fork_done;
    if
    :: page_data[PHYS_PAGE0] != DATA_ORIG_0 -> corruption_detected = true
    :: page_data[PHYS_PAGE1] != DATA_ORIG_1 -> corruption_detected = true
    :: else -> skip
    fi;
    assert(!corruption_detected)
}

#define p (fork_done)
#define q (page_data[PHYS_PAGE0] == DATA_ORIG_0 && page_data[PHYS_PAGE1] == DATA_ORIG_1)
ltl no_corruption { [] (p -> q) }

init {
    page_data[PHYS_PAGE0] = DATA_ORIG_0;
    page_data[PHYS_PAGE1] = DATA_ORIG_1;

    pte_perm[0 * NPAGE + 0] = PERM_WRITABLE;
    pte_phys[0 * NPAGE + 0] = PHYS_PAGE0;
    pte_perm[0 * NPAGE + 1] = PERM_WRITABLE;
    pte_phys[0 * NPAGE + 1] = PHYS_PAGE1;

    pte_perm[1 * NPAGE + 0] = 0;
    pte_phys[1 * NPAGE + 0] = 0;
    pte_perm[1 * NPAGE + 1] = 0;
    pte_phys[1 * NPAGE + 1] = 0;

    refcount[PHYS_PAGE0] = 1;
    refcount[PHYS_PAGE1] = 1;

    tlb_flush_all(0);

    tlb_perm[1 * NPAGE + 0] = PERM_WRITABLE;
    tlb_phys[1 * NPAGE + 0] = PHYS_PAGE0;
    tlb_valid[1 * NPAGE + 0] = true;
    tlb_perm[1 * NPAGE + 1] = PERM_WRITABLE;
    tlb_phys[1 * NPAGE + 1] = PHYS_PAGE1;
    tlb_valid[1 * NPAGE + 1] = true;

    run cpu0_fork();
    run cpu1_user_writes();
    run checker()
}
