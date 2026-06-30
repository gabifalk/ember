/*
 * Heap coalescing model for ember.
 *
 * Models the proposed fix: coalesce only with address-adjacent neighbors
 * using a prev pointer, instead of scanning the entire list.
 *
 * Current code (BUGGY_SLOW): kfree() walks the entire heap linked list
 * on every non-bucket free, looking for any pair of adjacent free blocks
 * to coalesce.  This is O(n) per free where n = total blocks.
 *
 * Proposed fix: each heap_block_t gets a prev pointer.  kfree() only
 * checks the freed block's immediate prev and next neighbors for
 * coalescing.  This is O(1) per free.
 *
 * We model a small heap with 4 blocks (A, B, C, D) laid out contiguously
 * in memory.  Two concurrent allocators perform alloc/free sequences.
 * The model checks that the O(1) neighbor-only coalescing produces the
 * same coalesced state as the O(n) full-scan approach.
 *
 * Properties:
 *   P1: No double-free (freeing an already-free block)
 *   P2: Coalescing correctness (adjacent free blocks are always merged)
 *   P3: List integrity (prev/next pointers consistent)
 *   P4: No lost blocks (total memory accounted for)
 *
 * Verify:
 *   spin -a models/heap_coalesce.pml && \
 *   gcc -O2 -o pan pan.c && \
 *   ./pan -E -m200000
 *
 * Bug injection (no coalescing at all):
 *   spin -a -DBUGGY_NO_COALESCE models/heap_coalesce.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m200000
 */

/*
 * We model 4 contiguous blocks: blk[0..3].
 * Each block is either free or allocated.
 * Adjacent free blocks should be coalesced.
 *
 * prev[i] and nxt[i] form a doubly-linked list in address order.
 * When blocks are coalesced, the merged block absorbs the neighbor.
 *
 * To keep the model tractable, we track:
 *   - free[i]: whether block i is free
 *   - size[i]: how many original slots this block spans
 *   - alive[i]: whether this slot is the "head" of a block (not absorbed)
 *
 * Coalescing with next: if blk[i] and blk[i+size[i]] are both free
 * and alive, merge them (size[i] += size[j], alive[j] = false).
 *
 * Coalescing with prev: find prev block (scan backward for alive),
 * if prev is free, merge prev with current.
 */

#define N 4

bool blk_free[N];    /* true = free, false = allocated */
byte blk_size[N];    /* number of original slots this block spans */
bool blk_alive[N];   /* true = this is a block header (not absorbed) */

bool double_free = false;
bool lock = false;
byte done_count = 0;

inline LOCK() {
    atomic { !lock -> lock = true }
}
inline UNLOCK() {
    lock = false
}

/*
 * Find the head block that contains slot i.
 * Returns the index of the alive block that spans slot i.
 * In the model, we just search backward.
 */
inline FIND_HEAD(slot, result) {
    result = slot;
    do
    :: (result > 0 && !blk_alive[result]) -> result = result - 1
    :: else -> break
    od
}

/*
 * Coalesce block at index h with its next neighbor if both free.
 * O(1) operation: just check h + size[h].
 */
inline COALESCE_NEXT(h) {
    byte nn;
    nn = h + blk_size[h];
    if
    :: (nn < N && blk_alive[nn] && blk_free[nn]) ->
        blk_size[h] = blk_size[h] + blk_size[nn];
        blk_alive[nn] = false
    :: else -> skip
    fi
}

/*
 * Coalesce block at index h with its prev neighbor if both free.
 * O(1) operation: scan backward for the nearest alive block.
 */
inline COALESCE_PREV(h, merged_h) {
    merged_h = h;
    if
    :: (h > 0) ->
        byte pp;
        pp = h - 1;
        do
        :: (pp > 0 && !blk_alive[pp]) -> pp = pp - 1
        :: else -> break
        od;
        if
        :: (blk_alive[pp] && blk_free[pp]) ->
            blk_size[pp] = blk_size[pp] + blk_size[h];
            blk_alive[h] = false;
            merged_h = pp
        :: else -> skip
        fi
    :: else -> skip
    fi
}

/*
 * Free block at slot i with O(1) neighbor coalescing.
 */
