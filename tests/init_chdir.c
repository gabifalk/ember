/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
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
#define SYS_fork    57
#define SYS_exit    60
#define SYS_wait4   61
#define SYS_chdir   80
#define SYS_getcwd  79
#define SYS_mkdir   83

/* Error codes. */
#define ENOENT  2
#define ENOTDIR 20

static int
strcmp_local(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

/* Test 1: getcwd should return "/" at startup. */
static void
test_initial_getcwd(void)
{
	char buf[256];
	long r = sys2(SYS_getcwd, (long)buf, 256);
	check(r > 0 && strcmp_local(buf, "/") == 0, "initial-getcwd");
}

/* Test 2: chdir to directory, getcwd returns new path. */
static void
test_chdir_to_dir(void)
{
	long r = sys2(SYS_mkdir, (long)"/testdir", 0755);
	if (r != 0) {
		check(0, "chdir-to-dir (mkdir)");
		return;
	}

	r = sys1(SYS_chdir, (long)"/testdir");
	if (r != 0) {
		check(0, "chdir-to-dir (chdir)");
		return;
	}

	char buf[256];
	r = sys2(SYS_getcwd, (long)buf, 256);
	check(r > 0 && strcmp_local(buf, "/testdir") == 0, "chdir-to-dir");
}

/* Test 3: relative open after chdir. */
static void
test_relative_open(void)
{
	/* Cwd should be /testdir from test 2. */
	int fd = sys3(SYS_open, (long)"file.txt", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		check(0, "relative-open (create)");
		return;
	}
	const char *data = "hello\n";
	sys3(SYS_write, fd, (long)data, 6);
	sys1(SYS_close, fd);

	/* Verify it exists at /testdir/file.txt via absolute path. */
	fd = sys2(SYS_open, (long)"/testdir/file.txt", O_RDONLY);
	check(fd >= 0, "relative-open");
	if (fd >= 0)
		sys1(SYS_close, fd);
}

/* Test 4: chdir with "..". */
static void
test_chdir_dotdot(void)
{
	/* Cwd is /testdir from test 2. */
	long r = sys1(SYS_chdir, (long)"..");
	if (r != 0) {
		check(0, "chdir-dotdot (chdir)");
		return;
	}

	char buf[256];
	r = sys2(SYS_getcwd, (long)buf, 256);
	check(r > 0 && strcmp_local(buf, "/") == 0, "chdir-dotdot");
}

/* Test 5: chdir to nested directory. */
static void
test_chdir_nested(void)
{
	sys2(SYS_mkdir, (long)"/a", 0755);
	sys2(SYS_mkdir, (long)"/a/b", 0755);

	long r = sys1(SYS_chdir, (long)"/a/b");
	if (r != 0) {
		check(0, "chdir-nested (chdir)");
		return;
	}

	char buf[256];
	r = sys2(SYS_getcwd, (long)buf, 256);
	check(r > 0 && strcmp_local(buf, "/a/b") == 0, "chdir-nested");

	/* Return to / for subsequent tests. */
	sys1(SYS_chdir, (long)"/");
}

/* Test 6: chdir to nonexistent directory. */
static void
test_chdir_nonexistent(void)
{
	long r = sys1(SYS_chdir, (long)"/nosuchdir");
	check(r == -ENOENT, "chdir-nonexistent");
}

/* Test 7: chdir to a regular file. */
static void
test_chdir_to_file(void)
{
	int fd = sys3(SYS_open, (long)"/plainfile", O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		check(0, "chdir-to-file (create)");
		return;
	}
	sys1(SYS_close, fd);

	long r = sys1(SYS_chdir, (long)"/plainfile");
	check(r == -ENOTDIR, "chdir-to-file");
}

/* Test 8: chdir in child does not affect parent. */
static void
test_chdir_child_isolation(void)
{
	/* Ensure we are at / */
	sys1(SYS_chdir, (long)"/");

	long pid = sys1(SYS_fork, 0);
	if (pid < 0) {
		check(0, "chdir-child (fork)");
		return;
	}

	if (pid == 0) {
		/* Child: chdir to /testdir. */
		sys1(SYS_chdir, (long)"/testdir");
		char buf[256];
		long r = sys2(SYS_getcwd, (long)buf, 256);
		check(r > 0
		      && strcmp_local(buf, "/testdir") == 0,
		      "chdir-child (child-cwd)");
		sys1(SYS_exit, 0);
		__builtin_unreachable();
	}

	/* Parent: wait for child. */
	int status = 0;
	long w;
	do {
		w = sys3(SYS_wait4, pid, (long)&status, 0);
	} while (w == -4);	/* EINTR. */

	/* Parent cwd should still be "/". */
	char buf[256];
	long r = sys2(SYS_getcwd, (long)buf, 256);
	check(r > 0 && strcmp_local(buf, "/") == 0, "chdir-child (parent-cwd)");
}

int
main(void)
{
	msg("=== chdir tests ===\n");
	test_initial_getcwd();
	test_chdir_to_dir();
	test_relative_open();
	test_chdir_dotdot();
	test_chdir_nested();
	test_chdir_nonexistent();
	test_chdir_to_file();
	test_chdir_child_isolation();
	msg("all chdir tests passed\n");
	test_done();
	return 0;
}
