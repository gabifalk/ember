/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * poll() syscall test -- exercises poll on pipes for POLLIN, POLLOUT, POLLHUP,
 * and zero-timeout (non-blocking) poll.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_CLOSE  3
#define SYS_POLL   7
#define SYS_PIPE2  293

/* Poll event flags. */
#define POLLIN   1
#define POLLOUT  4
#define POLLHUP  16

struct pollfd {
	int fd;
	short events;
	short revents;
};

/* Raw syscall wrappers. */
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

/* Test 1: poll POLLIN on pipe read end after write. */
static void
test_poll_pollin(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "pollin: pipe2");

	/* Write data to pipe. */
	r = sys3(SYS_WRITE, fds[1], (long)"hi", 2);
	check(r == 2, "pollin: write 2 bytes");

	/* Poll read end for POLLIN. */
	struct pollfd pfd;
	pfd.fd = fds[0];
	pfd.events = POLLIN;
	pfd.revents = 0;

	r = sys3(SYS_POLL, (long)&pfd, 1, 1000);
	check(r == 1, "pollin: poll returns 1");
	check((pfd.revents & POLLIN) != 0, "pollin: revents has POLLIN");

	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);
}

/* Test 2: poll with timeout=0 on empty pipe (non-blocking) */
static void
test_poll_timeout_zero(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "timeout0: pipe2");

	/* Poll read end with timeout=0 -- pipe is empty, should return 0. */
	struct pollfd pfd;
	pfd.fd = fds[0];
	pfd.events = POLLIN;
	pfd.revents = 0;

	r = sys3(SYS_POLL, (long)&pfd, 1, 0);
	check(r == 0, "timeout0: poll returns 0 (no events)");
	check(pfd.revents == 0, "timeout0: revents is 0");

	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);
}

/* Test 3: poll POLLHUP on pipe read end after write end closed. */
static void
test_poll_pollhup(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "pollhup: pipe2");

	/* Close write end. */
	sys1(SYS_CLOSE, fds[1]);

	/* Poll read end -- should get POLLHUP. */
	struct pollfd pfd;
	pfd.fd = fds[0];
	pfd.events = POLLIN;
	pfd.revents = 0;

	r = sys3(SYS_POLL, (long)&pfd, 1, 1000);
	check(r == 1, "pollhup: poll returns 1");
	check((pfd.revents & POLLHUP) != 0, "pollhup: revents has POLLHUP");

	sys1(SYS_CLOSE, fds[0]);
}

/* Test 4: poll POLLOUT on pipe write end (pipe has space) */
static void
test_poll_pollout(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "pollout: pipe2");

	/* Poll write end for POLLOUT -- pipe is empty, should have space. */
	struct pollfd pfd;
	pfd.fd = fds[1];
	pfd.events = POLLOUT;
	pfd.revents = 0;

	r = sys3(SYS_POLL, (long)&pfd, 1, 1000);
	check(r == 1, "pollout: poll returns 1");
	check((pfd.revents & POLLOUT) != 0, "pollout: revents has POLLOUT");

	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);
}

int
main(void)
{
	msg("=== poll tests ===\n");

	test_poll_pollin();
	test_poll_timeout_zero();
	test_poll_pollhup();
	test_poll_pollout();

	test_done();
	return 0;
}
