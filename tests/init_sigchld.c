/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * SIGCHLD delivery test -- exercises SIGCHLD on child exit, exit code
 * propagation, signal coalescing with multiple children, SIG_IGN auto-reap,
 * and wait4 inside a signal handler.
 */

#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_rt_sigaction   13
#define __NR_rt_sigreturn   15
#define __NR_nanosleep      35
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61

/* Signal numbers. */
#define SIGCHLD 17

/* Flags. */
#define SA_RESTORER 0x04000000
#define WNOHANG     1

/* Error codes. */
#define ECHILD 10

/* SIG_IGN / SIG_DFL. */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

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

/* Small delay for synchronization. */
static void
tiny_sleep(void)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, 10000000};		/* 10Ms. */
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* Install a sigaction for SIGCHLD. */
static long
install_sigchld(void (*handler) (int))
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;
	return sys4(__NR_rt_sigaction, SIGCHLD, (long)&sa, 0, 8);
}

/* ---- Test state ---- */
static volatile int sigchld_count;
static volatile int handler_wait_status;
static volatile int handler_wait_ret;

/* ---- Handlers ---- */
static void
sigchld_handler(int sig)
{
	(void)sig;
	sigchld_count++;
}

static void
sigchld_wait_handler(int sig)
{
	(void)sig;
	sigchld_count++;
	/* Reap child inside handler. */
	int status = 0;
	long ret = sys4(__NR_wait4, -1, (long)&status, WNOHANG, 0);
	handler_wait_ret = (int)ret;
	handler_wait_status = status;
}

/* ---- Test 1: Basic SIGCHLD delivery ---- */
static void
test_basic_sigchld(void)
{
	long r = install_sigchld(sigchld_handler);
	check(r == 0, "basic: install handler");

	sigchld_count = 0;

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "basic: fork");
		return;
	}
	if (pid == 0) {
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Wait for child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(sigchld_count >= 1, "basic: SIGCHLD delivered");
}

/* ---- Test 2: SIGCHLD with exit code ---- */
static void
test_sigchld_exit_code(void)
{
	long r = install_sigchld(sigchld_handler);
	check(r == 0, "exitcode: install handler");

	sigchld_count = 0;

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exitcode: fork");
		return;
	}
	if (pid == 0) {
		sys1(__NR_exit, 42);
		__builtin_unreachable();
	}
	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	check(wpid == pid, "exitcode: wait4 returned correct pid");
	check(code == 42, "exitcode: correct exit code");
	check(sigchld_count >= 1, "exitcode: SIGCHLD delivered");
}

/* ---- Test 3: Multiple children (signal coalescing) ---- */
static void
test_multiple_children(void)
{
	long r = install_sigchld(sigchld_handler);
	check(r == 0, "multi: install handler");

	sigchld_count = 0;

	long pids[3];
	for (int i = 0; i < 3; i++) {
		pids[i] = sys0(__NR_fork);
		if (pids[i] < 0) {
			check(0, "multi: fork");
			return;
		}
		if (pids[i] == 0) {
			/* Children exit at slightly different times. */
			if (i > 0)
				tiny_sleep();
			sys1(__NR_exit, i + 1);
			__builtin_unreachable();
		}
	}

	/* Wait for all children. */
	for (int i = 0; i < 3; i++) {
		int status = 0;
		sys4(__NR_wait4, pids[i], (long)&status, 0, 0);
	}

	/* Signals may coalesce, but at least one must have been delivered. */
	check(sigchld_count >= 1, "multi: SIGCHLD count >= 1");
}

/* ---- Test 4: SIG_IGN SIGCHLD (auto-reap) ---- */
static void
test_sigign_sigchld(void)
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = SIG_IGN;
	sa.flags = 0;

	long r = sys4(__NR_rt_sigaction, SIGCHLD, (long)&sa, 0, 8);
	check(r == 0, "sigign: install SIG_IGN");

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "sigign: fork");
		return;
	}
	if (pid == 0) {
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Give child time to exit and be auto-reaped. */
	tiny_sleep();
	tiny_sleep();

	/* Wait4 should return -ECHILD since child was auto-reaped. */
	int status = 0;
	long ret = sys4(__NR_wait4, pid, (long)&status, WNOHANG, 0);
	check(ret == -ECHILD, "sigign: wait4 returns -ECHILD (auto-reaped)");

	/* Restore default disposition. */
	memset(&sa, 0, sizeof(sa));
	sa.handler = SIG_DFL;
	sa.flags = 0;
	sys4(__NR_rt_sigaction, SIGCHLD, (long)&sa, 0, 8);
}

/* ---- Test 5: SIGCHLD handler calls wait4 ---- */
static void
test_sigchld_handler_wait(void)
{
	long r = install_sigchld(sigchld_wait_handler);
	check(r == 0, "hwait: install handler");

	sigchld_count = 0;
	handler_wait_ret = 0;
	handler_wait_status = 0;

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "hwait: fork");
		return;
	}
	if (pid == 0) {
		sys1(__NR_exit, 7);
		__builtin_unreachable();
	}
	/* Give child time to exit and signal to be delivered. */
	tiny_sleep();
	tiny_sleep();

	check(sigchld_count >= 1, "hwait: handler ran");
	check(handler_wait_ret > 0, "hwait: handler reaped child");
	int code = (handler_wait_status >> 8) & 0xff;
	check(code == 7, "hwait: handler got correct exit code");

	/* Parent wait should now get -ECHILD (already reaped by handler) */
	int status = 0;
	long ret = sys4(__NR_wait4, pid, (long)&status, WNOHANG, 0);
	check(ret == -ECHILD, "hwait: parent wait4 returns -ECHILD");
}

int
main(void)
{
	msg("=== sigchld tests ===\n");

	test_basic_sigchld();
	test_sigchld_exit_code();
	test_multiple_children();
	test_sigign_sigchld();
	test_sigchld_handler_wait();

	test_done();
	return 0;
}
