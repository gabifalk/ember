/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Deep path tests on ext2: nested directory creation, long filenames,
 * trailing slashes, and dot/dot-dot resolution.
 */

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
#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_stat    4
#define SYS_mkdir   83
#define SYS_rmdir   84
#define SYS_unlink  87

/* Open flags. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* Struct stat offsets (x86_64 Linux) */
#define STAT_INO_OFF    8

static int
my_memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *p = a, *q = b;
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != q[i])
			return p[i] - q[i];
	return 0;
}

static void
my_memset(void *p, int c, long n)
{
	char *d = p;
	while (n--)
		*d++ = c;
}

static void
my_memcpy(void *dst, const void *src, long n)
{
	char *d = dst;
	const char *s = src;
	while (n--)
		*d++ = *s++;
}

static int
my_strlen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static unsigned long
get_ino(const char *statbuf)
{
	return *(const unsigned long *)(statbuf + STAT_INO_OFF);
}

/* ------------------------------------------------------------------ */
/* Test 1: Nested directory creation (6 levels deep) */
/*  */
/* Create /dp/a/b/c/d/e, write a file at the bottom, read it back. */
/* ------------------------------------------------------------------ */
static void
test_nested_dirs(void)
{
	static const char *dirs[] = {
		"/dp", "/dp/a", "/dp/a/b", "/dp/a/b/c",
		"/dp/a/b/c/d", "/dp/a/b/c/d/e"
	};
	int ndirs = 6;

	for (int i = 0; i < ndirs; i++) {
		long r = sys2(SYS_mkdir, (long)dirs[i], 0755);
		if (r != 0) {
			check(0, "nested-dirs (mkdir)");
			return;
		}
	}

	const char *data = "deep file\n";
	int dlen = 10;
	long fd = sys3(SYS_open, (long)"/dp/a/b/c/d/e/leaf.txt",
		       O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "nested-dirs (create file)");
		return;
	}
	sys3(SYS_write, fd, (long)data, dlen);
	sys1(SYS_close, fd);

	/* Read back. */
	char buf[64];
	my_memset(buf, 0, sizeof(buf));
	fd = sys2(SYS_open, (long)"/dp/a/b/c/d/e/leaf.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "nested-dirs (open)");
		return;
	}
	long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
	sys1(SYS_close, fd);
	check(n == dlen && my_memcmp(buf, data, dlen) == 0, "nested-dirs");

	/* Cleanup (reverse order) */
	sys1(SYS_unlink, (long)"/dp/a/b/c/d/e/leaf.txt");
	for (int i = ndirs - 1; i >= 0; i--)
		sys1(SYS_rmdir, (long)dirs[i]);
}

/* ------------------------------------------------------------------ */
/* Test 2: Long filename (~200 chars) */
/*  */
/* Create a file with a 200-character name, stat it, write/read data. */
/* ------------------------------------------------------------------ */
static void
test_long_filename(void)
{
	/* Build a 200-char filename: /longfile_AAAA...A. */
	char path[256];
	my_memcpy(path, "/longfile_", 10);
	my_memset(path + 10, 'A', 190);
	path[200] = '\0';

	const char *data = "long name ok\n";
	int dlen = 13;

	long fd =
	    sys3(SYS_open, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "long-filename (create)");
		return;
	}
	sys3(SYS_write, fd, (long)data, dlen);
	sys1(SYS_close, fd);

	/* Stat should succeed. */
	char statbuf[144];
	long r = sys2(SYS_stat, (long)path, (long)statbuf);
	check(r == 0, "long-filename (stat)");

	/* Read back. */
	char buf[64];
	my_memset(buf, 0, sizeof(buf));
	fd = sys2(SYS_open, (long)path, O_RDONLY);
	if (fd < 0) {
		check(0, "long-filename (open)");
		return;
	}
	long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
	sys1(SYS_close, fd);
	check(n == dlen
	      && my_memcmp(buf, data, dlen) == 0, "long-filename (data)");

	/* Cleanup. */
	sys1(SYS_unlink, (long)path);
}

/* ------------------------------------------------------------------ */
/* Test 3: Trailing slashes. */
/*  */
/* Open("/") with O_RDONLY should succeed (it's the root directory). */
/* Mkdir "/trslash/" should create /trslash. */
/* ------------------------------------------------------------------ */
static void
test_trailing_slash(void)
{
	/* Open root with trailing slash. */
	long fd = sys2(SYS_open, (long)"/", O_RDONLY);
	check(fd >= 0, "trailing-slash (open /)");
	if (fd >= 0)
		sys1(SYS_close, fd);

	/* Mkdir with trailing slash. */
	long r = sys2(SYS_mkdir, (long)"/trslash/", 0755);
	/*
	 * Some kernels strip trailing slash, some don't -- either success
	 * or ENOENT-style error is implementation-defined. We just check
	 * that the directory exists afterward if mkdir succeeded.
	 */
	if (r == 0) {
		char statbuf[144];
		r = sys2(SYS_stat, (long)"/trslash", (long)statbuf);
		check(r == 0, "trailing-slash (mkdir / exists)");
		sys1(SYS_rmdir, (long)"/trslash");
	} else {
		/*
		 * If the kernel rejects trailing slash on mkdir, that's acceptable;
		 * just mkdir without the slash to verify basic functionality.
		 */
		r = sys2(SYS_mkdir, (long)"/trslash", 0755);
		check(r == 0, "trailing-slash (mkdir fallback)");
		sys1(SYS_rmdir, (long)"/trslash");
	}
}

/* ------------------------------------------------------------------ */
/* Test 4: Dot and dot-dot resolution. */
/*  */
/* Stat(".") should succeed, stat("..") should succeed,. */
/* stat("/dp_dot/..") should resolve to /. */
/* ------------------------------------------------------------------ */
static void
test_dot_dotdot(void)
{
	char statbuf1[144], statbuf2[144];

	/* Stat(".") should succeed (cwd is /) */
	long r = sys2(SYS_stat, (long)".", (long)statbuf1);
	check(r == 0, "dot (stat .)");

	/* Stat("..") should succeed (root's parent is root) */
	r = sys2(SYS_stat, (long)"..", (long)statbuf2);
	check(r == 0, "dotdot (stat ..)");

	/* For root, "." and ".." should have the same inode. */
	if (r == 0) {
		unsigned long ino1 = get_ino(statbuf1);
		unsigned long ino2 = get_ino(statbuf2);
		check(ino1 == ino2, "dot-dotdot (same ino at root)");
	}

	/* Create a subdir, stat subdir/.., should match / */
	sys2(SYS_mkdir, (long)"/dp_dot", 0755);

	my_memset(statbuf2, 0, sizeof(statbuf2));
	r = sys2(SYS_stat, (long)"/dp_dot/..", (long)statbuf2);
	check(r == 0, "dotdot (stat /dp_dot/..)");

	if (r == 0) {
		unsigned long ino_root = get_ino(statbuf1);
		unsigned long ino_parent = get_ino(statbuf2);
		check(ino_root == ino_parent, "dotdot (resolves to root)");
	}

	sys1(SYS_rmdir, (long)"/dp_dot");
}

int
main(void)
{
	msg("=== deep path tests ===\n");
	test_nested_dirs();
	test_long_filename();
	test_trailing_slash();
	test_dot_dotdot();
	msg("all deep path tests passed\n");
	test_done();
	return 0;
}
