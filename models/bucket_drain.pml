/*
 * Bucket cache drain model for ember kernel heap.
 *
 * Problem: bucket-sized blocks freed via kfree() are pushed onto
 * per-size bucket free lists and marked !free in the heap block list.
 * These lists grow without limit.  Blocks stay marked !free, so they
 * are never coalesced with adjacent free blocks.  Over time the heap
 * fragments into tiny bucket-sized pieces and large allocations fail
 * even though plenty of total memory is in bucket caches.
 *
 * Fix: cap each bucket free list at BUCKET_CAP entries.  When a
 * bucket-sized block is freed and the bucket list is already at
 * capacity, return the block to the heap (mark free + coalesce with
 * neighbors) instead of caching it.
 *
 * Model: 6 contiguous slots, one bucket size class.  Allocate 4 small
 * blocks (splitting the initial free block), free all 4 in
 * nondeterministic order, then assert a large contiguous free block
 * (size >= 3) exists.  With the cap (BUCKET_CAP=1), at most 1 freed
 * block is trapped in the cache; the other 3 return to the heap and
 * coalesce with the remaining 2 free slots, guaranteeing >= 3
 * contiguous free slots regardless of free order.  Without the cap
 * (BUGGY_NO_CAP), all 4 go to the bucket cache and the heap has only
 * 2 contiguous free slots — the assertion fails.
 *
 * Properties:
 *   P1: After small alloc/free cycles, large alloc still possible
 *   P2: Total slots accounted for (no lost memory)
 *
 * Verify:
 *   spin -a models/bucket_drain.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m200000
 *
 * Bug injection (no bucket cap):
 *   spin -a -DBUGGY_NO_CAP models/bucket_drain.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m200000
 */

#define SLOTS 6
#define BUCKET_CAP 1

/* Block states. */
#define ST_FREE   0
#define ST_ALLOC  1
#define ST_BUCKET 2   /* In bucket cache: !free in heap. */

byte st[SLOTS];
byte sz[SLOTS];       /* Heap block size in slots. */
bool alive[SLOTS];    /* true = heap block header here (not absorbed). */
byte bkt_cnt;         /* Entries in the single bucket free list. */

/*
 * Allocate one slot from the heap (first-fit, split if needed).
 * Sets result to slot index, or 255 if OOM.
 */
inline ALLOC(result) {
    result = 255;
    byte _ai;
    _ai = 0;
    do
    :: (_ai < SLOTS) ->
        if
        :: (alive[_ai] && st[_ai] == ST_FREE) ->
            if
            :: (sz[_ai] > 1) ->
                byte _sp;
                _sp = _ai + 1;
                alive[_sp] = true;
                st[_sp] = ST_FREE;
                sz[_sp] = sz[_ai] - 1;
                sz[_ai] = 1
            :: else -> skip
            fi;
            st[_ai] = ST_ALLOC;
            result = _ai;
            _ai = SLOTS
        :: else ->
            _ai = _ai + 1
        fi
    :: else -> break
    od
}

/*
 * Coalesce block at h with its next neighbor if both free.
 */
inline COALESCE_NEXT(h) {
    byte _cn;
    _cn = h + sz[h];
    if
    :: (_cn < SLOTS && alive[_cn] && st[_cn] == ST_FREE) ->
        sz[h] = sz[h] + sz[_cn];
        alive[_cn] = false
    :: else -> skip
    fi
}

/*
 * Coalesce block at h with its prev neighbor if prev is free.
 * After merge, h is absorbed into prev.
 */
inline COALESCE_PREV(h) {
    if
    :: (h > 0) ->
        byte _cp;
        _cp = h - 1;
        do
        :: (_cp > 0 && !alive[_cp]) -> _cp = _cp - 1
        :: else -> break
        od;
        if
        :: (alive[_cp] && st[_cp] == ST_FREE && (_cp + sz[_cp] == h)) ->
            sz[_cp] = sz[_cp] + sz[h];
            alive[h] = false
        :: else -> skip
        fi
    :: else -> skip
    fi
}

/*
 * Free a block.  If bucket has room, cache it (stays !free in heap).
 * If bucket is full, return to heap and coalesce with neighbors.
 */
inline FREE(slot) {
#ifdef BUGGY_NO_CAP
    st[slot] = ST_BUCKET;
    bkt_cnt = bkt_cnt + 1
#else
    if
    :: (bkt_cnt < BUCKET_CAP) ->
        st[slot] = ST_BUCKET;
        bkt_cnt = bkt_cnt + 1
    :: else ->
        st[slot] = ST_FREE;
        COALESCE_NEXT(slot);
        COALESCE_PREV(slot)
    fi
#endif
}

/*
 * Scenario: alloc 4 small blocks, free all 4, check for large free block.
 */
proctype scenario() {
    byte a0, a1, a2, a3;

    ALLOC(a0);   /* slot 0 */
    ALLOC(a1);   /* slot 1 */
    ALLOC(a2);   /* slot 2 */
    ALLOC(a3);   /* slot 3 */
    /* slots 4-5 remain free, size 2 */

    /* Free in nondeterministic order for broad coverage. */
    if
    :: true ->
        FREE(a3); FREE(a2); FREE(a1); FREE(a0)
    :: true ->
        FREE(a0); FREE(a1); FREE(a2); FREE(a3)
    :: true ->
        FREE(a1); FREE(a3); FREE(a0); FREE(a2)
    :: true ->
        FREE(a2); FREE(a0); FREE(a3); FREE(a1)
    :: true ->
        FREE(a0); FREE(a3); FREE(a1); FREE(a2)
    :: true ->
        FREE(a3); FREE(a0); FREE(a2); FREE(a1)
    fi;

    /* P1: A large contiguous free block (>= 3 slots) must exist. */
    bool large_ok = false;
    byte ci;
    ci = 0;
    do
    :: (ci < SLOTS) ->
        if
        :: (alive[ci] && st[ci] == ST_FREE && sz[ci] >= 3) ->
            large_ok = true;
            ci = SLOTS
        :: else ->
            ci = ci + 1
        fi
    :: else -> break
    od;
    assert(large_ok);

    /* P2: Total slots accounted for. */
    byte total;
    total = 0;
    byte ti;
    ti = 0;
    do
    :: (ti < SLOTS) ->
        if
        :: alive[ti] ->
            total = total + sz[ti];
            ti = ti + sz[ti]
        :: else ->
            ti = ti + 1
        fi
    :: else -> break
    od;
    assert(total == SLOTS)
}

init {
    byte i;
    i = 0;
    do
    :: (i < SLOTS) ->
        st[i] = ST_FREE;
        sz[i] = 1;
        alive[i] = true;
        i = i + 1
    :: else -> break
    od;

    /* Start as one big free block. */
    sz[0] = SLOTS;
    byte j;
    j = 1;
    do
    :: (j < SLOTS) ->
        alive[j] = false;
        j = j + 1
    :: else -> break
    od;

    bkt_cnt = 0;

    run scenario()
}
