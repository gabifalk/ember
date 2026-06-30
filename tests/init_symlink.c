/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Raw syscall wrappers (no libc) */
static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3)
			  :"rcx", "r11", "memory");
	return ret;
}

#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_STAT     4
#define SYS_LSTAT    6
#define SYS_MKDIR    83
#define SYS_UNLINK   87
#define SYS_SYMLINK  88
#define SYS_READLINK 89

/* Open flags. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_CREAT   0100

/* Errno values. */
#define ENOENT  2
#define ELOOP   40

/* Stat mode constants. */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFLNK  0120000

/* Struct stat offsets (x86_64 Linux) */
#define STAT_MODE_OFF  24

typedef char statbuf_t[144];

static int
memcmp_local(const void *a, const void *b, unsigned long n)
{
	const unsigned char *p = a, *q = b;
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != q[i])
			return p[i] - q[i];
	return 0;
}

static int
strlen_local(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static long
do_open(const char *path, long flags, long mode)
{
	return sys3(SYS_OPEN, (long)path, flags, mode);
}

static long
do_close(long fd)
{
	return sys1(SYS_CLOSE, fd);
}

static long
do_read(long fd, void *buf, long count)
{
	return sys3(SYS_READ, fd, (long)buf, count);
}

static long
do_write(long fd, const void *buf, long count)
{
	return sys3(SYS_WRITE, fd, (long)buf, count);
}

static long
do_stat(const char *path, void *buf)
{
	return sys2(SYS_STAT, (long)path, (long)buf);
}

static long
do_lstat(const char *path, void *buf)
{
	return sys2(SYS_LSTAT, (long)path, (long)buf);
}

static unsigned int
get_st_mode(const void *buf)
{
	return *(const unsigned int *)((const char *)buf + STAT_MODE_OFF);
}

/* Test 1: create file, write data, create symlink, verify readlink returns target. */
static void
test_readlink(void)
{
	const char *data = "symlink test data\n";
	int dlen = 18;

	/* Create /orig.txt and write data. */
	long fd = do_open("/orig.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "readlink (create)");
		return;
	}
	do_write(fd, data, dlen);
	do_close(fd);

	/* Create symlink /slink -> /orig.txt. */
	long r = sys2(SYS_SYMLINK, (long)"/orig.txt", (long)"/slink");
	if (r != 0) {
		check(0, "readlink (symlink)");
		return;
	}

	/* Readlink should return "/orig.txt". */
	char buf[256];
	for (int i = 0; i < 256; i++)
		buf[i] = 0;
	r = sys3(SYS_READLINK, (long)"/slink", (long)buf, 256);
	const char *expect = "/orig.txt";
	int elen = strlen_local(expect);
	check(r == elen && memcmp_local(buf, expect, elen) == 0, "readlink");
}

/* Test 2: open through symlink and verify data matches original file. */
static void
test_open_through_symlink(void)
{
	const char *data = "symlink test data\n";
	int dlen = 18;

	long fd = do_open("/slink", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "open-through (open)");
		return;
	}
	char buf[64];
	long n = do_read(fd, buf, sizeof(buf));
	do_close(fd);
	check(n == dlen && memcmp_local(buf, data, dlen) == 0, "open-through");
}

/* Test 3: stat follows symlink (shows regular file), lstat shows S_IFLNK. */
static void
test_stat_vs_lstat(void)
{
	statbuf_t st;

	/* Stat on symlink should follow -> regular file. */
	long r = do_stat("/slink", &st);
	if (r < 0) {
		check(0, "stat-follow (stat)");
		return;
	}
	unsigned int mode = get_st_mode(&st);
	check((mode & S_IFMT) == S_IFREG, "stat-follow");

	/* Lstat on symlink should show S_IFLNK. */
	r = do_lstat("/slink", &st);
	if (r < 0) {
		check(0, "lstat-link (lstat)");
		return;
	}
	mode = get_st_mode(&st);
	check((mode & S_IFMT) == S_IFLNK, "lstat-link");
}

/* Test 4: dangling symlink -- symlink to nonexistent path, open returns ENOENT. */
static void
test_dangling_symlink(void)
{
	long r = sys2(SYS_SYMLINK, (long)"/no_such_file", (long)"/dangling");
	if (r != 0) {
		check(0, "dangling (symlink)");
		return;
	}

	long fd = do_open("/dangling", O_RDONLY, 0);
	check(fd == -ENOENT, "dangling-ENOENT");
	if (fd >= 0)
		do_close(fd);
}

/* Test 5: symlink loop -- A->B and B->A, open returns ELOOP. */
static void
test_symlink_loop(void)
{
	/* /Loop_a -> /loop_b. */
	long r = sys2(SYS_SYMLINK, (long)"/loop_b", (long)"/loop_a");
	if (r != 0) {
		check(0, "loop (symlink A)");
		return;
	}

	/* /Loop_b -> /loop_a. */
	r = sys2(SYS_SYMLINK, (long)"/loop_a", (long)"/loop_b");
	if (r != 0) {
		check(0, "loop (symlink B)");
		return;
	}

	/* Open should fail with ELOOP. */
	long fd = do_open("/loop_a", O_RDONLY, 0);
	check(fd == -ELOOP, "loop-ELOOP");
	if (fd >= 0)
		do_close(fd);
}

int
main(void)
{
	msg("=== symlink tests ===\n");
	test_readlink();
	test_open_through_symlink();
	test_stat_vs_lstat();
	test_dangling_symlink();
	test_symlink_loop();
	test_done();
	return 0;
}
