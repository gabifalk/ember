/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Signal mask test -- exercises rt_sigprocmask SIG_BLOCK / SIG_UNBLOCK,
 * blocked signal pending behavior, and signal mask inheritance across fork.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_rt_sigaction    13
#define __NR_rt_sigprocmask  14
#define __NR_rt_sigreturn    15
#define __NR_getpid          39
#define __NR_fork            57
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_kill            62

/* Signal numbers. */
#define SIGUSR1 10

/* Signal mask bits. */
#define SIGUSR1_MASK (1UL << 10)

/* Sigprocmask how values. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* Flags. */
#define SA_RESTORER 0x04000000

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

/* Install SIGUSR1 handler. */
static long
install_sigusr1(void)
{
	struct kernel_sigaction sa;
	sa.handler = handler_usr1;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;
	return sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
}

/* ---- Test 1: SIG_BLOCK prevents handler from running ---- */
static void
test_sig_block(void)
{
	long r = install_sigusr1();
	check(r == 0, "block: install handler");

	handler_count = 0;

	/* Block SIGUSR1. */
	unsigned long mask = SIGUSR1_MASK;
	r = sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8);
	check(r == 0, "block: sigprocmask SIG_BLOCK");

	/* Send SIGUSR1 to self. */
	long pid = sys0(__NR_getpid);
	r = sys2(__NR_kill, pid, SIGUSR1);
	check(r == 0, "block: kill self");

	/* Handler should NOT have run (signal is pending but blocked) */
	check(handler_count == 0, "block: handler did not run while blocked");

	/* Unblock to clean up -- handler will run here. */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "block: sigprocmask SIG_UNBLOCK");

	/* Handler should have run now. */
	check(handler_count == 1, "block: handler ran after unblock");
}

/* ---- Test 2: SIG_UNBLOCK delivers pending signal immediately ---- */
static void
test_sig_unblock(void)
{
	long r = install_sigusr1();
	check(r == 0, "unblock: install handler");

	handler_count = 0;

	/* Block SIGUSR1. */
	unsigned long mask = SIGUSR1_MASK;
	r = sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8);
	check(r == 0, "unblock: sigprocmask SIG_BLOCK");

	/* Send SIGUSR1 to self. */
	long pid = sys0(__NR_getpid);
	sys2(__NR_kill, pid, SIGUSR1);

	check(handler_count == 0, "unblock: handler not run while blocked");

	/* Unblock -- handler should run immediately. */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "unblock: sigprocmask SIG_UNBLOCK");
	check(handler_count == 1,
	      "unblock: handler ran immediately after unblock");
}

/* ---- Test 3: Blocked signals coalesce ---- */
static void
test_blocked_coalesce(void)
{
	long r = install_sigusr1();
	check(r == 0, "coalesce: install handler");

	handler_count = 0;

	/* Block SIGUSR1. */
	unsigned long mask = SIGUSR1_MASK;
	r = sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8);
	check(r == 0, "coalesce: sigprocmask SIG_BLOCK");

	/* Send SIGUSR1 twice. */
	long pid = sys0(__NR_getpid);
	sys2(__NR_kill, pid, SIGUSR1);
	sys2(__NR_kill, pid, SIGUSR1);

	check(handler_count == 0, "coalesce: handler not run while blocked");

	/* Unblock -- handler should run at least once (standard signals coalesce) */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "coalesce: sigprocmask SIG_UNBLOCK");
	check(handler_count >= 1,
	      "coalesce: handler ran at least once after unblock");
}

/* ---- Test 4: Signal mask inherited across fork ---- */
static void
test_mask_inherited_fork(void)
{
	long r = install_sigusr1();
	check(r == 0, "fork: install handler");

	handler_count = 0;

	/* Block SIGUSR1 in parent. */
	unsigned long mask = SIGUSR1_MASK;
	r = sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8);
	check(r == 0, "fork: parent sigprocmask SIG_BLOCK");

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "fork: fork failed");
		return;
	}
	if (pid == 0) {
		/* Child: handler_count is a copy (COW), starts at 0. */
		handler_count = 0;

		/* Send SIGUSR1 to self -- should be blocked (mask inherited) */
		long cpid = sys0(__NR_getpid);
		sys2(__NR_kill, cpid, SIGUSR1);

		/* Handler should NOT have run. */
		if (handler_count != 0) {
			msg("  FAIL fork: child handler ran while blocked\n");
			sys1(__NR_exit, 1);
			__builtin_unreachable();
		}
		msg("  PASS fork: child handler did not run while blocked\n");

		/* Unblock -- handler should run. */
		unsigned long cmask = SIGUSR1_MASK;
		sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&cmask, 0, 8);

		if (handler_count < 1) {
			msg("  FAIL fork: child handler did not run after unblock\n");
			sys1(__NR_exit, 1);
			__builtin_unreachable();
		}
		msg("  PASS fork: child handler ran after unblock\n");

		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "fork: wait4 returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "fork: child exited successfully");

	/* Unblock in parent to clean up. */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "fork: parent sigprocmask SIG_UNBLOCK");
}

int
main(void)
{
	msg("=== signal mask tests ===\n");

	test_sig_block();
	test_sig_unblock();
	test_blocked_coalesce();
	test_mask_inherited_fork();

	test_done();
	return 0;
}
