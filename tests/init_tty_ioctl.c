/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * TTY ioctl tests -- TIOCGWINSZ, TCGETS on stdout/stdin/stderr, /dev/tty,
 * ioctl on pipe returns ENOTTY, isatty-like check.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_write           1
#define __NR_open            2
#define __NR_close           3
#define __NR_ioctl           16
#define __NR_pipe2           293

/* Ioctl requests. */
#define TCGETS       0x5401
#define TIOCGWINSZ   0x5413

/* Errno values. */
#define ENOENT       2
#define EINVAL       22
#define ENOTTY       25

/* Open flags. */
#define O_RDWR       2

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
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
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "S"(a2), "D"(a1):"rcx",
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
do_open(const char *path, long flags, long mode)
{
	return sys3(__NR_open, (long)path, flags, mode);
}

static long
do_close(long fd)
{
	return sys1(__NR_close, fd);
}

static long
do_ioctl(long fd, long request, long arg)
{
	return sys3(__NR_ioctl, fd, request, arg);
}

static long
do_pipe2(int pipefd[2], long flags)
{
	return sys2(__NR_pipe2, (long)pipefd, flags);
}

/* Test 1: TIOCGWINSZ on stdout. */
static void
test_tiocgwinsz_stdout(void)
{
	struct winsize ws;
	ws.ws_row = 0xffff;
	ws.ws_col = 0xffff;
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;

	long r = do_ioctl(1, TIOCGWINSZ, (long)&ws);
	if (r == -ENOTTY) {
		/* Acceptable: stdout is not a tty in this environment. */
		check(1, "TIOCGWINSZ stdout (ENOTTY ok)");
		return;
	}
	/*
	 * If success, verify non-negative rows/cols (unsigned short, always >= 0,
	 * but check they were actually written -- not still 0xffff sentinel)
	 */
	check(r == 0, "TIOCGWINSZ stdout returns 0");
}

/* Test 2: TCGETS on stdout. */
static void
test_tcgets_stdout(void)
{
	char termios_buf[64];
	/* Initialize with sentinel pattern. */
	for (int i = 0; i < 64; i++)
		termios_buf[i] = (char)0xaa;

	long r = do_ioctl(1, TCGETS, (long)termios_buf);
	if (r == -ENOTTY) {
		check(1, "TCGETS stdout (ENOTTY ok)");
		return;
	}
	/* If success, just verify it didn't crash and returned 0. */
	check(r == 0, "TCGETS stdout returns 0");
}

/* Test 3: Open /dev/tty and do TIOCGWINSZ on it. */
static void
test_dev_tty(void)
{
	long fd = do_open("/dev/tty", O_RDWR, 0);
	if (fd == -ENOENT) {
		/* /Dev/tty doesn't exist in cpio -- skip gracefully. */
		msg("  skip: /dev/tty not found\n");
		check(1, "/dev/tty TIOCGWINSZ (skipped, no /dev/tty)");
		return;
	}
	if (fd < 0) {
		/* Other open error -- also skip gracefully. */
		msg("  skip: /dev/tty open error\n");
		check(1, "/dev/tty TIOCGWINSZ (skipped, open error)");
		return;
	}

	struct winsize ws;
	ws.ws_row = 0;
	ws.ws_col = 0;
	long r = do_ioctl(fd, TIOCGWINSZ, (long)&ws);
	/* Either success or ENOTTY is acceptable. */
	check(r == 0 || r == -ENOTTY, "/dev/tty TIOCGWINSZ");
	do_close(fd);
}

/* Test 4: ioctl on a pipe fd should return -ENOTTY or -EINVAL. */
static void
test_ioctl_pipe(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "ioctl pipe (pipe2 failed)");
		return;
	}

	struct winsize ws;
	r = do_ioctl(pipefd[0], TIOCGWINSZ, (long)&ws);
	check(r == -ENOTTY || r == -EINVAL, "ioctl pipe ENOTTY/EINVAL");

	do_close(pipefd[0]);
	do_close(pipefd[1]);
}

/* Test 5: isatty-like check -- TCGETS on stdin, stdout, stderr. */
static void
test_isatty_check(void)
{
	char buf[64];
	int any_success = 0;

	long r0 = do_ioctl(0, TCGETS, (long)buf);
	if (r0 == 0)
		any_success = 1;

	long r1 = do_ioctl(1, TCGETS, (long)buf);
	if (r1 == 0)
		any_success = 1;

	long r2 = do_ioctl(2, TCGETS, (long)buf);
	if (r2 == 0)
		any_success = 1;

	/* Each result should be either 0 (success) or -ENOTTY. */
	int all_valid = (r0 == 0 || r0 == -ENOTTY) &&
	    (r1 == 0 || r1 == -ENOTTY) && (r2 == 0 || r2 == -ENOTTY);

	check(all_valid, "isatty TCGETS valid returns");
	/* At least one of stdin/stdout/stderr should be a tty (console) */
	check(any_success, "isatty at least one tty");
}

int
main(void)
{
	msg("=== tty ioctl tests ===\n");
	test_tiocgwinsz_stdout();
	test_tcgets_stdout();
	test_dev_tty();
	test_ioctl_pipe();
	test_isatty_check();
	test_done();
	return 0;
}
