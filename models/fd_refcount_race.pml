/*
 * File descriptor refcount model for ember SMP.
 *
 * Models the actual code:
 *   do_fork (syscall_proc_fork.c): BKL held throughout.
 *     1. paging_clone_user_pml4() — no sleep, synchronous alloc
 *     2. copy_fd_table() — copies descs, increments refcounts
 *     All under BKL, no release.
 *
 *   close (syscall_file.c): BKL held.
 *     Decrements file_desc refcount, frees if zero.
 *
 * BKL serializes fork and close — no race possible.
 *
 * Verify:
 *   spin -a models/fd_refcount_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

byte refcount = 2;        /* shared file_desc refcount: parent + sibling */
bool desc_freed = false;
bool use_after_free = false;

bool bkl = false;
byte bkl_cpu = 255;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}

/* CPU0: fork — copies fd table, increments refcount */
proctype cpu0_fork() {
    BKL_ACQ(0);

    /* paging_clone_user_pml4 — no sleep */
    skip;

    /* copy_fd_table: copy desc, increment refcount */
    if
    :: !desc_freed ->
        refcount = refcount + 1
    :: desc_freed ->
        use_after_free = true   /* BUG: accessing freed desc */
    fi;

    BKL_REL(0)
}

/* CPU1: close — decrements refcount, frees if zero */
proctype cpu1_close() {
    BKL_ACQ(1);

    refcount = refcount - 1;
    if
    :: refcount == 0 ->
        desc_freed = true
    :: else -> skip
    fi;

    BKL_REL(1)
}

init {
    refcount = 2;
    desc_freed = false;
    run cpu0_fork();
    run cpu1_close()
}

ltl no_uaf { [] !use_after_free }
ltl refcount_ok { [] (refcount >= 0 && refcount <= 3) }
