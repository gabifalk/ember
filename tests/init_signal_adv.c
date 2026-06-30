/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Advanced signal syscall tests -- exercises rt_sigpending, rt_sigsuspend,
 * tkill, tgkill, and sigaltstack.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_rt_sigaction    13
#define __NR_rt_sigprocmask  14
#define __NR_rt_sigreturn    15
#define __NR_rt_sigpending   127
#define __NR_rt_sigsuspend   130
#define __NR_sigaltstack     131
#define __NR_getpid          39
#define __NR_kill            62
#define __NR_alarm           37
#define __NR_fork            57
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_tkill           200
#define __NR_tgkill          234
#define __NR_nanosleep       35

/* Signal numbers. */
#define SIGUSR1      10
#define SIGUSR2      12
#define SIGALRM      14

/* Signal mask bits. */
#define SIGUSR1_MASK (1UL << 10)
#define SIGUSR2_MASK (1UL << 12)
#define SIGALRM_MASK (1UL << 14)

/* Sigprocmask how values. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

/* Flags. */
#define SA_RESTORER  0x04000000

/* Errno values. */
#define EINTR        4
#define ESRCH        3

/* Raw syscall wrappers. */
static long
sys0(long nr)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr):"rcx", "r11", "memory");
	return ret;
}

static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1):"rcx", "r11",
			  "memory");
	return ret;
}

static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2):"rcx",
			  "r11", "memory");
	return ret;
}

static long
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3):"rcx", "r11", "memory");
	return ret;
}

static long
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10):"rcx", "r11", "memory");
	return ret;
}

/* kernel_sigaction for x86_64: handler(8), flags(8), restorer(8), mask(8) */
struct kernel_sigaction {
	void (*handler) (int);
	unsigned long flags;
	void (*restorer) (void);
	unsigned long mask;
};

struct timespec {
	long tv_sec;
	long tv_nsec;
};

/* Custom restorer that calls rt_sigreturn. */
__asm__(".type my_restorer, @function\n"
	"my_restorer:\n" "    mov $15, %rax\n" "    syscall\n");
extern void my_restorer(void);

/* ---- Test state ---- */
static volatile int handler_count = 0;

static void
handler_usr1(int sig)
{
	(void)sig;
	handler_count++;
}

static void
handler_alrm(int sig)
{
	(void)sig;
	handler_count++;
}

/* ---- Helper: install signal handler ---- */
static void
install_handler(int signum, void (*fn) (int))
{
	struct kernel_sigaction sa;
	sa.handler = fn;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;
	sa.mask = 0;
	sys4(__NR_rt_sigaction, signum, (long)&sa, 0, 8);
}

/* ---- Tests ---- */

static void
test_sigpending_basic(void)
{
	msg("test_sigpending_basic");

	/* Block SIGUSR1. */
	unsigned long block_mask = SIGUSR1_MASK;
	sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&block_mask, 0, 8);

	/* Send SIGUSR1 to self (will be pending since blocked) */
	long pid = sys0(__NR_getpid);
	sys2(__NR_kill, pid, SIGUSR1);

	/* Check pending signals. */
	unsigned long pending = 0;
	long ret = sys2(__NR_rt_sigpending, (long)&pending, 8);
	check(ret == 0, "rt_sigpending returns 0");
	check((pending & SIGUSR1_MASK) != 0, "SIGUSR1 is pending");

	/* Unblock to clean up (handler will fire and consume the signal) */
	install_handler(SIGUSR1, handler_usr1);
	unsigned long unblock_mask = SIGUSR1_MASK;
	sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&unblock_mask, 0, 8);
}

static void
test_sigpending_empty(void)
{
	msg("test_sigpending_empty");

	unsigned long pending = ~0UL;	/* Set to non-zero to verify it gets cleared. */
	long ret = sys2(__NR_rt_sigpending, (long)&pending, 8);
	check(ret == 0, "rt_sigpending returns 0");
	check(pending == 0, "no signals pending");
}

