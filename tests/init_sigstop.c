/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * SIGSTOP/SIGCONT test -- exercises SIGSTOP, SIGCONT, waitpid with
 * WUNTRACED/WCONTINUED, and self-stop behavior.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_write      1
#define __NR_nanosleep  35
#define __NR_getpid     39
#define __NR_fork       57
#define __NR_exit       60
#define __NR_wait4      61
#define __NR_kill       62

/* Signal numbers. */
#define SIGKILL  9
#define SIGSTOP 19
#define SIGCONT 18

/* Wait flags. */
#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  8

/* Status macros. */
#define WIFSTOPPED(s)    (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)      (((s) >> 8) & 0xff)
#define WIFCONTINUED(s)  ((s) == 0xffff)
#define WIFEXITED(s)     (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)   (((s) >> 8) & 0xff)

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

/* Small delay for synchronization. */
static void
msleep(long ms)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, ms * 1000000};
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* ---- Test 1: SIGSTOP child, wait with WUNTRACED, then SIGCONT and exit ---- */
static void
test_sigstop_child(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "stop_child: fork");
		return;
	}
	if (pid == 0) {
		/* Child: loop with nanosleep, then exit 42. */
		for (int i = 0; i < 20; i++)
			msleep(50);
		sys1(__NR_exit, 42);
		__builtin_unreachable();
	}
	/* Parent: let child start. */
	msleep(30);

	/* Send SIGSTOP to child. */
	long r = sys2(__NR_kill, pid, SIGSTOP);
	check(r == 0, "stop_child: kill SIGSTOP");

	/* Wait for child to stop. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, WUNTRACED, 0);
	check(wpid == pid, "stop_child: wait4 returned correct pid");
	check(WIFSTOPPED(status), "stop_child: WIFSTOPPED");
	check(WSTOPSIG(status) == SIGSTOP, "stop_child: WSTOPSIG == SIGSTOP");

	/* Send SIGCONT to resume child. */
	r = sys2(__NR_kill, pid, SIGCONT);
	check(r == 0, "stop_child: kill SIGCONT");

	/* Wait for child to exit normally. */
	status = 0;
	wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "stop_child: wait4 after cont returned pid");
	check(WIFEXITED(status), "stop_child: WIFEXITED after cont");
	check(WEXITSTATUS(status) == 42, "stop_child: exit code 42");
}

/* ---- Test 2: Child sends SIGSTOP to itself ---- */
static void
test_sigstop_self(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "stop_self: fork");
		return;
	}
	if (pid == 0) {
		/* Child: stop itself. */
		long cpid = sys0(__NR_getpid);
		sys2(__NR_kill, cpid, SIGSTOP);
		/* After being continued, exit with code 7. */
		sys1(__NR_exit, 7);
		__builtin_unreachable();
	}
	/* Parent: wait for child to stop. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, WUNTRACED, 0);
	check(wpid == pid, "stop_self: wait4 returned correct pid");
	check(WIFSTOPPED(status), "stop_self: WIFSTOPPED");
	check(WSTOPSIG(status) == SIGSTOP, "stop_self: WSTOPSIG == SIGSTOP");

	/* Send SIGCONT so child resumes and exits. */
	long r = sys2(__NR_kill, pid, SIGCONT);
	check(r == 0, "stop_self: kill SIGCONT");

	/* Wait for child to exit. */
	status = 0;
	wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "stop_self: wait4 after cont returned pid");
	check(WIFEXITED(status), "stop_self: WIFEXITED after cont");
	check(WEXITSTATUS(status) == 7, "stop_self: exit code 7");
}

/* ---- Test 3: SIGCONT without prior SIGSTOP is a no-op ---- */
static void
test_sigcont_without_stop(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "cont_noop: fork");
		return;
	}
	if (pid == 0) {
		/* Child: sleep a bit and exit. */
		msleep(100);
		sys1(__NR_exit, 13);
		__builtin_unreachable();
	}
	/* Parent: let child start. */
	msleep(20);

	/* Send SIGCONT without prior SIGSTOP -- should be harmless. */
	long r = sys2(__NR_kill, pid, SIGCONT);
	check(r == 0, "cont_noop: kill SIGCONT");

	/* Child should still exit normally. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "cont_noop: wait4 returned correct pid");
	check(WIFEXITED(status), "cont_noop: WIFEXITED");
	check(WEXITSTATUS(status) == 13, "cont_noop: exit code 13");
}

int
main(void)
{
	msg("=== sigstop tests ===\n");

	test_sigstop_child();
	test_sigstop_self();
	test_sigcont_without_stop();

	test_done();
	return 0;
}
