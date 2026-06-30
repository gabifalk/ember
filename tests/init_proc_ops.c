/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 * Consolidated process ops tests -- wait_edge, proc_tree, pgroup.
 */

#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_write          1
#define __NR_rt_sigaction   13
#define __NR_rt_sigreturn   15
#define __NR_sched_yield    24
#define __NR_nanosleep      35
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_setpgid        109
#define __NR_getppid        110
#define __NR_getpgrp        111
#define __NR_getpgid        121

/* Flags. */
#define WNOHANG   1

/* Error codes. */
#define ECHILD    10

/* Signal numbers. */
#define SIGUSR1   10

/* SA_RESTORER flag. */
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

/* Helper wrappers (from wait_edge) */
static long
do_fork(void)
{
	return sys0(__NR_fork);
}

static void
do_exit(int code)
{
	sys1(__NR_exit, code);
	__builtin_unreachable();
}

static long
do_wait4(long pid, int *status, int options)
{
	return sys4(__NR_wait4, pid, (long)status, options, 0);
}

static long
do_getpid(void)
{
	return sys0(__NR_getpid);
}

static void
do_yield(void)
{
	sys0(__NR_sched_yield);
}

/* Sleep for a given number of milliseconds (rough, via nanosleep) */
static void
do_sleep_ms(long ms)
{
	/* Struct timespec { long tv_sec; long tv_nsec; }. */
	long ts[2];
	ts[0] = ms / 1000;
	ts[1] = (ms % 1000) * 1000000L;
	sys2(__NR_nanosleep, (long)ts, 0);
}

/* Helper wrappers (from proc_tree) */
static long
do_wait(long pid, int *status)
{
	return sys4(__NR_wait4, pid, (long)status, 0, 0);
}

static long
do_getppid(void)
{
	return sys0(__NR_getppid);
}

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

/* Helper wrappers (from pgroup) */

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

/* Small delay to let children set up. */
static void
tiny_sleep(void)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, 20000000};		/* 20Ms. */
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* ---- Signal state for child processes ---- */
static volatile int got_signal;

static void
sigusr1_handler(int sig)
{
	(void)sig;
	got_signal = 1;
}

/*
 * ===========================================================================
 * wait_edge tests
 * ===========================================================================
 */

