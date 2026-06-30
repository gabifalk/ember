/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * select() syscall test -- exercises select on pipes for readfds, writefds,
 * zero-timeout (non-blocking) select, and bad fd handling.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_CLOSE   3
#define SYS_SELECT  23
#define SYS_PIPE2   293

struct timeval {
	long tv_sec;
	long tv_usec;
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

static long
sys5(long nr, long a1, long a2, long a3, long a4, long a5)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
			  :"rcx", "r11", "memory");
	return ret;
}

/*
 * Helper: call select(nfds, readfds, writefds, exceptfds, timeout)
 * fd_set pointers are uint64_t* (sufficient for fds < 64)
 */
static long
do_select(int nfds, unsigned long *rfds, unsigned long *wfds,
	  unsigned long *efds, struct timeval *tv)
{
	return sys5(SYS_SELECT, nfds, (long)rfds, (long)wfds, (long)efds,
		    (long)tv);
}

/* Test 1: select readfds set after writing to pipe. */
static void
test_select_readfds(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "readfds: pipe2");

	/* Write data to pipe. */
	r = sys3(SYS_WRITE, fds[1], (long)"hi", 2);
	check(r == 2, "readfds: write 2 bytes");

	/* Select with read fd in readfds. */
	unsigned long rfds = (1UL << fds[0]);
	struct timeval tv = {.tv_sec = 1,.tv_usec = 0 };

	r = do_select(fds[0] + 1, &rfds, 0, 0, &tv);
	check(r == 1, "readfds: select returns 1");
	check((rfds >> fds[0]) & 1, "readfds: read fd is set");

	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);
}

/* Test 2: select writefds set on empty pipe (has space) */
static void
test_select_writefds(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "writefds: pipe2");

	/* Select with write fd in writefds -- pipe is empty, should have space. */
	unsigned long wfds = (1UL << fds[1]);
	struct timeval tv = {.tv_sec = 1,.tv_usec = 0 };

	r = do_select(fds[1] + 1, 0, &wfds, 0, &tv);
	check(r == 1, "writefds: select returns 1");
	check((wfds >> fds[1]) & 1, "writefds: write fd is set");

	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);
}

/* Test 3: select timeout=0 returns immediately with 0 on empty pipe. */
static void
test_select_timeout_zero(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "timeout0: pipe2");

	/* Select read fd with zero timeout -- pipe is empty, should return 0. */
	unsigned long rfds = (1UL << fds[0]);
	struct timeval tv = {.tv_sec = 0,.tv_usec = 0 };

	r = do_select(fds[0] + 1, &rfds, 0, 0, &tv);
	check(r == 0, "timeout0: select returns 0");
	check(((rfds >> fds[0]) & 1) == 0, "timeout0: read fd not set");

	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);
}

/* Test 4: select with closed/bad fd. */
static void
test_select_bad_fd(void)
{
	int fds[2];
	long r = sys2(SYS_PIPE2, (long)fds, 0);
	check(r == 0, "badfd: pipe2");

	int closed_fd = fds[0];
	sys1(SYS_CLOSE, fds[0]);
	sys1(SYS_CLOSE, fds[1]);

	/* Put closed fd in readfds with zero timeout. */
	unsigned long rfds = (1UL << closed_fd);
	struct timeval tv = {.tv_sec = 0,.tv_usec = 0 };

	r = do_select(closed_fd + 1, &rfds, 0, 0, &tv);
	/* Linux returns -EBADF for bad fds in the set; either that or 0 is acceptable. */
	check(r == 0 || r == -9, "badfd: select handles closed fd");
}

int
main(void)
{
	msg("=== select tests ===\n");

	test_select_readfds();
	test_select_writefds();
	test_select_timeout_zero();
	test_select_bad_fd();

	test_done();
	return 0;
}
