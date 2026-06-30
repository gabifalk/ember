/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Inline syscall helpers. */
static long
sys1(long nr, long a1)
{
	long r;
	__asm__ volatile ("syscall":"=a" (r)
			  :"a"(nr), "D"(a1)
			  :"rcx", "r11", "memory");
	return r;
}

static long
sys2(long nr, long a1, long a2)
{
	long r;
	__asm__ volatile ("syscall":"=a" (r)
			  :"a"(nr), "D"(a1), "S"(a2)
			  :"rcx", "r11", "memory");
	return r;
}

static long
sys3(long nr, long a1, long a2, long a3)
{
	long r;
	__asm__ volatile ("syscall":"=a" (r)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3)
			  :"rcx", "r11", "memory");
	return r;
}

/* Syscall numbers. */
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_stat    4
#define SYS_mkdir   83
#define SYS_rmdir   84
#define SYS_unlink  87

/* Open flags. */
#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* Error codes. */
#define ENOENT      2
#define EEXIST      17
#define ENOTEMPTY   39

/* Helper: stat a path, return 0 if exists, negative errno otherwise. */
static long
path_stat(const char *path)
{
	char statbuf[144];
	return sys2(SYS_stat, (long)path, (long)statbuf);
}

/* Test 1: mkdir basic + EEXIST on duplicate. */
static void
test_mkdir_basic(void)
{
	long r = sys2(SYS_mkdir, (long)"/testdir1", 0755);
	check(r == 0, "mkdir-basic (create)");

	r = path_stat("/testdir1");
	check(r == 0, "mkdir-basic (exists)");

	r = sys2(SYS_mkdir, (long)"/testdir1", 0755);
	check(r == -EEXIST, "mkdir-basic (EEXIST)");
}

/* Test 2: mkdir nested directories. */
static void
test_mkdir_nested(void)
{
	long r = sys2(SYS_mkdir, (long)"/testdir2", 0755);
	check(r == 0, "mkdir-nested (level1)");

	r = sys2(SYS_mkdir, (long)"/testdir2/sub1", 0755);
	check(r == 0, "mkdir-nested (level2)");

	r = sys2(SYS_mkdir, (long)"/testdir2/sub1/sub2", 0755);
	check(r == 0, "mkdir-nested (level3)");

	r = path_stat("/testdir2/sub1/sub2");
	check(r == 0, "mkdir-nested (deepest exists)");
}

/* Test 3: rmdir empty directory. */
static void
test_rmdir_empty(void)
{
	long r = sys2(SYS_mkdir, (long)"/testdir3", 0755);
	check(r == 0, "rmdir-empty (mkdir)");

	r = sys1(SYS_rmdir, (long)"/testdir3");
	check(r == 0, "rmdir-empty (rmdir)");

	r = path_stat("/testdir3");
	check(r == -ENOENT, "rmdir-empty (gone)");
}

/* Test 4: rmdir non-empty directory. */
static void
test_rmdir_notempty(void)
{
	long r = sys2(SYS_mkdir, (long)"/testdir4", 0755);
	check(r == 0, "rmdir-notempty (mkdir)");

	/* Create a file inside. */
	long fd =
	    sys3(SYS_open, (long)"/testdir4/file", O_CREAT | O_WRONLY, 0644);
	check(fd >= 0, "rmdir-notempty (create file)");
	if (fd >= 0)
		sys1(SYS_close, fd);

	/* Rmdir should fail with ENOTEMPTY. */
	r = sys1(SYS_rmdir, (long)"/testdir4");
	check(r == -ENOTEMPTY, "rmdir-notempty (ENOTEMPTY)");

	/* Unlink the file, then rmdir should succeed. */
	r = sys1(SYS_unlink, (long)"/testdir4/file");
	check(r == 0, "rmdir-notempty (unlink)");

	r = sys1(SYS_rmdir, (long)"/testdir4");
	check(r == 0, "rmdir-notempty (rmdir after unlink)");
}

/* Test 5: rmdir nonexistent directory. */
static void
test_rmdir_nonexistent(void)
{
	long r = sys1(SYS_rmdir, (long)"/testdir_nope");
	check(r == -ENOENT, "rmdir-nonexistent");
}

/* Test 6: nested cleanup -- rmdir parent fails while child exists. */
static void
test_nested_cleanup(void)
{
	long r = sys2(SYS_mkdir, (long)"/testdir5", 0755);
	check(r == 0, "nested-cleanup (mkdir parent)");

	r = sys2(SYS_mkdir, (long)"/testdir5/sub", 0755);
	check(r == 0, "nested-cleanup (mkdir child)");

	/* Rmdir parent should fail with ENOTEMPTY. */
	r = sys1(SYS_rmdir, (long)"/testdir5");
	check(r == -ENOTEMPTY, "nested-cleanup (parent ENOTEMPTY)");

	/* Rmdir child first. */
	r = sys1(SYS_rmdir, (long)"/testdir5/sub");
	check(r == 0, "nested-cleanup (rmdir child)");

	/* Now rmdir parent should succeed. */
	r = sys1(SYS_rmdir, (long)"/testdir5");
	check(r == 0, "nested-cleanup (rmdir parent)");
}

int
main(void)
{
	msg("=== mkdir/rmdir tests ===\n");
	test_mkdir_basic();
	test_mkdir_nested();
	test_rmdir_empty();
	test_rmdir_notempty();
	test_rmdir_nonexistent();
	test_nested_cleanup();
	msg("all mkdir/rmdir tests passed\n");
	test_done();
	return 0;
}
