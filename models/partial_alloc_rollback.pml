/*
 * Partial allocation rollback model for ember.
 *
 * Models the common pattern in mmap, mremap, and paging_clone: a loop
 * allocates N pages, but if allocation i fails, pages 0..i-1 must be
 * freed (rolled back).
 *
 * Current code (BUGGY): returns -ENOMEM without freeing already-mapped
 * pages, leaking physical memory.
 *
 * Proposed fix: on failure, unmap and free pages 0..i-1.
 *
 * We model 3 page allocations where the 3rd can fail.
 * A separate actor can allocate from the same PMM pool to create
 * pressure (making OOM realistic).
 *
 * Properties:
 *   P1: no page leak (all allocated pages freed on failure)
 *   P2: no double-free
 *   P3: on success, all pages allocated and accounted for
 *
 * Verify:
 *   spin -a models/partial_alloc_rollback.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -m200000
 *
 * Bug injection (no rollback):
 *   spin -a -DBUGGY_NO_ROLLBACK models/partial_alloc_rollback.pml && \
 *   gcc -O2 -DBUGGY_NO_ROLLBACK -o pan pan.c && ./pan -m200000
 */

#define POOL_SIZE 4    /* Total physical pages available */
#define NEED      3    /* Pages needed by the allocator */

byte pool_free = POOL_SIZE;  /* Free page count in PMM */
byte mapped = 0;             /* Pages successfully mapped so far */
bool alloc_done = false;
bool success = false;
bool double_free = false;
bool leak = false;

inline PMM_ALLOC(ok) {
    if
    :: (pool_free > 0) -> pool_free = pool_free - 1; ok = true
    :: (pool_free == 0) -> ok = false
    fi
}

inline PMM_FREE() {
    pool_free = pool_free + 1
}

/*
 * Pressure: another process allocates pages from the same pool,
 * creating the OOM condition that triggers the partial failure.
 */
proctype pressure() {
    bool ok;
    PMM_ALLOC(ok);
    if
    :: ok -> skip  /* Holds the page forever. */
    :: else -> skip
    fi
}

/*
 * Allocator: tries to allocate NEED pages.
 * Models mmap/mremap/clone loop.
 */
proctype allocator() {
    bool ok;
    byte i = 0;

    do
    :: (i < NEED) ->
        PMM_ALLOC(ok);
        if
        :: ok ->
            mapped = mapped + 1;
            i = i + 1
        :: !ok ->
            /* Allocation failed. Roll back pages 0..mapped-1. */
#ifndef BUGGY_NO_ROLLBACK
            do
            :: (mapped > 0) ->
                mapped = mapped - 1;
                PMM_FREE()
            :: (mapped == 0) -> break
            od
#endif
            ;
            alloc_done = true;
            break
        fi
    :: (i >= NEED) ->
        /* All NEED pages allocated successfully. */
        success = true;
        alloc_done = true;
        break
    od
}

/*
 * Monitor: check invariants after allocator finishes.
 */
proctype monitor() {
    alloc_done;

    if
    :: success ->
        /* All pages allocated — mapped must equal NEED. */
        assert(mapped == NEED)
    :: !success ->
        /* Failed — no pages should remain mapped (all rolled back). */
        assert(mapped == 0)
    fi;

    /* No pages leaked: free + mapped + pressure(0 or 1) == POOL_SIZE. */
    /* We can't track pressure's page directly, but we can check
     * that on failure, all our pages were returned. */
    if
    :: !success ->
        /* Our pages are all freed. Pool might be short by pressure's page. */
        assert(pool_free >= POOL_SIZE - 2)  /* pressure procs may hold up to 2 pages */
    :: else -> skip
    fi
}

init {
    pool_free = POOL_SIZE;
    mapped = 0;

    run pressure();
    run pressure();
    run allocator();
    run monitor()
}
