/*
 * TLB model for munmap under ember SMP.
 *
 * Models do_munmap with the IPI shootdown fix:
 *   paging_unmap_page, invlpg (local), smp_flush_tlb() (IPI), pmm_free_page
 *
 * CPU1 runs the same process in user mode.  The IPI shootdown
 * invalidates CPU1's TLB so stale entries can't be used to write
 * to freed/reallocated pages.
 *
 * See also: mmap_fixed_tlb.pml for the MAP_FIXED-specific model.
 *
 * Verify:
 *   spin -a models/munmap_tlb_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define OWNER_PROC_P   1
#define OWNER_FREE     0
#define OWNER_OTHER    2

#define PA_ORIG        1
#define PA_NEW         2

#define DATA_PROC_P   42
#define DATA_OTHER    77
#define DATA_STALE    99

byte owner[3];
byte page_data[3];

byte pte_phys = PA_ORIG;
bool pte_valid = true;

byte tlb_phys[2];
bool tlb_valid[2];

bool bkl = false;
bool munmap_done = false;
bool page_reallocated = false;
bool corruption_detected = false;

inline invlpg_local(cpu) {
    tlb_valid[cpu] = false
}

inline tlb_shootdown_ipi(remote_cpu) {
    tlb_valid[remote_cpu] = false
}

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
            result = 0
        fi
    fi
}

/* CPU0: munmap under BKL.  Matches do_munmap (syscall_mm.c). */
proctype cpu0_munmap() {
    atomic { !bkl -> bkl = true };

    /* paging_unmap_page */
    pte_valid = false;
    pte_phys = 0;

    /* pmm_free_page */
    owner[PA_ORIG] = OWNER_FREE;

    /* invlpg (local) */
    invlpg_local(0);

    /* smp_flush_tlb() — IPI shootdown */
    tlb_shootdown_ipi(1);

    bkl = false;
    munmap_done = true
}

/* Another process allocates the freed page from PMM */
proctype other_process_alloc() {
    (owner[PA_ORIG] == OWNER_FREE);
    owner[PA_ORIG] = OWNER_OTHER;
    page_data[PA_ORIG] = DATA_OTHER;
    page_reallocated = true
}

/* CPU1: user mode write after munmap */
proctype cpu1_user_write() {
    byte resolved;
    munmap_done && page_reallocated;

    tlb_resolve(1, resolved);

    if
    :: (resolved != 0) ->
        if
        :: (owner[resolved] == OWNER_OTHER) ->
            page_data[resolved] = DATA_STALE;
            corruption_detected = true
        :: (owner[resolved] == OWNER_PROC_P) ->
            page_data[resolved] = DATA_STALE
        :: (owner[resolved] == OWNER_FREE) ->
            page_data[resolved] = DATA_STALE;
            corruption_detected = true
        fi
    :: (resolved == 0) ->
        skip   /* page fault — correct */
    fi;

    assert(!corruption_detected)
}

init {
    owner[PA_ORIG] = OWNER_PROC_P;
    owner[PA_NEW] = OWNER_FREE;
    page_data[PA_ORIG] = DATA_PROC_P;
    page_data[PA_NEW] = 0;

    pte_phys = PA_ORIG;
    pte_valid = true;

    tlb_phys[0] = PA_ORIG;
    tlb_valid[0] = true;
    tlb_phys[1] = PA_ORIG;
    tlb_valid[1] = true;

    run cpu0_munmap();
    run other_process_alloc();
    run cpu1_user_write()
}
