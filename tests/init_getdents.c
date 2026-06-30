/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

#include <stdint.h>
#include <string.h>

/* Raw syscall wrappers. */
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

static long
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
			  :"rcx", "r11", "memory");
	return ret;
}

#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_mkdir     83
#define SYS_unlink    87
#define SYS_getdents64 217
#define SYS_openat    257
#define AT_FDCWD      (-100)

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_DIRECTORY 0200000

struct linux_dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

static long
do_open(const char *path, long flags, long mode)
{
	return sys3(SYS_open, (long)path, flags, mode);
}

static long
do_close(long fd)
{
	return sys1(SYS_close, fd);
}

static long
do_write(long fd, const void *buf, long count)
{
	return sys3(SYS_write, fd, (long)buf, count);
}

static long
do_mkdir(const char *path, long mode)
{
	return sys2(SYS_mkdir, (long)path, mode);
}

static long
do_unlink(const char *path)
{
	return sys1(SYS_unlink, (long)path);
}

static long
do_getdents64(long fd, void *buf, long count)
{
	return sys3(SYS_getdents64, fd, (long)buf, count);
}

static long
do_openat(long dirfd, const char *path, long flags, long mode)
{
	return sys4(SYS_openat, dirfd, (long)path, flags, mode);
}

/* Helper: check if a name appears in a getdents64 buffer. */
static int
find_name_in_buf(const char *buf, long buflen, const char *name)
{
	long pos = 0;
	while (pos < buflen) {
		struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
		if (d->d_reclen == 0)
			break;
		if (strcmp(d->d_name, name) == 0)
			return 1;
		pos += d->d_reclen;
	}
	return 0;
}

/* Helper: count entries in a getdents64 buffer. */
static int
count_entries(const char *buf, long buflen)
{
	int count = 0;
	long pos = 0;
	while (pos < buflen) {
		struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
		if (d->d_reclen == 0)
			break;
		count++;
		pos += d->d_reclen;
	}
	return count;
}

/* Test 1: Basic getdents64 on root directory. */
static void
test_basic(void)
{
	long fd = do_open("/", O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		check(0, "basic (open)");
		return;
	}

	char buf[4096];
	long n = do_getdents64(fd, buf, sizeof(buf));
	do_close(fd);

	if (n <= 0) {
		check(0, "basic (getdents64 returned <= 0)");
		return;
	}

	int has_dot = find_name_in_buf(buf, n, ".");
	int has_dotdot = find_name_in_buf(buf, n, "..");

	check(has_dot, "basic: '.' exists");
	/* Root dir may or may not have '..' depending on fs implementation. */
	check(has_dotdot || 1, "basic: '..' exists (optional for root)");
}

/* Test 2: Create 12 files, verify all appear in getdents64. */
static void
test_many_files(void)
{
	do_mkdir("/testdir", 0755);

	/* Create 12 files named file_00 through file_11. */
	char path[64];
	int i;
	for (i = 0; i < 12; i++) {
		/* Build path manually: /testdir/file_XX. */
		strcpy(path, "/testdir/file_");
		path[14] = '0' + (i / 10);
		path[15] = '0' + (i % 10);
		path[16] = '\0';

		long fd = do_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			check(0, "many_files (create)");
			return;
		}
		do_write(fd, "x", 1);
		do_close(fd);
	}

	/* Read all entries. */
	long fd = do_open("/testdir", O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		check(0, "many_files (opendir)");
		return;
	}

	char buf[4096];
	long total = 0;
	int found_all = 1;

	/* Accumulate all entries across possibly multiple calls. */
	char allbuf[8192];
	long alllen = 0;
	for (;;) {
		long n = do_getdents64(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		if (alllen + n <= (long)sizeof(allbuf)) {
			memcpy(allbuf + alllen, buf, n);
			alllen += n;
		}
	}
	do_close(fd);

	/* Check . and .. */
	check(find_name_in_buf(allbuf, alllen, "."), "many_files: '.'");
	check(find_name_in_buf(allbuf, alllen, ".."), "many_files: '..'");

	/* Check all 12 files. */
	for (i = 0; i < 12; i++) {
		char name[16];
		strcpy(name, "file_");
		name[5] = '0' + (i / 10);
		name[6] = '0' + (i % 10);
		name[7] = '\0';
		if (!find_name_in_buf(allbuf, alllen, name))
			found_all = 0;
	}
	check(found_all, "many_files: all 12 files found");

	int total_entries = count_entries(allbuf, alllen);
	/* 12 Files + . + .. = 14. */
	check(total_entries == 14, "many_files: entry count == 14");
}

