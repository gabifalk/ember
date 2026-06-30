/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <fcntl.h>
#include <unistd.h>
#include "test_common.h"

/* Inline syscall helpers. */
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

#define SYS_open    2
#define SYS_stat    4
#define SYS_rename  82
#define SYS_mkdir   83
#define SYS_unlink  87

#define ENOENT  2

static int
memcmp_local(const void *a, const void *b, unsigned long n)
{
	const unsigned char *p = a, *q = b;
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != q[i])
			return p[i] - q[i];
	return 0;
}

/* Helper: create a file with given data, return 0 on success. */
static int
create_file(const char *path, const char *data, int len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	write(fd, data, len);
	close(fd);
	return 0;
}

/* Helper: read file contents, return bytes read or -1. */
static int
read_file(const char *path, char *buf, int bufsz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	int n = read(fd, buf, bufsz);
	close(fd);
	return n;
}

/* Test 1: basic rename. */
static void
test_basic_rename(void)
{
	const char *data = "hello rename\n";
	int dlen = 13;

	if (create_file("/a.txt", data, dlen) < 0) {
		check(0, "basic-rename (create)");
		return;
	}

	long r = sys2(SYS_rename, (long)"/a.txt", (long)"/b.txt");
	if (r != 0) {
		check(0, "basic-rename (rename)");
		return;
	}

	/* Verify b.txt has the data. */
	char buf[64];
	int n = read_file("/b.txt", buf, sizeof(buf));
	check(n == dlen && memcmp_local(buf, data, dlen) == 0,
	      "basic-rename (data)");

	/* Verify a.txt is gone (ENOENT) */
	long fd = sys2(SYS_open, (long)"/a.txt", O_RDONLY);
	check(fd == -ENOENT, "basic-rename (ENOENT)");
	if (fd >= 0)
		close((int)fd);
}

/* Test 2: rename over existing (atomic replace) */
static void
test_rename_over_existing(void)
{
	const char *src_data = "source data\n";
	int src_len = 12;
	const char *dst_data = "dest data\n";
	int dst_len = 10;

	if (create_file("/src.txt", src_data, src_len) < 0) {
		check(0, "rename-over (create src)");
		return;
	}
	if (create_file("/dst.txt", dst_data, dst_len) < 0) {
		check(0, "rename-over (create dst)");
		return;
	}

	long r = sys2(SYS_rename, (long)"/src.txt", (long)"/dst.txt");
	if (r != 0) {
		check(0, "rename-over (rename)");
		return;
	}

	/* Dst.txt should now have src's data. */
	char buf[64];
	int n = read_file("/dst.txt", buf, sizeof(buf));
	check(n == src_len && memcmp_local(buf, src_data, src_len) == 0,
	      "rename-over (data)");

	/* Src.txt should be gone. */
	long fd = sys2(SYS_open, (long)"/src.txt", O_RDONLY);
	check(fd == -ENOENT, "rename-over (ENOENT)");
	if (fd >= 0)
		close((int)fd);
}

/* Test 3: rename across directories. */
static void
test_rename_across_dirs(void)
{
	const char *data = "cross-dir data\n";
	int dlen = 15;

	sys2(SYS_mkdir, (long)"/subA", 0755);
	sys2(SYS_mkdir, (long)"/subB", 0755);

	if (create_file("/subA/file.txt", data, dlen) < 0) {
		check(0, "cross-dir (create)");
		return;
	}

	long r =
	    sys2(SYS_rename, (long)"/subA/file.txt", (long)"/subB/file.txt");
	if (r != 0) {
		check(0, "cross-dir (rename)");
		return;
	}

	/* Verify /subB/file.txt has the data. */
	char buf[64];
	int n = read_file("/subB/file.txt", buf, sizeof(buf));
	check(n == dlen && memcmp_local(buf, data, dlen) == 0,
	      "cross-dir (data)");

	/* Verify /subA/file.txt is gone. */
	long fd = sys2(SYS_open, (long)"/subA/file.txt", O_RDONLY);
	check(fd == -ENOENT, "cross-dir (ENOENT)");
	if (fd >= 0)
		close((int)fd);
}

/* Test 4: rename directory. */
static void
test_rename_directory(void)
{
	const char *data = "inside dir\n";
	int dlen = 11;

	sys2(SYS_mkdir, (long)"/dirX", 0755);

	if (create_file("/dirX/inner.txt", data, dlen) < 0) {
		check(0, "rename-dir (create)");
		return;
	}

	long r = sys2(SYS_rename, (long)"/dirX", (long)"/dirY");
	if (r != 0) {
		check(0, "rename-dir (rename)");
		return;
	}

	/* Verify /dirY/inner.txt has the data. */
	char buf[64];
	int n = read_file("/dirY/inner.txt", buf, sizeof(buf));
	check(n == dlen && memcmp_local(buf, data, dlen) == 0,
	      "rename-dir (data)");

	/* Verify /dirX is gone -- stat should return ENOENT. */
	char statbuf[144];	/* Struct stat. */
	long sr = sys2(SYS_stat, (long)"/dirX", (long)statbuf);
	check(sr == -ENOENT, "rename-dir (ENOENT)");
}

/* Test 5: rename to self (no-op) */
static void
test_rename_self(void)
{
	const char *data = "self rename\n";
	int dlen = 12;

	if (create_file("/self.txt", data, dlen) < 0) {
		check(0, "rename-self (create)");
		return;
	}

	long r = sys2(SYS_rename, (long)"/self.txt", (long)"/self.txt");
	check(r == 0, "rename-self (retval)");

	/* Verify file still has the data. */
	char buf[64];
	int n = read_file("/self.txt", buf, sizeof(buf));
	check(n == dlen && memcmp_local(buf, data, dlen) == 0,
	      "rename-self (data)");
}

/* Test 6: rename nonexistent file. */
static void
test_rename_nonexistent(void)
{
	long r = sys2(SYS_rename, (long)"/nosuch", (long)"/other");
	check(r == -ENOENT, "rename-nonexistent");
}

int
main(void)
{
	msg("=== rename tests ===\n");
	test_basic_rename();
	test_rename_over_existing();
	test_rename_across_dirs();
	test_rename_directory();
	test_rename_self();
	test_rename_nonexistent();
	msg("all rename tests passed\n");
	test_done();
	return 0;
}