/* ---- Test 1: WNOHANG polling ---- */
static void
test_wnohang_poll(void)
{
	long pid = do_fork();
	if (pid == 0) {
		/* Child: sleep briefly then exit. */
		do_sleep_ms(100);
		do_exit(7);
	}
	/* Parent: immediately try WNOHANG -- child should still be running. */
	int status = -1;
	long r = do_wait4(pid, &status, WNOHANG);
	int poll_ok = (r == 0);	/* 0 Means no child exited yet. */

	/* Now do a blocking wait. */
	status = -1;
	r = do_wait4(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(poll_ok && r == pid && code == 7, "wnohang-poll");
}

/* ---- Test 2: Multiple children exit ordering ---- */
static void
test_multi_child(void)
{
	int codes[] = { 41, 42, 43, 44 };
	long pids[4];
	int i;

	for (i = 0; i < 4; i++) {
		long pid = do_fork();
		if (pid == 0) {
			do_exit(codes[i]);
		}
		pids[i] = pid;
	}

	/* Wait for all children (any order) */
	int collected[4] = { 0, 0, 0, 0 };
	int all_ok = 1;
	for (i = 0; i < 4; i++) {
		int status = 0;
		long r = do_wait4(-1, &status, 0);
		if (r < 0) {
			all_ok = 0;
			break;
		}
		int code = (status >> 8) & 0xff;

		/* Find which child this was. */
		int found = 0;
		for (int j = 0; j < 4; j++) {
			if (r == pids[j] && code == codes[j] && !collected[j]) {
				collected[j] = 1;
				found = 1;
				break;
			}
		}
		if (!found)
			all_ok = 0;
	}

	/* Verify all collected. */
	for (i = 0; i < 4; i++) {
		if (!collected[i])
			all_ok = 0;
	}
	check(all_ok, "multi-child-order");
}

/* ---- Test 3: Double-wait (wait again for already-reaped child) ---- */
static void
test_double_wait(void)
{
	long pid = do_fork();
	if (pid == 0) {
		do_exit(55);
	}

	int status = 0;
	long r = do_wait4(pid, &status, 0);
	int first_ok = (r == pid && ((status >> 8) & 0xff) == 55);

	/* Second wait for same PID should fail with -ECHILD. */
	status = 0;
	r = do_wait4(pid, &status, 0);
	int second_ok = (r == -ECHILD);

	check(first_ok && second_ok, "double-wait");
}

/* ---- Test 4: Wait on non-child PID ---- */
static void
test_wait_nonchild(void)
{
	int status = 0;
	long r = do_wait4(9999, &status, 0);
	check(r == -ECHILD, "wait-nonchild");
}

/* ---- Test 5: Orphan cleanup (grandchild survives parent's exit) ---- */
static void
test_orphan_cleanup(void)
{
	/*
	 * We fork a child. The child forks a grandchild, then exits.
	 * The grandchild should be reparented to init (us, PID 1) and
	 * we should be able to wait for it.
	 */
	long my_pid = do_getpid();

	long child = do_fork();
	if (child == 0) {
		/* Child: fork grandchild, then exit immediately. */
		long gc = do_fork();
		if (gc == 0) {
			/* Grandchild: sleep a bit then exit. */
			do_sleep_ms(100);
			do_exit(77);
		}
		/* Child exits, orphaning grandchild. */
		do_exit(0);
	}
	/* Parent: wait for child first. */
	int status = 0;
	long r = do_wait4(child, &status, 0);
	int child_ok = (r == child && ((status >> 8) & 0xff) == 0);

	/*
	 * Now wait for the orphaned grandchild (reparented to us)
	 * Use wait4(-1, ...) since we don't know the grandchild's PID
	 * Give it a moment to finish.
	 */
	do_sleep_ms(200);

	status = 0;
	r = do_wait4(-1, &status, 0);
	int gc_code = (status >> 8) & 0xff;
	int gc_ok = (r > 0 && gc_code == 77);

	check(child_ok && gc_ok, "orphan-cleanup");
}

/*
 * ===========================================================================
 * proc_tree tests
 * ===========================================================================
 */

/*
 * ---------------------------------------------------------------------------
 * Test 1: getpid/getppid basic relationship
 *
 * Fork a child. Child verifies getppid() matches parent's pid.
 * Parent waits and verifies child exited 0.
 * ---------------------------------------------------------------------------
 */
static void
test_pid_ppid(void)
{
	long parent_pid = do_getpid();
	check(parent_pid > 0, "pidppid: getpid ok");

	long pid = do_fork();
	check(pid >= 0, "pidppid: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child. */
		long my_pid = do_getpid();
		long my_ppid = do_getppid();
		if (my_pid <= 0)
			do_exit(1);
		if (my_ppid != parent_pid)
			do_exit(2);
		/* Pid should differ from parent. */
		if (my_pid == parent_pid)
			do_exit(3);
		do_exit(0);
	}
	/* Parent: wait for child. */
	int status = 0;
	long rpid = do_wait(pid, &status);
	check(rpid == pid, "pidppid: wait returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "pidppid: child verified getpid/getppid");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Grandchild wait semantics
 *
 * Parent forks child A. Child A forks grandchild B.
 * Grandchild B exits 33. Child A waits for B (expects 33), then exits 44.
 * Parent waits for A (expects 44). Parent cannot wait for grandchild B.
 * ---------------------------------------------------------------------------
 */
static void
test_grandchild(void)
{
	long pid_a = do_fork();
	check(pid_a >= 0, "grandchild: fork child A ok");
	if (pid_a < 0)
		return;

	if (pid_a == 0) {
		/* Child A: fork grandchild B. */
		long pid_b = do_fork();
		if (pid_b < 0)
			do_exit(99);

		if (pid_b == 0) {
			/* Grandchild B. */
			do_exit(33);
		}
		/* Child A: wait for grandchild B. */
		int st = 0;
		long r = do_wait(pid_b, &st);
		if (r != pid_b)
			do_exit(98);
		int bc = (st >> 8) & 0xff;
		if (bc != 33)
			do_exit(97);

		do_exit(44);
	}
	/* Parent: wait for child A. */
	int status = 0;
	long rpid = do_wait(pid_a, &status);
	check(rpid == pid_a, "grandchild: wait for child A ok");
	int code = (status >> 8) & 0xff;
	check(code == 44,
	      "grandchild: child A exited 44 (reaped grandchild ok)");

	/*
	 * Parent should NOT be able to wait for grandchild B (not a direct child).
	 * Any additional wait should return -ECHILD since we have no more children.
	 */
	int st2 = 0;
	long r2 = sys4(__NR_wait4, -1, (long)&st2, WNOHANG, 0);
	check(r2 == -ECHILD, "grandchild: parent cannot wait for grandchild");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Orphan reparenting to init (pid 1)
 *
 * Parent (pid 1) forks child A. Child A forks grandchild B, then exits
 * immediately. Grandchild B becomes an orphan, reparented to init (pid 1).
 * Grandchild B sleeps 100ms then exits 55.
 * Parent waits for child A first, then waits for grandchild B (now
 * reparented). Verifies grandchild's exit status is 55.
 * ---------------------------------------------------------------------------
 */
static void
test_orphan_reparent(void)
{
	long my_pid = do_getpid();
	check(my_pid == 1, "orphan: we are init (pid 1)");

	long pid_a = do_fork();
	check(pid_a >= 0, "orphan: fork child A ok");
	if (pid_a < 0)
		return;

	if (pid_a == 0) {
		/* Child A: fork grandchild B, then exit immediately. */
		long pid_b = do_fork();
		if (pid_b < 0)
			do_exit(99);

		if (pid_b == 0) {
			/* Grandchild B: sleep 100ms so child A exits first. */
			msleep(100);
			do_exit(55);
		}
		/* Child A: exit immediately, orphaning grandchild B. */
		do_exit(0);
	}
	/* Parent (init): wait for child A first. */
	int status_a = 0;
	long rpid_a = do_wait(pid_a, &status_a);
	check(rpid_a == pid_a, "orphan: wait for child A ok");
	int code_a = (status_a >> 8) & 0xff;
	check(code_a == 0, "orphan: child A exited 0");

	/*
	 * Now wait for grandchild B (reparented to us, pid 1)
	 * Use pid=-1 since we don't know grandchild's pid directly.
	 */
	int status_b = 0;
	long rpid_b = do_wait(-1, &status_b);
	check(rpid_b > 0, "orphan: wait for reparented grandchild ok");
	int code_b = (status_b >> 8) & 0xff;
	check(code_b == 55, "orphan: grandchild exited 55 after reparenting");
}

/*
 * ===========================================================================
 * pgroup tests
 * ===========================================================================
 */

/* ---- Test 1: getpgid(0) returns own pgid (same as pid for init) ---- */
static void
test_getpgid(void)
{
	long pid = sys0(__NR_getpid);
	long pgid = sys1(__NR_getpgid, 0);
	check(pgid == pid, "getpgid(0) == getpid()");
}

/* ---- Test 2: setpgid(child, child) creates new group ---- */
static void
test_setpgid_parent(void)
{
	long child = sys0(__NR_fork);
	if (child < 0) {
		check(0, "setpgid: fork");
		return;
	}
	if (child == 0) {
		/* Child: sleep briefly then exit. */
		tiny_sleep();
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: move child to its own process group. */
	long r = sys2(__NR_setpgid, child, child);
	check(r == 0, "setpgid(child, child)");

	long cpgid = sys1(__NR_getpgid, child);
	check(cpgid == child, "getpgid(child) == child");

	int status = 0;
	sys4(__NR_wait4, child, (long)&status, 0, 0);
}

/* ---- Test 3: setpgid(0,0) in child sets pgid to own pid ---- */
static void
test_setpgid_self(void)
{
	long child = sys0(__NR_fork);
	if (child < 0) {
		check(0, "setpgid(0,0): fork");
		return;
	}
	if (child == 0) {
		/* Child: set own pgid, verify, exit with result. */
		long r = sys2(__NR_setpgid, 0, 0);
		if (r != 0) {
			sys1(__NR_exit, 1);
			__builtin_unreachable();
		}
		long my_pid = sys0(__NR_getpid);
		long my_pgid = sys1(__NR_getpgid, 0);
		sys1(__NR_exit, (my_pgid == my_pid) ? 0 : 2);
		__builtin_unreachable();
	}
	int status = 0;
	sys4(__NR_wait4, child, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "setpgid(0,0) in child");
}

/* ---- Test 4: kill(-pgid) delivers to process group ---- */
static void
test_kill_pgroup(void)
{
	/* Fork two children into the same new process group. */
	long child1 = sys0(__NR_fork);
	if (child1 < 0) {
		check(0, "kill_pgroup: fork1");
		return;
	}
	if (child1 == 0) {
		/* Child 1: install SIGUSR1 handler, wait for signal, exit with flag. */
		got_signal = 0;
		struct kernel_sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.handler = sigusr1_handler;
		sa.flags = SA_RESTORER;
		sa.restorer = my_restorer;
		sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);

		/* Set own pgid (parent will also do this, but race-safe) */
		sys2(__NR_setpgid, 0, 0);

		/* Wait for signal. */
		for (int i = 0; i < 200 && !got_signal; i++)
			tiny_sleep();

		sys1(__NR_exit, got_signal ? 42 : 1);
		__builtin_unreachable();
	}
	/* Parent: put child1 into its own group (the new pgid) */
	long pgid = child1;
	sys2(__NR_setpgid, child1, pgid);

	long child2 = sys0(__NR_fork);
	if (child2 < 0) {
		check(0, "kill_pgroup: fork2");
		sys2(__NR_kill, child1, 9);	/* Cleanup. */
		sys4(__NR_wait4, child1, 0, 0, 0);
		return;
	}
	if (child2 == 0) {
		/* Child 2: install SIGUSR1 handler, join child1's group, wait. */
		got_signal = 0;
		struct kernel_sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.handler = sigusr1_handler;
		sa.flags = SA_RESTORER;
		sa.restorer = my_restorer;
		sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);

		sys2(__NR_setpgid, 0, pgid);

		for (int i = 0; i < 200 && !got_signal; i++)
			tiny_sleep();

		sys1(__NR_exit, got_signal ? 42 : 1);
		__builtin_unreachable();
	}
	/* Parent: put child2 into same group. */
	sys2(__NR_setpgid, child2, pgid);

	/* Wait for children to set up handlers. */
	tiny_sleep();
	tiny_sleep();

	/* Send signal to the process group. */
	long r = sys2(__NR_kill, -pgid, SIGUSR1);
	check(r == 0, "kill(-pgid, SIGUSR1)");

	/* Wait for both children and check exit codes. */
	int status1 = 0, status2 = 0;
	sys4(__NR_wait4, child1, (long)&status1, 0, 0);
	sys4(__NR_wait4, child2, (long)&status2, 0, 0);

	int code1 = (status1 >> 8) & 0xff;
	int code2 = (status2 >> 8) & 0xff;
	check(code1 == 42, "kill_pgroup: child1 got signal");
	check(code2 == 42, "kill_pgroup: child2 got signal");
}

/* ---- Test 5: getpgrp() == getpgid(0) ---- */
static void
test_getpgrp(void)
{
	long pgrp = sys0(__NR_getpgrp);
	long pgid = sys1(__NR_getpgid, 0);
	check(pgrp == pgid, "getpgrp() == getpgid(0)");
}

/* =========================================================================== */

int
main(void)
{
	msg("=== proc_ops tests ===\n");
	msg("--- wait edge ---\n");
	test_wnohang_poll();
	test_multi_child();
	test_double_wait();
	test_wait_nonchild();
	test_orphan_cleanup();
	msg("--- proc tree ---\n");
	test_pid_ppid();
	test_grandchild();
	test_orphan_reparent();
	msg("--- pgroup ---\n");
	test_getpgid();
	test_setpgid_parent();
	test_setpgid_self();
	test_kill_pgroup();
	test_getpgrp();
	test_done();
	return 0;
}
