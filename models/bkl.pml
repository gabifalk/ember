/*
 * BKL (Big Kernel Lock) model for ember SMP.
 *
 * Models N CPUs entering/exiting the kernel via a single global lock.
 * Verifies: mutual exclusion, no deadlock, progress (liveness).
 */

#define N_CPUS 3
#define N_ITERS 4

/* The BKL: 0 = free, 1 = held */
bool bkl = 0;

/* Track which CPU is in the kernel (for mutual exclusion check) */
byte in_kernel = 0;

/* Per-CPU: count of completed kernel entries (for liveness) */
byte completed[N_CPUS];

/* Unfair xchg-based spinlock acquire */
inline bkl_acquire() {
    bool got;
    do
    :: atomic {
        got = bkl;
        bkl = 1;
        if
        :: !got -> break
        :: else -> skip
        fi
    }
    od
}

inline bkl_release() {
    assert(bkl == 1);
    bkl = 0;
}

proctype cpu(byte id) {
    byte i = 0;
    do
    :: i < N_ITERS ->

        /* --- User space (parallel, no lock) --- */
        skip;

        /* --- Kernel entry: acquire BKL --- */
        bkl_acquire();
        in_kernel++;
        assert(in_kernel == 1);  /* mutual exclusion */

        /* --- Kernel work (could be syscall, interrupt, etc.) --- */
        skip;

        /* --- Kernel exit: release BKL --- */
        in_kernel--;
        bkl_release();

        completed[id]++;
        i++
    :: else -> break
    od
}

init {
    byte c = 0;
    do
    :: c < N_CPUS -> run cpu(c); c++
    :: else -> break
    od
}

/* Liveness: every CPU eventually completes all iterations */
ltl progress_cpu0 { <> (completed[0] == N_ITERS) }
ltl progress_cpu1 { <> (completed[1] == N_ITERS) }
ltl progress_cpu2 { <> (completed[2] == N_ITERS) }
