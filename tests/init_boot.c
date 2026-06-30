/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Boot regression test -- minimal sanity checks that the kernel booted
 * correctly and basic syscalls work.
 */

#include "test_common.h"

#define __NR_write      1
#define __NR_getpid     39
#define __NR_uname      63
#define __NR_brk        12

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

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

int
main(void)
{
	msg("=== boot regression test ===\n");

	/* Test 1: We are PID 1 (init) */
	long pid = sys0(__NR_getpid);
	check(pid == 1, "getpid == 1 (we are init)");

	/* Test 2: write to stdout works. */
	long w = sys3(__NR_write, 1, (long)"ok\n", 3);
	check(w == 3, "write to stdout");

	/* Test 3: brk works (query current brk) */
	long brk_val = sys1(__NR_brk, 0);
	check(brk_val > 0, "brk query returns positive");

	/* Test 4: uname works. */
	struct utsname uts;
	/* Zero it. */
	char *p = (char *)&uts;
	for (int i = 0; i < (int)sizeof(uts); i++)
		p[i] = 0;
	long r = sys1(__NR_uname, (long)&uts);
	check(r == 0, "uname succeeds");
	/* Verify sysname is non-empty. */
	check(uts.sysname[0] != 0, "uname sysname non-empty");

	test_done();
	return 0;
}
