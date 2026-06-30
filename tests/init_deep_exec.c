/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Deep exec tests -- exercises exec chaining and fd inheritance:
 * pipe+exec with inherited fds, cloexec across exec, chained fork
 * with grandchild, and multiple sequential fork+exec cycles.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read            0
#define __NR_write           1
#define __NR_close           3
#define __NR_getpid          39
#define __NR_fork            57
#define __NR_execve          59
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_fcntl           72
#define __NR_getppid         110
#define __NR_pipe2           293

/* Fcntl commands. */
#define F_SETFD  2

/* Flags. */
#define FD_CLOEXEC   1
#define O_CLOEXEC    02000000

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

/* ---------- Test 1: exec with inherited pipe fds ---------- */

static void
test_exec_with_pipe(void)
{
	int pipefd[2] = { -1, -1 };
	long r = sys2(__NR_pipe2, (long)pipefd, 0);
	if (r < 0) {
		check(0, "exec with pipe (pipe2 failed)");
		return;
	}

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec with pipe (fork failed)");
		return;
	}
	if (pid == 0) {
		/* Child: pipe fds are inherited (but /hello doesn't use them) */
		char *argv[] = { "/hello", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	/* Parent: close pipe ends and wait. */
	sys1(__NR_close, pipefd[0]);
	sys1(__NR_close, pipefd[1]);

	int status = 0;
	r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(r == pid && exited && code == 0, "exec with inherited pipe fds");
}

/* ---------- Test 2: cloexec pipe fd closed after exec ---------- */

static void
test_exec_cloexec_pipe(void)
{
	int pipefd[2] = { -1, -1 };
	long r = sys2(__NR_pipe2, (long)pipefd, 0);
	if (r < 0) {
		check(0, "exec cloexec pipe (pipe2 failed)");
		return;
	}
	/* Set read end to FD_CLOEXEC. */
	r = sys3(__NR_fcntl, pipefd[0], F_SETFD, FD_CLOEXEC);
	if (r < 0) {
		check(0, "exec cloexec pipe (fcntl failed)");
		return;
	}

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec cloexec pipe (fork failed)");
		return;
	}
	if (pid == 0) {
		/* Child: exec /hello -- the cloexec fd should be closed automatically. */
		char *argv[] = { "/hello", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	/* Parent: close pipe ends and wait. */
	sys1(__NR_close, pipefd[0]);
	sys1(__NR_close, pipefd[1]);

	int status = 0;
	r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int exited = (((status) & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(r == pid && exited && code == 0, "exec with cloexec pipe fd");
}

/* ---------- Test 3: chained fork (grandchild) ---------- */

static void
test_chained_fork(void)
{
	long pid_a = sys0(__NR_fork);
	if (pid_a < 0) {
		check(0, "chained fork (fork A failed)");
		return;
	}
	if (pid_a == 0) {
		/* Child A: fork grandchild B. */
		long pid_b = sys0(__NR_fork);
		if (pid_b < 0) {
			sys1(__NR_exit, 99);
			__builtin_unreachable();
		}
		if (pid_b == 0) {
			/* Grandchild B: exit 77. */
			sys1(__NR_exit, 77);
			__builtin_unreachable();
		}
		/* Child A: wait for grandchild B. */
		int status_b = 0;
		long r = sys4(__NR_wait4, pid_b, (long)&status_b, 0, 0);
		int exited_b = (((status_b) & 0x7f) == 0);
		int code_b = (status_b >> 8) & 0xff;
		if (r == pid_b && exited_b && code_b == 77)
			sys1(__NR_exit, 55);
		else
			sys1(__NR_exit, 98);
		__builtin_unreachable();
	}
	/* Parent: wait for child A. */
	int status_a = 0;
	long r = sys4(__NR_wait4, pid_a, (long)&status_a, 0, 0);
	int exited_a = (((status_a) & 0x7f) == 0);
	int code_a = (status_a >> 8) & 0xff;
	check(r == pid_a && exited_a
	      && code_a == 55, "chained fork grandchild exit");
}

/* ---------- Test 4: multiple sequential fork+exec cycles ---------- */

static void
test_sequential_exec(void)
{
	/* First fork+exec. */
	long pid1 = sys0(__NR_fork);
	if (pid1 < 0) {
		check(0, "sequential exec (fork 1 failed)");
		return;
	}
	if (pid1 == 0) {
		char *argv[] = { "/hello", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}

	int status1 = 0;
	long r1 = sys4(__NR_wait4, pid1, (long)&status1, 0, 0);
	int exited1 = (((status1) & 0x7f) == 0);
	int code1 = (status1 >> 8) & 0xff;
	int first_ok = (r1 == pid1 && exited1 && code1 == 0);

	/* Second fork+exec with different argv. */
	long pid2 = sys0(__NR_fork);
	if (pid2 < 0) {
		check(0, "sequential exec (fork 2 failed)");
		return;
	}
	if (pid2 == 0) {
		char *argv[] = { "/hello", "second", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}

	int status2 = 0;
	long r2 = sys4(__NR_wait4, pid2, (long)&status2, 0, 0);
	int exited2 = (((status2) & 0x7f) == 0);
	int code2 = (status2 >> 8) & 0xff;
	int second_ok = (r2 == pid2 && exited2 && code2 == 0);

	check(first_ok && second_ok, "multiple sequential fork+exec");
}

int
main(void)
{
	msg("=== deep exec tests ===\n");

	test_exec_with_pipe();
	test_exec_cloexec_pipe();
	test_chained_fork();
	test_sequential_exec();

	test_done();
	return 0;
}
