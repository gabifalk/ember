/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Signal + fork interaction tests -- verifies signal delivery across fork,
 * cross-process signal sending, and signal mask inheritance with blocking.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_write           1
#define __NR_rt_sigaction    13
#define __NR_rt_sigprocmask  14
#define __NR_rt_sigreturn    15
#define __NR_getpid          39
#define __NR_fork            57
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_kill            62
#define __NR_getppid         110

/* Signal numbers. */
#define SIGUSR1 10
#define SIGUSR2 12

/* Signal mask bits -- Ember uses bit N for signal N (not N-1) */
#define SIGUSR1_MASK (1UL << 10)
#define SIGUSR2_MASK (1UL << 12)

/* Sigprocmask how values. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1

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
static volatile int usr1_count = 0;
static volatile int usr2_count = 0;

static void
handler_usr1(int sig)
{
	(void)sig;
	usr1_count++;
}

static void
handler_usr2(int sig)
{
	(void)sig;
	usr2_count++;
}

/* Install a signal handler. */
static long
install_handler(int signum, void (*handler) (int))
{
	struct kernel_sigaction sa;
	sa.handler = handler;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;
	sa.mask = 0;
	return sys4(__NR_rt_sigaction, signum, (long)&sa, 0, 8);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: Parent signal after fork
 *
 * Install SIGUSR1 handler. Fork. Parent sends SIGUSR1 to itself right after
 * fork returns. Verify parent's handler ran. Wait for child (which exits 0).
 * Verify child exited cleanly.
 * ---------------------------------------------------------------------------
 */
static void
test_parent_signal_after_fork(void)
{
	long r = install_handler(SIGUSR1, handler_usr1);
	check(r == 0, "parent_sig: install handler");

	usr1_count = 0;

	long pid = sys0(__NR_fork);
	check(pid >= 0, "parent_sig: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: just exit 0. */
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: send SIGUSR1 to self right after fork. */
	long self = sys0(__NR_getpid);
	r = sys2(__NR_kill, self, SIGUSR1);
	check(r == 0, "parent_sig: kill self");
	check(usr1_count == 1, "parent_sig: handler ran in parent");

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "parent_sig: wait4 returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "parent_sig: child exited 0");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Child sends signal to parent
 *
 * Install SIGUSR1 handler. Fork. Child sends SIGUSR1 to parent via
 * kill(getppid(), SIGUSR1), then exits. Parent waits for child and verifies
 * its signal handler ran.
 * ---------------------------------------------------------------------------
 */
static void
test_child_signals_parent(void)
{
	long r = install_handler(SIGUSR1, handler_usr1);
	check(r == 0, "child_sig: install handler");

	usr1_count = 0;

	long pid = sys0(__NR_fork);
	check(pid >= 0, "child_sig: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: send SIGUSR1 to parent. */
		long ppid = sys0(__NR_getppid);
		sys2(__NR_kill, ppid, SIGUSR1);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: wait for child to finish (signal may arrive during wait) */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "child_sig: wait4 returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "child_sig: child exited 0");

	/* Handler should have run (child sent us SIGUSR1) */
	check(usr1_count >= 1, "child_sig: parent handler ran");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Blocked signal mask inherited across fork
 *
 * Install SIGUSR2 handler. Block SIGUSR2. Fork. Child inherits blocked mask.
 * Child sends SIGUSR2 to itself -- handler should NOT run (blocked).
 * Child unblocks -- handler runs. Child exits 0. Parent waits, verifies
 * child exited 0.
 * ---------------------------------------------------------------------------
 */
static void
test_blocked_mask_inherited(void)
{
	long r = install_handler(SIGUSR2, handler_usr2);
	check(r == 0, "inherit: install SIGUSR2 handler");

	/* Block SIGUSR2 in parent before fork. */
	unsigned long mask = SIGUSR2_MASK;
	r = sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8);
	check(r == 0, "inherit: parent block SIGUSR2");

	long pid = sys0(__NR_fork);
	check(pid >= 0, "inherit: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: usr2_count is a COW copy, starts at 0. */
		usr2_count = 0;

		/* Send SIGUSR2 to self -- should be blocked (mask inherited) */
		long cpid = sys0(__NR_getpid);
		sys2(__NR_kill, cpid, SIGUSR2);

		/* Handler should NOT have run. */
		if (usr2_count != 0) {
			msg("  FAIL inherit: child handler ran while blocked\n");
			sys1(__NR_exit, 1);
			__builtin_unreachable();
		}
		msg("  PASS inherit: child handler did not run while blocked\n");

		/* Unblock SIGUSR2 -- handler should run now. */
		unsigned long cmask = SIGUSR2_MASK;
		sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&cmask, 0, 8);

		if (usr2_count < 1) {
			msg("  FAIL inherit: child handler did not run after unblock\n");
			sys1(__NR_exit, 1);
			__builtin_unreachable();
		}
		msg("  PASS inherit: child handler ran after unblock\n");

		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "inherit: wait4 returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "inherit: child exited 0");

	/* Unblock in parent to clean up. */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "inherit: parent unblock SIGUSR2");
}

int
main(void)
{
	msg("=== signal+fork tests ===\n");

	test_parent_signal_after_fork();
	test_child_signals_parent();
	test_blocked_mask_inherited();

	test_done();
	return 0;
}
