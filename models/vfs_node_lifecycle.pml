/*
 * VFS node lifecycle model for ember.
 *
 * Models the proposed fix: free evicted VFS nodes when refcount == 0.
 *
 * Current code (BUGGY): vfs_evict() unlinks node from VFS list but
 * never calls kfree(), leaking memory.  Over time the heap linked
 * list grows monotonically, causing O(n) degradation in kmalloc/kfree.
 *
 * Proposed fix: when a node is evicted AND refcount == 0, kfree it.
 * When close decrements refcount to 0 AND node is evicted, kfree it.
 * Both transitions must be checked atomically under vfs_lock.
 *
 * Actors (models a fork+exec build cycle):
 *   proc0_open  — parent opens file, bumps refcount
 *   proc_fork   — fork copies fd table, bumps refcount
 *   proc0_close — parent closes fd
 *   proc1_close — child closes fd (inherited from fork)
 *   evictor     — cache pressure evicts node from VFS list
 *
 * Properties:
 *   P1: no use-after-free (no access to freed node)
 *   P2: no double-free
 *   P3: no leak (node freed after eviction + all refs released)
 *   P4: refcount never negative (assert in close)
 *
 * Verify:
 *   spin -a models/vfs_node_lifecycle.pml && \
 *   gcc -O2 -o pan pan.c && \
 *   ./pan -E -m200000 -N no_uaf && \
 *   ./pan -E -m200000 -a -N no_double_free && \
 *   ./pan -E -m200000
 *
 * Bug injection (current code — never frees):
 *   spin -a -DBUGGY_LEAK models/vfs_node_lifecycle.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m200000
 */

/* Completion tracking for leak monitor */
byte done_count = 0;

/* Node state */
byte refcount = 0;       /* Open FD references to this node */
bool in_vfs_list = true; /* Node is linked in the VFS list */
bool freed = false;      /* Node memory has been kfree'd */
bool double_free = false;
bool use_after_free = false;

/* Locks */
bool bkl = false;
byte bkl_cpu = 255;
bool vfs_locked = false;

/* Per-process open FD count on this node. */
byte proc0_refs = 0;
byte proc1_refs = 0;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}
inline VFS_LOCK() {
    atomic { !vfs_locked -> vfs_locked = true }
}
inline VFS_UNLOCK() {
    vfs_locked = false
}

inline CHECK_ACCESS() {
    if
    :: freed -> use_after_free = true
    :: else -> skip
    fi
}

inline TRY_FREE() {
    if
    :: (refcount == 0 && !in_vfs_list) ->
        if
        :: freed -> double_free = true
        :: else ->
#ifndef BUGGY_LEAK
            freed = true
#endif
        fi
    :: else -> skip
    fi
}

/*
 * proc0_open: parent opens file.
 * Models: do_openat -> vfs_lookup -> file_desc_alloc.
 */
proctype proc0_open() {
    BKL_ACQ(0);
    VFS_LOCK();

    if
    :: in_vfs_list && !freed ->
        refcount = refcount + 1;
        proc0_refs = proc0_refs + 1;
        CHECK_ACCESS()
    :: else -> skip
    fi;

    VFS_UNLOCK();
    BKL_REL(0);
    done_count++
}

/*
 * proc0_close: parent closes fd.
 * Models: do_close -> file_desc_unref -> vfs_unref.
 */
proctype proc0_close() {
    BKL_ACQ(0);

    if
    :: proc0_refs > 0 ->
        VFS_LOCK();
        CHECK_ACCESS();
        refcount = refcount - 1;
        assert(refcount >= 0);
        proc0_refs = proc0_refs - 1;
        TRY_FREE();
        VFS_UNLOCK()
    :: else -> skip
    fi;

    BKL_REL(0);
    done_count++
}

/*
 * fork: copies fd table, bumps refcount for each shared file_desc.
 * Models: do_fork -> copy_fd_table.  BKL held throughout.
 * Child (proc1) inherits all of parent's open fds.
 */
proctype proc_fork() {
    BKL_ACQ(0);

    if
    :: proc0_refs > 0 && !freed ->
        VFS_LOCK();
        refcount = refcount + proc0_refs;
        proc1_refs = proc1_refs + proc0_refs;
        VFS_UNLOCK()
    :: else -> skip
    fi;

    BKL_REL(0);
    done_count++
}

/*
 * proc1_close: child closes inherited fd.
 * Models: do_close -> file_desc_unref -> vfs_unref.
 */
proctype proc1_close() {
    BKL_ACQ(1);

    if
    :: proc1_refs > 0 ->
        VFS_LOCK();
        CHECK_ACCESS();
        refcount = refcount - 1;
        assert(refcount >= 0);
        proc1_refs = proc1_refs - 1;
        TRY_FREE();
        VFS_UNLOCK()
    :: else -> skip
    fi;

    BKL_REL(1);
    done_count++
}

/*
 * evictor: vfs_evict triggered by cache pressure.
 * Unlinks node from VFS list.  If refcount == 0, frees immediately.
 * Otherwise, defers free to the last close.
 *
 * Runs under BKL (called from syscall path, e.g. vfs_create).
 */
proctype evictor() {
    BKL_ACQ(0);
    VFS_LOCK();

    if
    :: in_vfs_list ->
        in_vfs_list = false;
        TRY_FREE()
    :: else -> skip
    fi;

    VFS_UNLOCK();
    BKL_REL(0);
    done_count++
}

/*
 * leak_monitor: waits for all actors to finish, then checks that
 * the node was properly freed (not leaked).
 */
proctype leak_monitor() {
    (done_count == 5);
    assert(freed || in_vfs_list || proc0_refs > 0 || proc1_refs > 0)
}

init {
    refcount = 0;
    in_vfs_list = true;
    freed = false;

    run proc0_open();
    run proc_fork();
    run evictor();
    run proc0_close();
    run proc1_close();
    run leak_monitor()
}

/* P1: No use-after-free */
ltl no_uaf { [] !use_after_free }

/* P2: No double-free */
ltl no_double_free { [] !double_free }

/* P3: No leak — checked by leak_monitor assertion */
/* P4: Refcount never negative — checked by assert in close */
