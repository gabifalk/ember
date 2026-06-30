/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Resource limit tests -- fd exhaustion (EMFILE), fork exhaustion (EAGAIN),
 * and dup beyond fd table limit.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_open      2
#define __NR_close     3
#define __NR_dup      32
#define __NR_nanosleep 35
#define __NR_fork     57
#define __NR_exit     60
#define __NR_wait4    61

/* Flags and error codes. */
#define O_RDONLY  0
#define EAGAIN   11
#define ENOMEM   12
#define EMFILE   24

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

static long
do_open(const char *path, long flags)
{
	return sys2(__NR_open, (long)path, flags);
}

static long
do_close(long fd)
{
	return sys1(__NR_close, fd);
}

static long
do_dup(long fd)
{
	return sys1(__NR_dup, fd);
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

struct timespec {
	long tv_sec;
	long tv_nsec;
};

static long
do_nanosleep(struct timespec *req, struct timespec *rem)
{
	return sys2(__NR_nanosleep, (long)req, (long)rem);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: FD exhaustion (EMFILE)
 *
 * Open /dev/null in a loop until open() returns -EMFILE. Verify we opened
 * a reasonable number (>= 3, since stdin/stdout/stderr are pre-opened).
 * Then close all opened fds and verify we can open again.
 * ---------------------------------------------------------------------------
 */
static void
test_fd_exhaustion(void)
{
	int fds[256];
	int count = 0;

	/* Open /dev/null until we hit the limit. */
	for (int i = 0; i < 256; i++) {
		long fd = do_open("/dev/null", O_RDONLY);
		if (fd < 0) {
			check(fd == -EMFILE, "fd_exhaust: error is EMFILE");
			break;
		}
		fds[count++] = (int)fd;
	}

	check(count >= 3, "fd_exhaust: opened >= 3 fds");
	msg("fd_exhaust: opened ");
	print_int(count);
	msg(" fds before EMFILE\n");

	/* Close all opened fds (skip 0, 1, 2) */
	for (int i = 0; i < count; i++) {
		if (fds[i] > 2)
			do_close(fds[i]);
	}

	/* Verify we can open again after closing. */
	long fd = do_open("/dev/null", O_RDONLY);
	check(fd >= 0, "fd_exhaust: can open after close");
	if (fd >= 0)
		do_close(fd);
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Fork exhaustion (EAGAIN)
 *
 * Fork in a loop, children sleep for 2 seconds then exit. When fork returns
 * -EAGAIN (or -ENOMEM), verify we created a reasonable number of children
 * (>= 5). Then wait for all children.
 * ---------------------------------------------------------------------------
 */
static void
test_fork_exhaustion(void)
{
	long pids[256];
	int count = 0;

	for (int i = 0; i < 256; i++) {
		long pid = do_fork();
		if (pid < 0) {
			check(pid == -EAGAIN || pid == -ENOMEM,
			      "fork_exhaust: correct error code");
			break;
		}
		if (pid == 0) {
			/* Child: sleep for 2 seconds, then exit. */
			struct timespec ts = {.tv_sec = 2,.tv_nsec = 0 };
			do_nanosleep(&ts, (struct timespec *)0);
			do_exit(0);
		}
		pids[count++] = pid;
	}

	check(count >= 5, "fork_exhaust: created >= 5 children");
	msg("fork_exhaust: forked ");
	print_int(count);
	msg(" children before limit\n");

	/* Wait for all children. */
	int ok = 1;
	for (int i = 0; i < count; i++) {
		int status = 0;
		long r = do_wait(-1, &status);
		if (r < 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "fork_exhaust: all children reaped");

	/* Verify recovery: can fork again. */
	long pid = do_fork();
	check(pid >= 0, "fork_exhaust: fork works after recovery");
	if (pid == 0) {
		do_exit(0);
	}
	if (pid > 0) {
		int status = 0;
		do_wait(pid, &status);
	}
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Dup beyond fd table limit
 *
 * Fill the fd table by opening /dev/null, then verify dup() returns -EMFILE.
 * ---------------------------------------------------------------------------
 */
static void
test_dup_beyond_limit(void)
{
	int fds[256];
	int count = 0;

	/* Fill the fd table. */
	for (int i = 0; i < 256; i++) {
		long fd = do_open("/dev/null", O_RDONLY);
		if (fd < 0)
			break;
		fds[count++] = (int)fd;
	}

	check(count > 0, "dup_limit: opened fds to fill table");

	/* Now dup should fail with EMFILE. */
	long ret = do_dup(0);	/* Try to dup stdin. */
	check(ret == -EMFILE, "dup_limit: dup returns EMFILE");

	/* Clean up: close all opened fds (skip 0, 1, 2) */
	for (int i = 0; i < count; i++) {
		if (fds[i] > 2)
			do_close(fds[i]);
	}

	/* Verify dup works again after cleanup. */
	long fd = do_dup(0);
	check(fd >= 0, "dup_limit: dup works after cleanup");
	if (fd >= 0)
		do_close(fd);
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== resource limit tests ===\n");

	test_fd_exhaustion();
	test_fork_exhaustion();
	test_dup_beyond_limit();

	test_done();
	return 0;
}
