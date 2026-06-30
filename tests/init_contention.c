/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Multi-process contention test -- stresses spinlock correctness under
 * concurrent access to pipes, proc table, PMM/paging, VFS, and fd table.
 * Runs on cpio initrd.
 */

#include <sys/stat.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read    0
#define __NR_write   1
#define __NR_open    2
#define __NR_close   3
#define __NR_stat    4
#define __NR_brk    12
#define __NR_pipe   22
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

/*
 * ---------------------------------------------------------------------------
 * Test 1: Pipe contention
 *
 * 4 children each write 100 messages to a shared pipe. Parent reads all
 * messages and verifies the total count is 400.
 * ---------------------------------------------------------------------------
 */
static void
test_pipe_contention(void)
{
	int pipefd[2];
	long r = sys1(__NR_pipe, (long)pipefd);
	if (r < 0) {
		check(0, "pipe_contention: pipe");
		return;
	}
	/* Each child writes "XX\n" (3 bytes) 100 times. */
	for (int c = 0; c < 4; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "pipe_contention: fork");
			return;
		}
		if (pid == 0) {
			sys1(__NR_close, pipefd[0]);	/* Close read end. */
			char buf[4];
			buf[0] = '0' + (char)c;
			buf[1] = '\n';
			for (int i = 0; i < 100; i++) {
				sys3(__NR_write, pipefd[1], (long)buf, 2);
			}
			sys1(__NR_close, pipefd[1]);
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	/* Parent: close write end so reads will EOF after children finish. */
	sys1(__NR_close, pipefd[1]);

	/* Read all data from the pipe. */
	int total_bytes = 0;
	char rbuf[256];
	for (;;) {
		long n = sys3(__NR_read, pipefd[0], (long)rbuf, sizeof(rbuf));
		if (n <= 0)
			break;
		total_bytes += (int)n;
	}
	sys1(__NR_close, pipefd[0]);

	/* Wait for all 4 children. */
	for (int c = 0; c < 4; c++) {
		int status = 0;
		sys4(__NR_wait4, -1, (long)&status, 0, 0);
	}

	/* 4 Children * 100 messages * 2 bytes each = 800 bytes. */
	check(total_bytes == 800, "pipe_contention: total bytes");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Fork storm
 *
 * Fork 8 children in a loop. Each exits with its index. Parent waits for
 * all and verifies each returned the correct exit code.
 * ---------------------------------------------------------------------------
 */
static void
test_fork_storm(void)
{
	long pids[8];
	int ok = 1;

	for (int i = 0; i < 8; i++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "fork_storm: fork");
			return;
		}
		if (pid == 0) {
			sys1(__NR_exit, i);
			__builtin_unreachable();
		}
		pids[i] = pid;
	}

	/* Wait for all children and verify exit codes. */
	int seen[8] = { 0 };
	for (int i = 0; i < 8; i++) {
		int status = 0;
		long rpid = sys4(__NR_wait4, -1, (long)&status, 0, 0);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		/* Find which child this was. */
		for (int j = 0; j < 8; j++) {
			if (pids[j] == rpid) {
				if (code != j)
					ok = 0;
				seen[j] = 1;
				break;
			}
		}
	}

	/* Verify we saw all children. */
	for (int i = 0; i < 8; i++) {
		if (!seen[i])
			ok = 0;
	}

	check(ok, "fork_storm: all exit codes correct");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Concurrent brk
 *
 * Fork 4 children. Each extends brk by 4 pages (16 KiB), writes to all
 * pages, then exits 0. Parent verifies all succeeded.
 * ---------------------------------------------------------------------------
 */
static void
test_concurrent_brk(void)
{
	for (int c = 0; c < 4; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "concurrent_brk: fork");
			return;
		}
		if (pid == 0) {
			long cur = sys1(__NR_brk, 0);
			long new_brk = sys1(__NR_brk, cur + 4096 * 4);
			if (new_brk < cur + 4096 * 4) {
				sys1(__NR_exit, 1);
				__builtin_unreachable();
			}
			/* Write to each page. */
			for (int p = 0; p < 4; p++) {
				volatile char *ptr =
				    (volatile char *)(cur + 4096 * p);
				ptr[0] = 0x42;
				ptr[4095] = 0x43;
			}
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = 1;
	for (int c = 0; c < 4; c++) {
		int status = 0;
		long rpid = sys4(__NR_wait4, -1, (long)&status, 0, 0);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		if (code != 0)
			ok = 0;
	}

	check(ok, "concurrent_brk: all children succeeded");
}

/*
 * ---------------------------------------------------------------------------
 * Test 4: Concurrent file ops
 *
 * Fork 4 children. Each opens /init, reads 64 bytes, stats it, closes it,
 * then exits 0. Parent verifies all succeeded.
 * ---------------------------------------------------------------------------
 */
static void
test_concurrent_file_ops(void)
{
	for (int c = 0; c < 4; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "concurrent_file_ops: fork");
			return;
		}
		if (pid == 0) {
			long fd =
			    sys2(__NR_open, (long)"/init", 0 /* O_RDONLY. */ );
			if (fd < 0) {
				sys1(__NR_exit, 1);
				__builtin_unreachable();
			}
			char buf[64];
			long n = sys3(__NR_read, fd, (long)buf, 64);
			if (n != 64) {
				sys1(__NR_close, fd);
				sys1(__NR_exit, 2);
				__builtin_unreachable();
			}
			struct stat st;
			long r = sys2(__NR_stat, (long)"/init", (long)&st);
			if (r < 0) {
				sys1(__NR_close, fd);
				sys1(__NR_exit, 3);
				__builtin_unreachable();
			}
			sys1(__NR_close, fd);
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = 1;
	for (int c = 0; c < 4; c++) {
		int status = 0;
		long rpid = sys4(__NR_wait4, -1, (long)&status, 0, 0);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		if (code != 0)
			ok = 0;
	}

	check(ok, "concurrent_file_ops: all children succeeded");
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== contention tests ===\n");

	test_pipe_contention();
	test_fork_storm();
	test_concurrent_brk();
	test_concurrent_file_ops();

	test_done();
	return 0;
}