static void
test_sigsuspend_wakeup(void)
{
	msg("test_sigsuspend_wakeup");

	handler_count = 0;
	install_handler(SIGALRM, handler_alrm);

	long parent_pid = sys0(__NR_getpid);
	long pid = sys0(__NR_fork);

	if (pid == 0) {
		/* Child: sleep 50ms then send SIGALRM to parent. */
		struct timespec ts;
		ts.tv_sec = 0;
		ts.tv_nsec = 50000000;	/* 50Ms. */
		sys2(__NR_nanosleep, (long)&ts, 0);
		sys2(__NR_kill, parent_pid, SIGALRM);
		sys1(__NR_exit, 0);
	}
	/* Parent: sigsuspend with empty mask (all signals unblocked) */
	unsigned long mask = 0;
	long ret = sys2(__NR_rt_sigsuspend, (long)&mask, 8);
	check(ret == -EINTR, "rt_sigsuspend returns -EINTR");
	check(handler_count > 0, "SIGALRM handler ran");

	/* Wait for child. */
	sys4(__NR_wait4, pid, 0, 0, 0);
}

static void
test_sigsuspend_unmask_delivery(void)
{
	msg("test_sigsuspend_unmask_delivery");

	handler_count = 0;

	/* Block SIGUSR1. */
	unsigned long block_mask = SIGUSR1_MASK;
	sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&block_mask, 0, 8);

	/* Send SIGUSR1 to self (now pending) */
	long pid = sys0(__NR_getpid);
	sys2(__NR_kill, pid, SIGUSR1);

	/* Install handler. */
	install_handler(SIGUSR1, handler_usr1);

	/*
	 * Sigsuspend with mask=0 (SIGUSR1 unmasked in temp mask)
	 * Note: Ember restores the original mask before returning to userspace,
	 * so the handler may not fire until we explicitly unblock.
	 */
	unsigned long mask = 0;
	long ret = sys2(__NR_rt_sigsuspend, (long)&mask, 8);
	check(ret == -EINTR, "rt_sigsuspend returns -EINTR");

	/* Unblock SIGUSR1 so the pending signal is delivered through the handler. */
	unsigned long unblock_mask = SIGUSR1_MASK;
	sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&unblock_mask, 0, 8);
	check(handler_count > 0, "SIGUSR1 handler fired after unblock");
}

static void
test_tkill_self(void)
{
	msg("test_tkill_self");

	handler_count = 0;
	install_handler(SIGUSR1, handler_usr1);

	long pid = sys0(__NR_getpid);
	long ret = sys2(__NR_tkill, pid, SIGUSR1);
	check(ret == 0, "tkill returns 0");
	check(handler_count > 0, "SIGUSR1 handler ran via tkill");
}

static void
test_tkill_invalid(void)
{
	msg("test_tkill_invalid");

	long ret = sys2(__NR_tkill, 99999, SIGUSR1);
	check(ret == -ESRCH, "tkill to nonexistent pid returns -ESRCH");
}

static void
test_tgkill_self(void)
{
	msg("test_tgkill_self");

	handler_count = 0;
	install_handler(SIGUSR1, handler_usr1);

	long pid = sys0(__NR_getpid);
	long ret = sys3(__NR_tgkill, pid, pid, SIGUSR1);
	check(ret == 0, "tgkill returns 0");
	check(handler_count > 0, "SIGUSR1 handler ran via tgkill");
}

static void
test_tgkill_sig_zero(void)
{
	msg("test_tgkill_sig_zero");

	handler_count = 0;
	install_handler(SIGUSR1, handler_usr1);

	long pid = sys0(__NR_getpid);
	long ret = sys3(__NR_tgkill, pid, pid, 0);
	check(ret == 0, "tgkill sig=0 returns 0");
	check(handler_count == 0, "handler did NOT run for sig=0");
}

static void
test_sigaltstack_stub(void)
{
	msg("test_sigaltstack_stub");

	long ret = sys2(__NR_sigaltstack, 0, 0);
	check(ret == 0, "sigaltstack(NULL, NULL) returns 0");
}

int
main(void)
{
	msg("=== signal_adv tests ===\n");
	test_sigpending_basic();
	test_sigpending_empty();
	test_sigsuspend_wakeup();
	test_sigsuspend_unmask_delivery();
	test_tkill_self();
	test_tkill_invalid();
	test_tgkill_self();
	test_tgkill_sig_zero();
	test_sigaltstack_stub();
	test_done();
}
