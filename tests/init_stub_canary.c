/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

/*
 * Stub canary test -- verifies that known stub syscalls return their expected
 * stub values.  When a stub is replaced with a real implementation, the
 * corresponding canary will fail, signaling that proper tests should be added.
 */

#include "test_common.h"

static long sys0(long nr) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr) : "rcx","r11","memory");
    return ret;
}
static long sys1(long nr, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1) : "rcx","r11","memory");
    return ret;
}
static long sys2(long nr, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2) : "rcx","r11","memory");
    return ret;
}
static long sys3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx","r11","memory");
    return ret;
}
static long sys4(long nr, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx","r11","memory");
    return ret;
}
static long sys5(long nr, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx","r11","memory");
    return ret;
}

/* Signal stubs. */
#define __NR_sigaltstack     131   /* Returns 0. */

/* Memory stubs. */
#define __NR_mlock           149   /* Returns 0. */
#define __NR_munlock         150   /* Returns 0. */
#define __NR_msync           26    /* Returns 0. */

/* Ownership stubs. */
#define __NR_chown           92    /* Returns 0. */
#define __NR_fchown          93    /* Returns 0. */
#define __NR_lchown          94    /* Returns 0. */

/* Identity stubs. */
#define __NR_setuid          105   /* Returns 0. */
#define __NR_setgid          106   /* Returns 0. */
#define __NR_setgroups       116   /* Returns 0. */

/* Epoll stubs (partial -- create is real, ctl/wait are stubs) */
#define __NR_epoll_create1   291   /* Returns fd (real, not a stub) */
#define __NR_epoll_ctl       233   /* Returns 0. */
#define __NR_epoll_wait      232   /* Returns 0 (with timeout=0) */
#define __NR_epoll_pwait     281   /* Returns 0 (with timeout=0) */
#define __NR_close           3

/* arch_prctl GS variants (not implemented, return -EINVAL) */
#define __NR_arch_prctl      158   /* ARCH_SET_GS=0x1001 returns -22, ARCH_GET_GS=0x1004 returns -22. */

#define ARCH_SET_GS  0x1001
#define ARCH_GET_GS  0x1004
#define EINVAL       22

int main(void) {
    msg("=== stub canary tests ===\n");
    msg("(these verify stub return values; failure means a real impl landed)\n");

    /* -- Signal stubs -- */
    long r;

    r = sys2(__NR_sigaltstack, 0, 0);
    check(r == 0, "sigaltstack stub returns 0");

    /* -- Memory stubs -- */
    r = sys2(__NR_mlock, 0, 0);
    check(r == 0, "mlock stub returns 0");

    r = sys2(__NR_munlock, 0, 0);
    check(r == 0, "munlock stub returns 0");

    r = sys3(__NR_msync, 0, 0, 0);
    check(r == 0, "msync stub returns 0");

    /*
     * -- Ownership stubs --
     * Note: chown/fchown/lchown accept any path/fd and return 0.
     */
    r = sys3(__NR_chown, (long)"/", 0, 0);
    check(r == 0, "chown stub returns 0");

    r = sys3(__NR_fchown, 0, 0, 0);  /* Fd 0 = stdin, valid. */
    check(r == 0, "fchown stub returns 0");

    r = sys3(__NR_lchown, (long)"/", 0, 0);
    check(r == 0, "lchown stub returns 0");

    /* -- Identity stubs -- */
    r = sys1(__NR_setuid, 0);
    check(r == 0, "setuid stub returns 0");

    r = sys1(__NR_setgid, 0);
    check(r == 0, "setgid stub returns 0");

    r = sys2(__NR_setgroups, 0, 0);
    check(r == 0, "setgroups stub returns 0");

    /*
     * -- Epoll stubs --
     * First create an epoll fd (this is a real operation)
     */
    long epfd = sys1(__NR_epoll_create1, 0);
    check(epfd >= 0, "epoll_create1 for stub tests");

    if (epfd >= 0) {
        r = sys4(__NR_epoll_ctl, epfd, 1, 0, 0);  /* EPOLL_CTL_ADD=1, fd=0(stdin), no event. */
        check(r == 0, "epoll_ctl stub returns 0");

        r = sys4(__NR_epoll_wait, epfd, 0, 1, 0);  /* Events=NULL, maxevents=1, timeout=0. */
        check(r == 0, "epoll_wait stub returns 0 (timeout=0)");

        r = sys5(__NR_epoll_pwait, epfd, 0, 1, 0, 0);  /* +Sigmask=NULL. */
        check(r == 0, "epoll_pwait stub returns 0 (timeout=0)");

        sys1(__NR_close, epfd);
    }

    /* -- Arch_prctl GS variants (not implemented) -- */
    r = sys2(__NR_arch_prctl, ARCH_SET_GS, 0);
    check_errno(r, -EINVAL, "ARCH_SET_GS returns -EINVAL");

    unsigned long gs_val = 0;
    r = sys2(__NR_arch_prctl, ARCH_GET_GS, (long)&gs_val);
    check_errno(r, -EINVAL, "ARCH_GET_GS returns -EINVAL");

    test_done();
    return 0;
}
