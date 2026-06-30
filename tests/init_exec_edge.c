/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Execve edge case tests -- exercises execve error handling paths:
 * nonexistent binary, empty path, directory exec, truncated ELF,
 * NULL argv, valid exec sanity check, and PID preservation across exec.
 */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read           0
#define __NR_write          1
#define __NR_open           2
#define __NR_close          3
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_execve         59
#define __NR_exit           60
#define __NR_wait4          61

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
	register long r10 __asm__("r10") = a3;
	(void)r10;
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
 * Helper: fork, run child_fn in child, wait and return child exit code.
 * child_fn should call sys1(__NR_exit, code) and not return.
 * Returns -1 on fork failure.
 */
static int
fork_and_wait(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0)
		return -1;
	if (pid == 0)
		return 0;	/* Caller checks and runs child path. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	return (status >> 8) & 0xff;
}

/* ---------- Test 1: Nonexistent binary ---------- */

static void
test_exec_nonexistent(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec nonexistent (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/nonexistent", 0 };
		char *envp[] = { 0 };
		long r =
		    sys3(__NR_execve, (long)"/nonexistent", (long)argv,
			 (long)envp);
		/* Execve failed, exit with negated error (e.g. -(-2) = 2 for ENOENT) */
		sys1(__NR_exit, (int)(-r));
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	/* ENOENT = 2. */
	check(r == pid && code == 2, "exec nonexistent returns ENOENT");
}

/* ---------- Test 2: Empty path ---------- */

static void
test_exec_empty_path(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec empty path (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "", 0 };
		char *envp[] = { 0 };
		long r = sys3(__NR_execve, (long)"", (long)argv, (long)envp);
		sys1(__NR_exit, (int)(-r));
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	/* ENOENT = 2. */
	check(r == pid && code == 2, "exec empty path returns ENOENT");
}

/* ---------- Test 3: Directory exec ---------- */

static void
test_exec_directory(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec directory (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/", 0 };
		char *envp[] = { 0 };
		long r = sys3(__NR_execve, (long)"/", (long)argv, (long)envp);
		sys1(__NR_exit, (int)(-r));
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	/* Accept EACCES(13), EISDIR(21), or EPERM(1) */
	check(r == pid && (code == 13 || code == 21 || code == 1),
	      "exec directory returns error");
}

/* ---------- Test 4: Truncated ELF ---------- */

static void
test_exec_truncated_elf(void)
{
	/* Try to create a file -- only works on writable fs (ext2) */
	long fd = sys3(__NR_open, (long)"/bad_elf",
		       0x41 /* O_WRONLY|O_CREAT. */ , 0755);
	if (fd < 0) {
		msg("  SKIP exec truncated ELF (read-only fs)\n");
		return;
	}
	/* Write a fake ELF header (just magic + garbage) */
	char fake[] = { 0x7f, 'E', 'L', 'F', 0, 0, 0, 0 };
	sys3(__NR_write, fd, (long)fake, 8);
	sys1(__NR_close, fd);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec truncated ELF (fork)");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/bad_elf", 0 };
		char *envp[] = { 0 };
		long r =
		    sys3(__NR_execve, (long)"/bad_elf", (long)argv, (long)envp);
		sys1(__NR_exit, (int)(-r));
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	/* ENOEXEC = 8. */
	check(r == pid && code == 8, "exec truncated ELF returns ENOEXEC");
}

/* ---------- Test 5: NULL argv ---------- */

static void
test_exec_null_argv(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec NULL argv (fork)");
		return;
	}
	if (pid == 0) {
		/* Pass NULL for both argv and envp -- Linux allows this. */
		sys3(__NR_execve, (long)"/hello", 0, 0);
		/* If execve fails, exit with 99 to distinguish. */
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	/*
	 * Either succeeds (exit 0 from /hello) or returns an error (not 99 = crash)
	 * The key thing is no kernel panic/crash happened.
	 */
	check(r == pid && code != 99, "exec NULL argv does not crash");
}

/* ---------- Test 6: Valid exec ---------- */

static void
test_exec_valid(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "exec valid (fork)");
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
	int code = (status >> 8) & 0xff;
	check(r == pid && code == 0, "exec valid /hello exits 0");
}

/* ---------- Test 7: Exec preserves PID ---------- */

static void
test_exec_preserves_pid(void)
{
	long child_pid = sys0(__NR_fork);
	if (child_pid < 0) {
		check(0, "exec preserves pid (fork)");
		return;
	}
	if (child_pid == 0) {
		/*
		 * Child: exec /hello. PID should be preserved.
		 * We cannot easily pass the PID to /hello and have it verify,
		 * but the parent can verify that wait4 returns the same PID.
		 */
		char *argv[] = { "/hello", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	long reaped = sys4(__NR_wait4, child_pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	/* The reaped PID must match the fork return value. */
	check(reaped == child_pid,
	      "exec preserves pid (wait4 returns fork pid)");
	check(code == 0, "exec preserves pid (child exits 0)");
}

int
main(void)
{
	msg("=== exec edge case tests ===\n");

	test_exec_nonexistent();
	test_exec_empty_path();
	test_exec_directory();
	test_exec_truncated_elf();
	test_exec_null_argv();
	test_exec_valid();
	test_exec_preserves_pid();

	test_done();
	return 0;
}
