/*
 * GS segment load zeroes GS base — enter_user bug.
 *
 * On x86-64, GS base is managed by MSRs:
 *   IA32_GS_BASE (current GS base)
 *   IA32_KERNEL_GS_BASE (swapped by swapgs)
 *
 * Loading a segment selector into GS via "mov %cx, %gs" zeroes
 * IA32_GS_BASE on AMD64 (and some Intel).  enter_user does this
 * to set user data segments before iretq to user mode.
 *
 * After "mov $0x1b, %gs":
 *   IA32_GS_BASE = 0 (zeroed by segment load)
 *   IA32_KERNEL_GS_BASE = unchanged (still kernel cpu_local)
 *
 * enter_user then does iretq → user mode.  GS_BASE=0, KGSBASE=kernel.
 * User does syscall → swapgs: GS_BASE=kernel, KGSBASE=0.  Correct!
 *
 * But if enter_user is called AFTER swapgs already set things up:
 *   Before enter_user: GS_BASE=kernel, KGSBASE=user
 *   "mov $0x1b, %gs": GS_BASE=0, KGSBASE=user
 *   iretq → user mode: GS_BASE=0, KGSBASE=user
 *   syscall → swapgs: GS_BASE=user, KGSBASE=0  ← WRONG! KGSBASE=0!
 *   gs:0 reads from user memory → bad kstack_top
 *
 * Fix: don't load GS segment in enter_user.
 *
 * Verify:
 *   spin -a models/gs_segment_load.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

/* MSR state */
byte gs_base = 0;      /* 0=zero, 1=kernel, 2=user */
byte kgs_base = 0;     /* 0=zero, 1=kernel, 2=user */

bool gs_wrong = false;

/* swapgs: swap gs_base ↔ kgs_base */
inline SWAPGS() {
    byte tmp = gs_base;
    gs_base = kgs_base;
    kgs_base = tmp
}

proctype lifecycle() {
    /* Initial state: kernel mode, gs_base=kernel, kgs_base=user(0) */
    gs_base = 1;    /* kernel cpu_local */
    kgs_base = 0;   /* no user TLS yet */

    /* ── First entry to user mode via enter_user ── */

#ifdef FIXED
    /* FIX: don't load GS segment, use swapgs to set user GS */
    SWAPGS();
    /* gs_base=0 (user), kgs_base=1 (kernel) */
#else
    /* BUGGY: "mov $0x1b, %gs" zeroes gs_base */
    gs_base = 0;    /* segment load zeroes GS_BASE */
    /* kgs_base unchanged = 0 (no user TLS) */
    /* gs_base=0, kgs_base=0 */
#endif

    /* iretq → user mode */
    /* User runs... */

    /* ── Syscall ── */
    SWAPGS();
    /* Should be: gs_base=kernel(1), kgs_base=user */

    /* CHECK: gs_base must be kernel after swapgs at syscall entry */
    if
    :: gs_base != 1 -> gs_wrong = true
    :: else -> skip
    fi;

    /* syscall_dispatch... */
    /* syscall_return: swapgs back */
    SWAPGS();

    /* iretq → user mode */
    /* User runs... */

    /* ── Second syscall ── */
    SWAPGS();

    if
    :: gs_base != 1 -> gs_wrong = true
    :: else -> skip
    fi;

    SWAPGS();
    /* back to user */

    /* ── Third syscall ── */
    SWAPGS();

    if
    :: gs_base != 1 -> gs_wrong = true
    :: else -> skip
    fi
}

/* Also test: enter_user called after exec (schedule already set up GS) */
proctype exec_path() {
    /* Schedule set up: gs_base=kernel, kgs_base=user_tls(2) */
    gs_base = 1;
    kgs_base = 2;

    /* exec calls enter_user to jump to new program */

#ifdef FIXED
    SWAPGS();
    /* gs_base=2(user_tls), kgs_base=1(kernel) */
#else
    /* BUGGY: segment load zeroes gs_base */
    gs_base = 0;
    /* kgs_base=2(user_tls) — unchanged */
#endif

    /* iretq → user mode */

    /* Syscall */
    SWAPGS();
    /* BUGGY: gs_base=2(user_tls), kgs_base=0 ← WRONG! kernel lost */
    /* FIXED: gs_base=1(kernel), kgs_base=2(user_tls) ← correct */

    if
    :: gs_base != 1 -> gs_wrong = true
    :: else -> skip
    fi
}

init {
    /* Run separately — gs_base/kgs_base are per-CPU MSRs */
    if
    :: true -> run lifecycle()
    :: true -> run exec_path()
    fi
}

ltl no_gs_wrong { [] !gs_wrong }