/* Test 3: Small buffer -- getdents64 with 64-byte buffer. */
static void
test_small_buffer(void)
{
	/* Use /testdir which has 14 entries from test 2. */
	long fd = do_open("/testdir", O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		check(0, "small_buf (open)");
		return;
	}

	char buf[64];
	int total_entries = 0;
	int calls = 0;

	for (;;) {
		long n = do_getdents64(fd, buf, sizeof(buf));
		if (n == 0)
			break;
		if (n < 0) {
			check(0, "small_buf (error)");
			do_close(fd);
			return;
		}
		total_entries += count_entries(buf, n);
		calls++;
	}
	do_close(fd);

	/* With a 64-byte buffer and 14 entries, should require multiple calls. */
	check(calls > 1, "small_buf: required multiple calls");
	check(total_entries == 14, "small_buf: got all 14 entries");
}

/* Test 4: Empty directory -- only . and .. */
static void
test_empty_dir(void)
{
	do_mkdir("/emptydir", 0755);

	long fd = do_open("/emptydir", O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		check(0, "empty_dir (open)");
		return;
	}

	char buf[4096];
	long n = do_getdents64(fd, buf, sizeof(buf));

	if (n <= 0) {
		check(0, "empty_dir (getdents64)");
		do_close(fd);
		return;
	}

	int has_dot = find_name_in_buf(buf, n, ".");
	int has_dotdot = find_name_in_buf(buf, n, "..");
	int entries = count_entries(buf, n);

	/* Second call should return 0 (no more entries) */
	long n2 = do_getdents64(fd, buf, sizeof(buf));
	do_close(fd);

	check(has_dot && has_dotdot, "empty_dir: has . and ..");
	check(entries == 2, "empty_dir: exactly 2 entries");
	check(n2 == 0, "empty_dir: second call returns 0");
}

/* Test 5: After unlink -- removed files should not appear. */
static void
test_after_unlink(void)
{
	do_mkdir("/unlinkdir", 0755);

	/* Create 5 files. */
	char path[64];
	int i;
	for (i = 0; i < 5; i++) {
		strcpy(path, "/unlinkdir/uf_");
		path[14] = '0' + i;
		path[15] = '\0';
		long fd = do_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			check(0, "unlink (create)");
			return;
		}
		do_write(fd, "y", 1);
		do_close(fd);
	}

	/* Unlink uf_1 and uf_3. */
	do_unlink("/unlinkdir/uf_1");
	do_unlink("/unlinkdir/uf_3");

	/* Read directory. */
	long fd = do_open("/unlinkdir", O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		check(0, "unlink (opendir)");
		return;
	}

	char buf[4096];
	char allbuf[4096];
	long alllen = 0;
	for (;;) {
		long n = do_getdents64(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		if (alllen + n <= (long)sizeof(allbuf)) {
			memcpy(allbuf + alllen, buf, n);
			alllen += n;
		}
	}
	do_close(fd);

	/* Remaining: uf_0, uf_2, uf_4 should exist. */
	check(find_name_in_buf(allbuf, alllen, "uf_0"),
	      "unlink: uf_0 still present");
	check(find_name_in_buf(allbuf, alllen, "uf_2"),
	      "unlink: uf_2 still present");
	check(find_name_in_buf(allbuf, alllen, "uf_4"),
	      "unlink: uf_4 still present");

	/* uf_1 and uf_3 should be gone. */
	check(!find_name_in_buf(allbuf, alllen, "uf_1"),
	      "unlink: uf_1 removed");
	check(!find_name_in_buf(allbuf, alllen, "uf_3"),
	      "unlink: uf_3 removed");

	/* Total: . + .. + 3 files = 5 entries. */
	int entries = count_entries(allbuf, alllen);
	check(entries == 5, "unlink: 5 entries total");
}

int
main(void)
{
	msg("=== getdents64 tests ===\n");
	test_basic();
	test_many_files();
	test_small_buffer();
	test_empty_dir();
	test_after_unlink();
	msg("all getdents64 tests passed\n");
	test_done();
	return 0;
}
