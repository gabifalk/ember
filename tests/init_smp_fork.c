/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * SMP fork/exit stress test -- exercises idle-CPU CR3 safety (P1) and
 * single-schedule invariant (P4) under 4 CPUs. Validates fix 3d63786
 * (kernel_idle_cr3 for idle CPUs after process exit).
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI). */
#define __NR_write   1
#define __NR_fork   57
#define __NR_exit   60
#define __NR_wait4  61

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
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10):"rcx", "r11", "memory");
	return ret;
}

static long
do_fork(void)
{
	return sys0(__NR_fork);
}

static void
do_exit(long code)
{
	sys1(__NR_exit, code);
	__builtin_unreachable();
}

static long
do_wait(long pid, int *status)
{
	return sys4(__NR_wait4, pid, (long)status, 0, 0);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: Rapid fork-exit (P1: dangling CR3).
 *
 * 100 rounds of fork-then-immediately-exit. With 4 CPUs, idle CPUs must
 * safely handle CR3 when the process whose PML4 they were using exits.
 * If CR3 dangles, this triple-faults.
 * ---------------------------------------------------------------------------
 */
static void
test_rapid_fork_exit(void)
{
	int ok = 1;

	for (int i = 0; i < 100; i++) {
		long pid = do_fork();
		if (pid < 0) {
			ok = 0;
			break;
		}
		if (pid == 0) {
			do_exit(0);
		}
		int status = 0;
		long rpid = do_wait(pid, &status);
		if (rpid != pid)
			ok = 0;
	}

	check(ok, "rapid_fork_exit: 100 rounds survived (no triple fault)");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Concurrent children (P4: double schedule).
 *
 * Fork 16 children that each busy-loop briefly then exit with i & 0xff.
 * Parent reaps all, verifies each exit code and no signals.
 * ---------------------------------------------------------------------------
 */
static void
test_concurrent_children(void)
{
	long pids[16];
	int ok = 1;

	for (int i = 0; i < 16; i++) {
		long pid = do_fork();
		if (pid < 0) {
			check(0, "concurrent_children: fork failed");
			for (int j = 0; j < i; j++) {
				int st = 0;
				do_wait(pids[j], &st);
			}
			return;
		}
		if (pid == 0) {
			/* Busy-loop briefly to stay scheduled. */
			volatile int sink = 0;
			for (int k = 0; k < 50000; k++)
				sink += k;
			(void)sink;
			do_exit(i & 0xff);
		}
		pids[i] = pid;
	}

	/* Reap all children. */
	int seen[16] = { 0 };
	for (int i = 0; i < 16; i++) {
		int status = 0;
		long rpid = do_wait(-1, &status);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		int sig = status & 0x7f;
		/* Find which child this was. */
		for (int j = 0; j < 16; j++) {
			if (pids[j] == rpid) {
				if (code != (j & 0xff))
					ok = 0;
				if (sig != 0)
					ok = 0;
				seen[j] = 1;
				break;
			}
		}
	}

	for (int i = 0; i < 16; i++) {
		if (!seen[i])
			ok = 0;
	}

	check(ok, "concurrent_children: all 16 exit codes correct, no signals");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Fork chain (proc table slot reuse under SMP).
 *
 * Depth-20 chain: each iteration forks, child continues the loop (becomes
 * new parent), parent waits for child then exits. Tests proc table slot
 * reuse under SMP.
 * ---------------------------------------------------------------------------
 */
static void
test_fork_chain(void)
{
	int depth = 20;
	int ok = 1;

	for (int i = 0; i < depth; i++) {
		long pid = do_fork();
		if (pid < 0) {
			ok = 0;
			break;
		}
		if (pid > 0) {
			/* Parent: wait for child then exit (unless original). */
			int status = 0;
			long rpid = do_wait(pid, &status);
			if (rpid != pid)
				ok = 0;
			int code = (status >> 8) & 0xff;
			if (code != 0)
				ok = 0;
			if (i == 0) {
				/* Original caller -- report and return. */
				check(ok, "fork_chain: depth-20 chain completed");
				return;
			}
			do_exit(ok ? 0 : 1);
		}
		/* Child continues the loop as new parent. */
	}

	/* Reached end of chain (deepest child). */
	do_exit(0);
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== SMP fork stress (P1/P4) ===\n");

	test_rapid_fork_exit();
	test_concurrent_children();
	test_fork_chain();

	test_done();
	return 0;
}
