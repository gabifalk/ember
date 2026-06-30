/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * epoll stub and ioctl variant tests -- exercises epoll_create, epoll_create1,
 * epoll_ctl, epoll_wait, epoll_pwait stubs, and ioctl TIOCGWINSZ/FIONREAD.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read          0
#define __NR_write         1
#define __NR_close         3
#define __NR_ioctl         16
#define __NR_epoll_create  213
#define __NR_epoll_wait    232
#define __NR_epoll_ctl     233
#define __NR_epoll_pwait   281
#define __NR_epoll_create1 291
#define __NR_pipe2         293

/* Constants. */
#define O_CLOEXEC    02000000
#define TIOCGWINSZ   0x5413
#define FIONREAD     0x541B
#define EPOLL_CTL_ADD 1
#define EPOLLIN      0x001

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

struct epoll_event {
	unsigned int events;
	unsigned long long data;
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
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10):"rcx", "r11", "memory");
	return ret;
}

static long
sys5(long nr, long a1, long a2, long a3, long a4, long a5)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10), "r"(r8):"rcx", "r11", "memory");
	return ret;
}

/* Test 1: epoll_create returns a valid fd. */
static void
test_epoll_create(void)
{
	long fd = sys1(__NR_epoll_create, 1);
	check(fd >= 0, "epoll_create: returns fd >= 0");
	sys1(__NR_close, fd);
}

/* Test 2: epoll_create1 with flags=0 returns a valid fd. */
static void
test_epoll_create1(void)
{
	long fd = sys1(__NR_epoll_create1, 0);
	check(fd >= 0, "epoll_create1: returns fd >= 0");
	sys1(__NR_close, fd);
}

/* Test 3: epoll_create1 with O_CLOEXEC returns a valid fd. */
static void
test_epoll_create1_cloexec(void)
{
	long fd = sys1(__NR_epoll_create1, O_CLOEXEC);
	check(fd >= 0, "epoll_create1(O_CLOEXEC): returns fd >= 0");
	sys1(__NR_close, fd);
}

/* Test 4: epoll_ctl stub returns 0. */
static void
test_epoll_ctl_stub(void)
{
	long epfd = sys1(__NR_epoll_create1, 0);
	check(epfd >= 0, "epoll_ctl: create epoll fd");

	int fds[2];
	long r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "epoll_ctl: pipe2");

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data = 0;

	r = sys4(__NR_epoll_ctl, epfd, EPOLL_CTL_ADD, fds[0], (long)&event);
	check(r == 0, "epoll_ctl: ADD returns 0");

	sys1(__NR_close, fds[0]);
	sys1(__NR_close, fds[1]);
	sys1(__NR_close, epfd);
}

/* Test 5: epoll_wait stub returns 0 with timeout=0. */
static void
test_epoll_wait_stub(void)
{
	long epfd = sys1(__NR_epoll_create1, 0);
	check(epfd >= 0, "epoll_wait: create epoll fd");

	struct epoll_event events[1];
	long r = sys4(__NR_epoll_wait, epfd, (long)&events, 1, 0);
	check(r == 0, "epoll_wait: returns 0 (no events)");

	sys1(__NR_close, epfd);
}

/* Test 6: epoll_pwait stub returns 0 with timeout=0, sigmask=NULL. */
static void
test_epoll_pwait_stub(void)
{
	long epfd = sys1(__NR_epoll_create1, 0);
	check(epfd >= 0, "epoll_pwait: create epoll fd");

	struct epoll_event events[1];
	long r = sys5(__NR_epoll_pwait, epfd, (long)&events, 1, 0, 0);
	check(r == 0, "epoll_pwait: returns 0 (no events)");

	sys1(__NR_close, epfd);
}

/* Test 7: ioctl TIOCGWINSZ on stdout. */
static void
test_ioctl_tiocgwinsz(void)
{
	struct winsize ws;
	ws.ws_row = 0;
	ws.ws_col = 0;

	long r = sys3(__NR_ioctl, 1, TIOCGWINSZ, (long)&ws);
	check(r == 0, "ioctl TIOCGWINSZ: returns 0");
	check(ws.ws_row > 0, "ioctl TIOCGWINSZ: ws_row > 0");
	check(ws.ws_col > 0, "ioctl TIOCGWINSZ: ws_col > 0");
}

/* Test 8: ioctl FIONREAD on pipe with data. */
static void
test_ioctl_fionread_pipe(void)
{
	int fds[2];
	long r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "fionread: pipe2");

	r = sys3(__NR_write, fds[1], (long)"hello", 5);
	check(r == 5, "fionread: write 5 bytes");

	int count = 0;
	r = sys3(__NR_ioctl, fds[0], FIONREAD, (long)&count);
	check(r == 0, "fionread: ioctl returns 0");
	check(count == 5, "fionread: count == 5");

	sys1(__NR_close, fds[0]);
	sys1(__NR_close, fds[1]);
}

/* Test 9: ioctl FIONREAD on empty pipe. */
static void
test_ioctl_fionread_empty(void)
{
	int fds[2];
	long r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "fionread_empty: pipe2");

	int count = 0;
	r = sys3(__NR_ioctl, fds[0], FIONREAD, (long)&count);
	check(r == 0, "fionread_empty: ioctl returns 0");
	check(count == 0, "fionread_empty: count == 0");

	sys1(__NR_close, fds[0]);
	sys1(__NR_close, fds[1]);
}

/* Test 10: close epoll fd. */
static void
test_epoll_close(void)
{
	long epfd = sys1(__NR_epoll_create1, 0);
	check(epfd >= 0, "epoll_close: create epoll fd");

	long r = sys1(__NR_close, epfd);
	check(r == 0, "epoll_close: close returns 0");
}

int
main(void)
{
	msg("=== io_misc tests ===\n");

	test_epoll_create();
	test_epoll_create1();
	test_epoll_create1_cloexec();
	test_epoll_ctl_stub();
	test_epoll_wait_stub();
	test_epoll_pwait_stub();
	test_ioctl_tiocgwinsz();
	test_ioctl_fionread_pipe();
	test_ioctl_fionread_empty();
	test_epoll_close();

	test_done();
	return 0;
}
