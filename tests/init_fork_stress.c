/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Fork stress test -- exercises concurrent forking with COW page handling
 * under SMP. Stresses PMM refcounting, COW fault resolution, process table
 * limits, and nested process hierarchies.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_write   1
#define __NR_getpid 39
#define __NR_fork   57
#define __NR_exit   60
#define __NR_wait4  61

#define EAGAIN 11
#define ENOMEM 12

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

static long
do_getpid(void)
{
	return sys0(__NR_getpid);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: Rapid fork-exit
 *
 * Fork 32 children in a loop, each child immediately exits with a unique
 * code. Parent waits for all and verifies exit codes.
 * ---------------------------------------------------------------------------
 */
static void
test_rapid_fork_exit(void)
{
	long pids[32];
	int ok = 1;

	for (int i = 0; i < 32; i++) {
		long pid = do_fork();
		if (pid < 0) {
			check(0, "rapid_fork: fork failed");
			/* Wait for already-forked children. */
			for (int j = 0; j < i; j++) {
				int st = 0;
				do_wait(pids[j], &st);
			}
			return;
		}
		if (pid == 0) {
			do_exit(i);
		}
		pids[i] = pid;
	}

	/* Wait for all children and verify exit codes. */
	int seen[32] = { 0 };
	for (int i = 0; i < 32; i++) {
		int status = 0;
		long rpid = do_wait(-1, &status);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		/* Find which child this was. */
		for (int j = 0; j < 32; j++) {
			if (pids[j] == rpid) {
				if (code != j)
					ok = 0;
				seen[j] = 1;
				break;
			}
		}
	}

	for (int i = 0; i < 32; i++) {
		if (!seen[i])
			ok = 0;
	}

	check(ok, "rapid_fork: all 32 exit codes correct");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Fork with COW write
 *
 * Allocate a large static buffer, fork 8 children, each writes its PID to
 * a different region (triggers COW faults), exits with checksum of written
 * data. Parent verifies all exit codes.
 * ---------------------------------------------------------------------------
 */
static char cow_buffer[8 * 4096] __attribute__ ((aligned(4096)));

static void
test_fork_cow_write(void)
{
	/* Parent initializes the buffer with a known pattern. */
	for (int i = 0; i < (int)sizeof(cow_buffer); i++) {
		cow_buffer[i] = (char)(i & 0xff);
	}

	long pids[8];
	int ok = 1;

	for (int i = 0; i < 8; i++) {
		long pid = do_fork();
		if (pid < 0) {
			check(0, "cow_write: fork failed");
			for (int j = 0; j < i; j++) {
				int st = 0;
				do_wait(pids[j], &st);
			}
			return;
		}
		if (pid == 0) {
			/* Each child writes to its own 4096-byte region. */
			volatile char *region =
			    (volatile char *)&cow_buffer[i * 4096];
			long my_pid = do_getpid();
			unsigned char checksum = 0;
			for (int b = 0; b < 4096; b++) {
				char val = (char)(my_pid + b);
				region[b] = val;
				checksum += (unsigned char)val;
			}
			/* Verify we can read back what we wrote. */
			unsigned char verify = 0;
			for (int b = 0; b < 4096; b++) {
				verify += (unsigned char)region[b];
			}
			do_exit(checksum == verify ? 0 : 1);
		}
		pids[i] = pid;
	}

	for (int i = 0; i < 8; i++) {
		int status = 0;
		long rpid = do_wait(pids[i], &status);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		if (code != 0)
			ok = 0;
	}
	check(ok, "cow_write: all 8 children verified");

	/* Verify parent's buffer is untouched. */
	int intact = 1;
	for (int i = 0; i < (int)sizeof(cow_buffer); i++) {
		if (cow_buffer[i] != (char)(i & 0xff)) {
			intact = 0;
			break;
		}
	}
	check(intact, "cow_write: parent buffer intact");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Nested fork
 *
 * Fork a child, child forks a grandchild, grandchild exits 42, child waits
 * for grandchild and exits with grandchild's code, parent waits and verifies
 * both levels.
 * ---------------------------------------------------------------------------
 */
static void
test_nested_fork(void)
{
	long pid = do_fork();
	check(pid >= 0, "nested_fork: fork");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: fork a grandchild. */
		long gpid = do_fork();
		if (gpid < 0)
			do_exit(99);

		if (gpid == 0) {
			/* Grandchild: exit with 42. */
			do_exit(42);
		}
		/* Child: wait for grandchild. */
		int status = 0;
		long r = do_wait(gpid, &status);
		if (r < 0)
			do_exit(98);
		int code = (status >> 8) & 0xff;
		/* Exit with grandchild's code so parent can verify chain. */
		do_exit(code);
	}
	/* Parent: wait for child. */
	int status = 0;
	long rpid = do_wait(pid, &status);
	check(rpid == pid, "nested_fork: wait returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 42, "nested_fork: grandchild code propagated");
}

/*
 * ---------------------------------------------------------------------------
 * Test 4: Fork bomb with limit
 *
 * Fork children until fork fails (EAGAIN or ENOMEM). Verify the error code.
 * Then wait for all children to exit, verifying process table recovers.
 * ---------------------------------------------------------------------------
 */
static void
test_fork_bomb_limit(void)
{
	long pids[256];		/* MAX_PROCS. */
	int count = 0;

	/* Fork until failure. */
	for (int i = 0; i < 256; i++) {
		long pid = do_fork();
		if (pid < 0) {
			/* Expected: fork fails when proc table is full. */
			check(pid == -EAGAIN || pid == -ENOMEM,
			      "fork_bomb: correct error on limit");
			break;
		}
		if (pid == 0) {
			/* Child: just exit immediately. */
			do_exit(0);
		}
		pids[count++] = pid;
	}

	check(count > 0, "fork_bomb: at least one child forked");

	/* Wait for all children to drain the process table. */
	int ok = 1;
	for (int i = 0; i < count; i++) {
		int status = 0;
		long r = do_wait(-1, &status);
		if (r < 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "fork_bomb: all children reaped");

	/* Verify recovery: can fork again after reaping. */
	long pid = do_fork();
	check(pid >= 0, "fork_bomb: fork works after recovery");
	if (pid == 0) {
		do_exit(7);
	}
	if (pid > 0) {
		int status = 0;
		do_wait(pid, &status);
		int code = (status >> 8) & 0xff;
		check(code == 7, "fork_bomb: recovered child exit code");
	}
}

/*
 * ---------------------------------------------------------------------------
 * Test 5: Concurrent writers (COW isolation)
 *
 * Fork 4 children, each child writes to a shared (COW) page and reads back
 * its own copy, verifying COW isolation between processes.
 * ---------------------------------------------------------------------------
 */
static volatile int shared_page[1024] __attribute__ ((aligned(4096)));

static void
test_concurrent_writers(void)
{
	/* Parent writes a sentinel. */
	for (int i = 0; i < 1024; i++) {
		shared_page[i] = 0xDEAD;
	}

	long pids[4];
	int ok = 1;

	for (int i = 0; i < 4; i++) {
		long pid = do_fork();
		if (pid < 0) {
			check(0, "conc_writers: fork failed");
			for (int j = 0; j < i; j++) {
				int st = 0;
				do_wait(pids[j], &st);
			}
			return;
		}
		if (pid == 0) {
			/* Each child writes its own pattern (triggers COW) */
			int my_val = 0x1000 * (i + 1);
			for (int k = 0; k < 1024; k++) {
				shared_page[k] = my_val + k;
			}

			/* Read back and verify isolation -- must see only our values. */
			int child_ok = 1;
			for (int k = 0; k < 1024; k++) {
				if (shared_page[k] != my_val + k) {
					child_ok = 0;
					break;
				}
			}
			do_exit(child_ok ? 0 : 1);
		}
		pids[i] = pid;
	}

	for (int i = 0; i < 4; i++) {
		int status = 0;
		long rpid = do_wait(pids[i], &status);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		if (code != 0)
			ok = 0;
	}
	check(ok, "conc_writers: all children isolated");

	/* Parent's page must still have sentinel. */
	int intact = 1;
	for (int i = 0; i < 1024; i++) {
		if (shared_page[i] != 0xDEAD) {
			intact = 0;
			break;
		}
	}
	check(intact, "conc_writers: parent page intact");
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== fork stress tests ===\n");

	test_rapid_fork_exit();
	test_fork_cow_write();
	test_nested_fork();
	test_fork_bomb_limit();
	test_concurrent_writers();

	test_done();
	return 0;
}
