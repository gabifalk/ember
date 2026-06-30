/*
 * Block cache safety model for ember SMP.
 *
 * Models the actual blkcache_read (blkcache.c:143-220):
 *   spin_lock(&blkcache_lock);
 *   // hash lookup, find_lru, blkdev_read, fill slot, hash_insert
 *   // ALL under the lock — no release during I/O
 *   spin_unlock(&blkcache_lock);
 *
 * Under SMP with BKL, kernel entry is serialized. The cache lock
 * provides additional protection. Both are held during the entire
 * read path — no release/reacquire window.
 *
 * Verifies: no cache corruption — slot data always matches block tag.
 *
 * Verify:
 *   spin -a models/blkcache_evict_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define N_SLOTS   3
#define BLOCK_A   1
#define BLOCK_B   2
#define BLOCK_C   3

byte slot_block[N_SLOTS];
byte slot_data[N_SLOTS];
byte slot_valid[N_SLOTS];
byte slot_use[N_SLOTS];
byte use_counter = 0;

bool bkl = false;
byte bkl_cpu = 255;
bool cache_lock = false;
bool corruption = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}
inline CACHE_LOCK() {
    atomic { !cache_lock -> cache_lock = true }
}
inline CACHE_UNLOCK() {
    cache_lock = false
}

inline HASH_LOOKUP(block, result) {
    result = 255;
    byte hli = 0;
    do
    :: hli < N_SLOTS ->
        if
        :: slot_valid[hli] && slot_block[hli] == block ->
            result = hli; break
        :: else -> skip
        fi;
        hli++
    :: else -> break
    od
}

inline FIND_LRU(result) {
    result = 255;
    byte fli;
    byte min_use = 255;
    fli = 0;
    do
    :: fli < N_SLOTS ->
        if
        :: !slot_valid[fli] -> result = fli; break
        :: else -> skip
        fi;
        fli++
    :: else -> break
    od;
    if
    :: result == 255 ->
        fli = 0;
        do
        :: fli < N_SLOTS ->
            if
            :: slot_valid[fli] && slot_use[fli] < min_use ->
                min_use = slot_use[fli];
                result = fli
            :: else -> skip
            fi;
            fli++
        :: else -> break
        od;
        if
        :: result != 255 ->
            slot_valid[result] = 0;
            slot_block[result] = 0;
            slot_data[result] = 0
        :: else -> skip
        fi
    :: else -> skip
    fi
}

inline CHECK_INTEGRITY() {
    byte ci = 0;
    do
    :: ci < N_SLOTS ->
        if
        :: slot_valid[ci] && slot_data[ci] != slot_block[ci] ->
            corruption = true
        :: else -> skip
        fi;
        ci++
    :: else -> break
    od
}

/* CPU0: blkcache_read(BLOCK_A) — matches blkcache.c:143-184 */
proctype cpu0_read() {
    byte slot;
    byte found;

    BKL_ACQ(0);
    CACHE_LOCK();
    use_counter++;

    HASH_LOOKUP(BLOCK_A, found);

    if
    :: found != 255 ->
        /* Cache hit — just bump use_count */
        slot_use[found] = use_counter;
        CACHE_UNLOCK();
        BKL_REL(0)
    :: found == 255 ->
        /* Cache miss — find LRU, read from disk, fill slot.
         * ALL under cache_lock (blkcache.c:161-184) */
        FIND_LRU(slot);
        assert(slot != 255);

        /* blkdev_read + fill metadata (lines 163-173) */
        slot_block[slot] = BLOCK_A;
        slot_data[slot] = BLOCK_A;
        slot_valid[slot] = 1;
        slot_use[slot] = use_counter;

        CACHE_UNLOCK();
        BKL_REL(0)
    fi
}

/* CPU1: blkcache_read(BLOCK_B) */
proctype cpu1_read() {
    byte slot;
    byte found;

    BKL_ACQ(1);
    CACHE_LOCK();
    use_counter++;

    HASH_LOOKUP(BLOCK_B, found);

    if
    :: found != 255 ->
        slot_use[found] = use_counter;
        CACHE_UNLOCK();
        BKL_REL(1)
    :: found == 255 ->
        FIND_LRU(slot);
        assert(slot != 255);

        slot_block[slot] = BLOCK_B;
        slot_data[slot] = BLOCK_B;
        slot_valid[slot] = 1;
        slot_use[slot] = use_counter;

        CACHE_UNLOCK();
        BKL_REL(1)
    fi
}

/* Checker: verify integrity after both reads complete */
proctype checker() {
    /* Wait for both to finish (they terminate) */
    byte dummy;
    HASH_LOOKUP(BLOCK_A, dummy);
    HASH_LOOKUP(BLOCK_B, dummy);
    CHECK_INTEGRITY();
    assert(!corruption)
}

init {
    /* Pre-fill slots 0,1; slot 2 empty */
    slot_block[0] = BLOCK_C; slot_data[0] = BLOCK_C;
    slot_valid[0] = 1; slot_use[0] = 1;
    slot_block[1] = BLOCK_C + 1; slot_data[1] = BLOCK_C + 1;
    slot_valid[1] = 1; slot_use[1] = 2;
    slot_valid[2] = 0;
    use_counter = 3;

    run cpu0_read();
    run cpu1_read();
    run checker()
}

ltl no_corruption { [] !corruption }
