/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* --- Raw syscall wrappers --- */

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

/* --- Syscall numbers --- */

#define __NR_read      0
#define __NR_write     1
#define __NR_fork      57
#define __NR_exit      60
#define __NR_wait4     61
#define __NR_getpid    39
#define __NR_execveat  322
#define __NR_nanosleep 35
#define __NR_getrandom 318

/* --- Constants --- */

#define AT_FDCWD       -100
#define AT_EMPTY_PATH  0x1000
#define ENOENT         2

struct timespec {
	long tv_sec;
	long tv_nsec;
};

/* --- Test 1: execveat with AT_FDCWD --- */

static void
test_execveat_fdcwd(void)
{
	long pid = sys0(__NR_fork);
	if (pid == 0) {
		char *argv[] = { "/hello", (char *)0 };
		char *envp[] = { (char *)0 };
		long r =
		    sys5(__NR_execveat, AT_FDCWD, (long)"/hello", (long)argv,
			 (long)envp, 0);
		(void)r;
		sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "execveat_fdcwd: wait4");
	int code = (status >> 8) & 0xff;
	check(code == 0, "execveat_fdcwd: child exited 0");
}

/* --- Test 2: execveat with absolute path, dirfd=-1 --- */

static void
test_execveat_absolute(void)
{
	long pid = sys0(__NR_fork);
	if (pid == 0) {
		char *argv[] = { "/hello", (char *)0 };
		char *envp[] = { (char *)0 };
		long r =
		    sys5(__NR_execveat, -1, (long)"/hello", (long)argv,
			 (long)envp, 0);
		(void)r;
		sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "execveat_absolute: wait4");
	int code = (status >> 8) & 0xff;
	check(code == 0, "execveat_absolute: child exited 0");
}

/* --- Test 3: execveat with nonexistent path --- */

static void
test_execveat_enoent(void)
{
	long pid = sys0(__NR_fork);
	if (pid == 0) {
		char *argv[] = { "/no_such_binary", (char *)0 };
		char *envp[] = { (char *)0 };
		long r =
		    sys5(__NR_execveat, AT_FDCWD, (long)"/no_such_binary",
			 (long)argv, (long)envp, 0);
		if (r == -ENOENT)
			sys1(__NR_exit, 0);
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "execveat_enoent: wait4");
	int code = (status >> 8) & 0xff;
	check(code == 0, "execveat_enoent: returned -ENOENT");
}

/* --- Test 4: getrandom basic --- */

static void
test_getrandom_basic(void)
{
	char buf[32];
	for (int i = 0; i < 32; i++)
		buf[i] = 0;

	long r = sys3(__NR_getrandom, (long)buf, 32, 0);
	check(r == 32, "getrandom_basic: returned 32");

	/* Check buffer is not all zeros. */
	int nonzero = 0;
	for (int i = 0; i < 32; i++) {
		if (buf[i] != 0)
			nonzero = 1;
	}
	check(nonzero, "getrandom_basic: not all zeros");
}

/* --- Test 5: getrandom returns different data --- */

static void
test_getrandom_different(void)
{
	char buf1[16];
	char buf2[16];
	for (int i = 0; i < 16; i++) {
		buf1[i] = 0;
		buf2[i] = 0;
	}

	long r1 = sys3(__NR_getrandom, (long)buf1, 16, 0);
	/* Small delay so kernel_ticks advances (LCG seed changes) */
	struct timespec delay = { 0, 10000000 };	/* 10Ms. */
	sys2(__NR_nanosleep, (long)&delay, 0);
	long r2 = sys3(__NR_getrandom, (long)buf2, 16, 0);
	check(r1 == 16 && r2 == 16, "getrandom_different: both returned 16");

	int same = 1;
	for (int i = 0; i < 16; i++) {
		if (buf1[i] != buf2[i])
			same = 0;
	}
	check(!same, "getrandom_different: buffers differ");
}

/* --- Test 6: getrandom with zero length --- */

static void
test_getrandom_zero(void)
{
	char buf[1];
	buf[0] = 0x42;
	long r = sys3(__NR_getrandom, (long)buf, 0, 0);
	check(r == 0, "getrandom_zero: returned 0");
}

/* --- Main --- */

int
main(void)
{
	msg("=== exec_adv tests ===\n");

	test_execveat_fdcwd();
	test_execveat_absolute();
	test_execveat_enoent();
	test_getrandom_basic();
	test_getrandom_different();
	test_getrandom_zero();

	test_done();
	return 0;
}
