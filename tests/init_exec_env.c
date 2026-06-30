/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Execve argv/envp tests -- exercises execve with various argv and envp
 * combinations: basic argv, non-empty envp, empty argv, ENOENT path,
 * and multiple argv entries.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_write          1
#define __NR_fork           57
#define __NR_execve         59
#define __NR_exit           60
#define __NR_wait4          61

/* Error codes. */
#define ENOENT  2

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

/* ---------- Test 1: execve passes argv correctly ---------- */

static void
test_execve_argv(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "execve argv (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(r == pid && exited && code == 0, "execve with argv");
}

/* ---------- Test 2: execve with environment ---------- */

static void
test_execve_envp(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "execve envp (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello", 0 };
		char *envp[] = { "FOO=bar", 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(r == pid && exited && code == 0, "execve with non-empty envp");
}

/* ---------- Test 3: execve with empty argv ---------- */

static void
test_execve_empty_argv(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "execve empty argv (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		/*
		 * If execve succeeded, /hello runs and exits 0.
		 * If execve failed, exit 99 to distinguish.
		 */
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	/* Accept exit 0 (execve succeeded with empty argv) -- key is no crash. */
	check(r == pid && exited && code != 99, "execve with empty argv");
}

/* ---------- Test 4: execve ENOENT ---------- */

static void
test_execve_enoent(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "execve ENOENT (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/nonexistent", 0 };
		char *envp[] = { 0 };
		long r =
		    sys3(__NR_execve, (long)"/nonexistent", (long)argv,
			 (long)envp);
		/* Execve failed, verify ENOENT then exit 0. */
		if (r == -ENOENT)
			sys1(__NR_exit, 0);
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(r == pid && exited && code == 0, "execve ENOENT on /nonexistent");
}

/* ---------- Test 5: execve with multiple argv ---------- */

static void
test_execve_multi_argv(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "execve multi argv (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello", "arg1", "arg2", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(r == pid && exited && code == 0, "execve with multiple argv");
}

int
main(void)
{
	msg("=== exec-env tests ===\n");

	test_execve_argv();
	test_execve_envp();
	test_execve_empty_argv();
	test_execve_enoent();
	test_execve_multi_argv();

	test_done();
	return 0;
}
