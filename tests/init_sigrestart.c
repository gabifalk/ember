/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Signal restart test -- exercises EINTR from nanosleep and pipe read,
 * and SA_RESTART automatic restart of pipe read after signal delivery.
 */

#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read            0
#define __NR_write           1
#define __NR_close           3
#define __NR_rt_sigaction   13
#define __NR_rt_sigreturn   15
#define __NR_nanosleep      35
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_pipe2         293

/* Signal numbers. */
#define SIGUSR1 10

/* Flags. */
#define SA_RESTORER  0x04000000
#define SA_RESTART   0x10000000

/* Error codes. */
#define EINTR 4

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

/* ---- Test state ---- */
static volatile int usr1_count;

static void
handler_usr1(int sig)
{
	(void)sig;
	usr1_count++;
}

/* Small delay for synchronization. */
static void
msleep(int ms)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, ms * 1000000L};
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* Install SIGUSR1 handler with given flags (ORed with SA_RESTORER) */
static long
install_sigusr1(unsigned long extra_flags)
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler_usr1;
	sa.flags = SA_RESTORER | extra_flags;
	sa.restorer = my_restorer;
	return sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
}

/* ---- Test 1: nanosleep EINTR ---- */
static void
test_nanosleep_eintr(void)
{
	long r = install_sigusr1(0);	/* No SA_RESTART. */
	check(r == 0, "nanosleep: install handler");

	usr1_count = 0;
	long ppid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "nanosleep: fork");
		return;
	}
	if (pid == 0) {
		/* Child: sleep 50ms, then send SIGUSR1 to parent. */
		msleep(50);
		sys2(__NR_kill, ppid, SIGUSR1);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: nanosleep for 2 seconds (should be interrupted) */
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	2, 0};
	r = sys2(__NR_nanosleep, (long)&ts, 0);
	check(r == -EINTR, "nanosleep: returns -EINTR");
	check(usr1_count == 1, "nanosleep: handler ran");

	/* Reap child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
}

/* ---- Test 2: pipe read EINTR ---- */
static void
test_pipe_read_eintr(void)
{
	int fds[2];
	long r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "pipe-eintr: pipe2");

	r = install_sigusr1(0);	/* No SA_RESTART. */
	check(r == 0, "pipe-eintr: install handler");

	usr1_count = 0;
	long ppid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "pipe-eintr: fork");
		return;
	}
	if (pid == 0) {
		/*
		 * Child: close read end, sleep 50ms, signal parent,
		 * then sleep again to avoid closing write end before parent sees EINTR.
		 */
		sys1(__NR_close, fds[0]);
		msleep(50);
		sys2(__NR_kill, ppid, SIGUSR1);
		msleep(200);
		sys1(__NR_close, fds[1]);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: close write end, block on read. */
	sys1(__NR_close, fds[1]);
	char buf[16];
	r = sys3(__NR_read, fds[0], (long)buf, sizeof(buf));
	check(r == -EINTR, "pipe-eintr: read returns -EINTR");
	check(usr1_count == 1, "pipe-eintr: handler ran");

	/* Clean up. */
	sys1(__NR_close, fds[0]);
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
}

/* ---- Test 3: SA_RESTART pipe read ---- */
static void
test_pipe_read_restart(void)
{
	int fds[2];
	long r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "restart: pipe2");

	r = install_sigusr1(SA_RESTART);
	check(r == 0, "restart: install handler with SA_RESTART");

	usr1_count = 0;
	long ppid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "restart: fork");
		return;
	}
	if (pid == 0) {
		/*
		 * Child: close read end, sleep 50ms, signal parent,
		 * then sleep 50ms more and write "ok" to pipe.
		 */
		sys1(__NR_close, fds[0]);
		msleep(50);
		sys2(__NR_kill, ppid, SIGUSR1);
		msleep(50);
		sys3(__NR_write, fds[1], (long)"ok", 2);
		sys1(__NR_close, fds[1]);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/*
	 * Parent: close write end, block on read
	 * With SA_RESTART, read should restart after signal and return data.
	 */
	sys1(__NR_close, fds[1]);
	char buf[16];
	memset(buf, 0, sizeof(buf));
	r = sys3(__NR_read, fds[0], (long)buf, sizeof(buf));
	check(r == 2, "restart: read returns 2 bytes");
	check(buf[0] == 'o' && buf[1] == 'k', "restart: read got 'ok'");
	check(usr1_count == 1, "restart: handler ran");

	/* Clean up. */
	sys1(__NR_close, fds[0]);
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
}

int
main(void)
{
	msg("=== signal restart tests ===\n");

	test_nanosleep_eintr();
	test_pipe_read_eintr();
	test_pipe_read_restart();

	test_done();
	return 0;
}
