/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Scheduler and process table coverage tests -- zombie reaping, sched_yield
 * under load, process slot recycling, orphan reparenting, and WNOHANG.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_sched_yield 24
#define __NR_nanosleep   35
#define __NR_getpid      39
#define __NR_fork        57
#define __NR_exit        60
#define __NR_wait4       61

/* Constants. */
#define WNOHANG  1
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12

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
do_wait4(long pid, int *status, int options)
{
	return sys4(__NR_wait4, pid, (long)status, (long)options, 0);
}

static long
do_sched_yield(void)
{
	return sys0(__NR_sched_yield);
}

static long
do_getpid(void)
{
	return sys0(__NR_getpid);
}

static void
do_sleep_ms(long ms)
{
	long ts[2];
	ts[0] = ms / 1000;
	ts[1] = (ms % 1000) * 1000000L;
	sys2(__NR_nanosleep, (long)ts, 0);
}

/* ---- Test 1: Zombie reaping -- many children ---- */
static void
test_zombie_reaping(void)
{
	msg("test_zombie_reaping\n");

	int reaped = 0;
	int seen[30];
	for (int i = 0; i < 30; i++)
		seen[i] = 0;

	/* Fork 30 children, each exits with its index as exit code. */
	for (int i = 0; i < 30; i++) {
		long pid = do_fork();
		if (pid == 0)
			do_exit(i);
		check(pid > 0, "zombie: fork succeeded");
	}

	/* Reap all 30. */
	for (int i = 0; i < 30; i++) {
		int status = 0;
		long r = do_wait4(-1, &status, 0);
		if (r > 0) {
			reaped++;
			int code = (status >> 8) & 0xff;
			if (code >= 0 && code < 30)
				seen[code] = 1;
		}
	}

	check(reaped == 30, "zombie: reaped all 30 children");

	int all_seen = 1;
	for (int i = 0; i < 30; i++) {
		if (!seen[i]) {
			all_seen = 0;
			break;
		}
	}
	check(all_seen, "zombie: all exit codes 0..29 seen");

	/* No more children -- WNOHANG should return -ECHILD. */
	long r = do_wait4(-1, (void *)0, WNOHANG);
	check(r == -ECHILD, "zombie: no leftover zombies");
}

/* ---- Test 2: sched_yield under load ---- */
static void
test_yield_under_load(void)
{
	msg("test_yield_under_load\n");

	for (int i = 0; i < 4; i++) {
		long pid = do_fork();
		check(pid >= 0, "yield: fork succeeded");
		if (pid == 0) {
			/* Child: yield 100 times, exit non-zero if any fails. */
			for (int j = 0; j < 100; j++) {
				long r = do_sched_yield();
				if (r != 0)
					do_exit(1);
			}
			do_exit(0);
		}
	}

	/* Wait for all 4 children. */
	int all_ok = 1;
	for (int i = 0; i < 4; i++) {
		int status = 0;
		long r = do_wait4(-1, &status, 0);
		if (r <= 0 || ((status >> 8) & 0xff) != 0)
			all_ok = 0;
	}
	check(all_ok, "yield: all children exited successfully");
}

/* ---- Test 3: Fork-wait cycle (process slot recycling) ---- */
static void
test_fork_wait_cycle(void)
{
	msg("test_fork_wait_cycle\n");

	int ok = 1;
	for (int i = 0; i < 50; i++) {
		long pid = do_fork();
		if (pid < 0) {
			ok = 0;
			break;
		}
		if (pid == 0)
			do_exit(0);
		int status = 0;
		long r = do_wait4(pid, &status, 0);
		if (r != pid) {
			ok = 0;
			break;
		}
	}
	check(ok, "fork-wait-cycle: 50 iterations succeeded");
}

/* ---- Test 4: Orphan reparenting ---- */
static void
test_orphan_reparent(void)
{
	msg("test_orphan_reparent\n");

	long my_pid = do_getpid();

	long child_a = do_fork();
	check(child_a >= 0, "orphan: fork child A");
	if (child_a == 0) {
		/* Child A: fork grandchild B, then exit immediately. */
		long child_b = do_fork();
		if (child_b == 0) {
			/* Grandchild B: sleep briefly so parent A exits first, then exit. */
			do_sleep_ms(50);
			do_exit(42);
		}
		do_exit(0);
	}
	/* Parent: wait for child A. */
	int status = 0;
	long r = do_wait4(child_a, &status, 0);
	check(r == child_a, "orphan: reaped child A");

	/*
	 * Grandchild B should be reparented to us (init, PID 1).
	 * Wait for it.
	 */
	status = 0;
	r = do_wait4(-1, &status, 0);
	check(r > 0, "orphan: reaped grandchild B");
	int code = (status >> 8) & 0xff;
	check(code == 42, "orphan: grandchild exit code is 42");
}

/* ---- Test 5: Wait with WNOHANG on running children ---- */
static void
test_wnohang_running(void)
{
	msg("test_wnohang_running\n");

	long pid = do_fork();
	check(pid >= 0, "wnohang: fork succeeded");
	if (pid == 0) {
		/* Child: sleep 100ms then exit. */
		do_sleep_ms(100);
		do_exit(7);
	}
	/* Immediately try WNOHANG -- child should still be running. */
	int status = 0;
	long r = do_wait4(-1, &status, WNOHANG);
	check(r == 0, "wnohang: returns 0 while child runs");

	/* Now blocking wait -- should return child PID. */
	status = 0;
	r = do_wait4(-1, &status, 0);
	check(r == pid, "wnohang: blocking wait returns child pid");
	int code = (status >> 8) & 0xff;
	check(code == 7, "wnohang: child exit code is 7");
}

int
main(void)
{
	msg("=== scheduler and process table tests ===\n");

	test_zombie_reaping();
	test_yield_under_load();
	test_fork_wait_cycle();
	test_orphan_reparent();
	test_wnohang_running();

	test_done();
	return 0;
}
