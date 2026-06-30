/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <fcntl.h>
#include <unistd.h>
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

#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_stat    4
#define SYS_link    86
#define SYS_unlink  87

#define ENOENT  2
#define EEXIST  17

/* Struct stat offsets (x86_64 Linux): st_dev(8), st_ino(8), st_nlink(8), ... */
#define STAT_INO_OFF    8
#define STAT_NLINK_OFF  16

static int
memcmp_local(const void *a, const void *b, unsigned long n)
{
	const unsigned char *p = a, *q = b;
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != q[i])
			return p[i] - q[i];
	return 0;
}

static long
do_stat(const char *path, char *buf)
{
	return sys2(SYS_stat, (long)path, (long)buf);
}

static unsigned long
get_ino(const char *statbuf)
{
	return *(unsigned long *)(statbuf + STAT_INO_OFF);
}

static unsigned long
get_nlink(const char *statbuf)
{
	return *(unsigned long *)(statbuf + STAT_NLINK_OFF);
}

/* Test 1: basic link -- create file, link it, verify data via link. */
static void
test_basic_link(void)
{
	const char *data = "hardlink test data\n";
	int dlen = 19;

	int fd = open("/orig.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "basic-link (create)");
		return;
	}
	write(fd, data, dlen);
	close(fd);

	long r = sys2(SYS_link, (long)"/orig.txt", (long)"/link.txt");
	if (r != 0) {
		check(0, "basic-link (link)");
		return;
	}

	fd = open("/link.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "basic-link (open link)");
		return;
	}
	char buf[64];
	int n = read(fd, buf, sizeof(buf));
	close(fd);
	check(n == dlen && memcmp_local(buf, data, dlen) == 0, "basic-link");
}

/* Test 2: stat nlink=2 after link. */
static void
test_nlink_2(void)
{
	char statbuf[144];	/* Struct stat is ~144 bytes on x86_64. */
	long r = do_stat("/orig.txt", statbuf);
	if (r != 0) {
		check(0, "nlink=2 (stat)");
		return;
	}
	unsigned long nlink = get_nlink(statbuf);
	if (nlink != 2) {
		msg("  [DBG] nlink=");
		print_int((int)nlink);
		msg("\n");
	}
	check(nlink == 2, "nlink=2");
}

/* Test 3: same inode for both paths. */
static void
test_same_inode(void)
{
	char statbuf1[144], statbuf2[144];
	long r1 = do_stat("/orig.txt", statbuf1);
	long r2 = do_stat("/link.txt", statbuf2);
	if (r1 != 0 || r2 != 0) {
		check(0, "same-inode (stat)");
		return;
	}
	unsigned long ino1 = get_ino(statbuf1);
	unsigned long ino2 = get_ino(statbuf2);
	check(ino1 == ino2 && ino1 != 0, "same-inode");
}

/* Test 4: unlink original, link still readable. */
static void
test_unlink_original(void)
{
	const char *data = "hardlink test data\n";
	int dlen = 19;

	long r = sys1(SYS_unlink, (long)"/orig.txt");
	if (r != 0) {
		check(0, "unlink-orig (unlink)");
		return;
	}

	/* Verify /link.txt still readable with correct data. */
	int fd = open("/link.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "unlink-orig (open)");
		return;
	}
	char buf[64];
	int n = read(fd, buf, sizeof(buf));
	close(fd);
	check(n == dlen && memcmp_local(buf, data, dlen) == 0, "unlink-orig");
}

/* Test 5: nlink drops to 1 after unlinking original. */
static void
test_nlink_1(void)
{
	char statbuf[144];
	long r = do_stat("/link.txt", statbuf);
	if (r != 0) {
		check(0, "nlink=1 (stat)");
		return;
	}
	unsigned long nlink = get_nlink(statbuf);
	check(nlink == 1, "nlink=1");
}

/* Test 6: link to nonexistent file returns -ENOENT. */
static void
test_link_nonexistent(void)
{
	long r = sys2(SYS_link, (long)"/nosuch", (long)"/other");
	check(r == -ENOENT, "link-ENOENT");
}

/* Test 7: write via one name, read via other (shared inode) */
static void
test_shared_write(void)
{
	const char *data1 = "initial content\n";
	int dlen1 = 16;
	const char *data2 = "updated content\n";
	int dlen2 = 16;

	/* Create /shared.txt. */
	int fd = open("/shared.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "shared-write (create)");
		return;
	}
	write(fd, data1, dlen1);
	close(fd);

	/* Link to /shared_link.txt. */
	long r = sys2(SYS_link, (long)"/shared.txt", (long)"/shared_link.txt");
	if (r != 0) {
		check(0, "shared-write (link)");
		return;
	}

	/* Overwrite via original path. */
	fd = open("/shared.txt", O_WRONLY | O_TRUNC);
	if (fd < 0) {
		check(0, "shared-write (open-write)");
		return;
	}
	write(fd, data2, dlen2);
	close(fd);

	/* Read via link path. */
	fd = open("/shared_link.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "shared-write (open-read)");
		return;
	}
	char buf[64];
	int n = read(fd, buf, sizeof(buf));
	close(fd);
	check(n == dlen2
	      && memcmp_local(buf, data2, dlen2) == 0, "shared-write");
}

int
main(void)
{
	msg("=== hardlink tests ===\n");
	test_basic_link();
	test_nlink_2();
	test_same_inode();
	test_unlink_original();
	test_nlink_1();
	test_link_nonexistent();
	test_shared_write();
	test_done();
	return 0;
}