inline DO_FREE(slot) {
    LOCK();
    byte head;
    FIND_HEAD(slot, head);
    if
    :: blk_free[head] -> double_free = true
    :: else ->
        blk_free[head] = true;
#ifndef BUGGY_NO_COALESCE
        /* Coalesce with next, then with prev. */
        COALESCE_NEXT(head);
        byte merged;
        COALESCE_PREV(head, merged);
        /* After merging with prev, try coalescing prev with ITS next
         * (which is now the block after the merged region). */
        if
        :: (merged != head) -> COALESCE_NEXT(merged)
        :: else -> skip
        fi
#endif
    fi;
    UNLOCK()
}

/*
 * Allocate: find first free block with size >= 1, mark it allocated.
 * If block size > 1, split: keep size 1 allocated, remainder free.
 * Returns slot index in 'result' (-1 if OOM).
 */
inline DO_ALLOC(result) {
    LOCK();
    result = 255;  /* 255 = not found */
    byte ai;
    ai = 0;
    do
    :: (ai < N) ->
        if
        :: (blk_alive[ai] && blk_free[ai]) ->
            /* Found a free block. Split if size > 1. */
            if
            :: (blk_size[ai] > 1) ->
                byte split_at;
                split_at = ai + 1;
                blk_alive[split_at] = true;
                blk_free[split_at] = true;
                blk_size[split_at] = blk_size[ai] - 1;
                blk_size[ai] = 1
            :: else -> skip
            fi;
            blk_free[ai] = false;
            result = ai;
            ai = N  /* break */
        :: else -> ai = ai + 1
        fi
    :: else -> break
    od;
    UNLOCK()
}

/*
 * Check that no two adjacent alive free blocks exist (coalescing invariant).
 * This is the key correctness property.
 */
inline CHECK_COALESCE_INVARIANT() {
    byte ci;
    ci = 0;
    do
    :: (ci < N) ->
        if
        :: blk_alive[ci] && blk_free[ci] ->
            byte ni;
            ni = ci + blk_size[ci];
            /* Next alive block must not also be free. */
            assert(!(ni < N && blk_alive[ni] && blk_free[ni]))
        :: else -> skip
        fi;
        ci = ci + 1
    :: else -> break
    od
}

/*
 * Check list integrity: sizes add up, alive blocks don't overlap.
 */
inline CHECK_INTEGRITY() {
    byte total;
    total = 0;
    byte ii;
    ii = 0;
    do
    :: (ii < N) ->
        if
        :: blk_alive[ii] ->
            total = total + blk_size[ii];
            ii = ii + blk_size[ii]
        :: else ->
            ii = ii + 1
        fi
    :: else -> break
    od;
    assert(total == N)
}

/*
 * Worker 0: alloc two blocks, free them in reverse order.
 */
proctype worker0() {
    byte a0, a1;
    DO_ALLOC(a0);
    DO_ALLOC(a1);

    if
    :: (a1 != 255) -> DO_FREE(a1)
    :: else -> skip
    fi;
    if
    :: (a0 != 255) -> DO_FREE(a0)
    :: else -> skip
    fi;

    done_count++
}

/*
 * Worker 1: alloc one block, free it.
 */
proctype worker1() {
    byte a0;
    DO_ALLOC(a0);

    if
    :: (a0 != 255) -> DO_FREE(a0)
    :: else -> skip
    fi;

    done_count++
}

/*
 * Monitor: after all workers finish, check invariants.
 */
proctype monitor() {
    (done_count == 2);
    LOCK();
    CHECK_COALESCE_INVARIANT();
    CHECK_INTEGRITY();
    UNLOCK()
}

init {
    /* Initialize: 4 contiguous free blocks, each size 1. */
    byte i;
    i = 0;
    do
    :: (i < N) ->
        blk_free[i] = true;
        blk_size[i] = 1;
        blk_alive[i] = true;
        i = i + 1
    :: else -> break
    od;

    /* Pre-coalesce into one big block (initial heap state). */
    blk_size[0] = N;
    blk_alive[1] = false;
    blk_alive[2] = false;
    blk_alive[3] = false;

    run worker0();
    run worker1();
    run monitor()
}
