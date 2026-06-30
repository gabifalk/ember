/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * VFS edge-case tests on ext2: unlink-while-open, hard link across
 * directories, rename over existing, rmdir non-empty, unlink directory.
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
#define SYS_lseek   8
#define SYS_rename  82
#define SYS_mkdir   83
#define SYS_rmdir   84
#define SYS_link    86
#define SYS_unlink  87

/* Open flags. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* Error codes. */
#define ENOENT      2
#define EISDIR      21
#define ENOTEMPTY   39
#define EPERM       1

/* SEEK_SET. */
#define SEEK_SET    0

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

/* ------------------------------------------------------------------ */
/* Test 1: Unlink while file is open. */
/*  */
/* Create a file, open it, unlink the name, verify read/write still. */
/* Works on the open fd, then close. */
/* ------------------------------------------------------------------ */
static void
test_unlink_while_open(void)
{
	const char *data = "open-unlink test\n";
	int dlen = 17;

	/* Create and write. */
	long fd =
	    sys3(SYS_open, (long)"/uwo.txt", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (fd < 0) {
		check(0, "unlink-open (create)");
		return;
	}
	sys3(SYS_write, fd, (long)data, dlen);
	sys1(SYS_close, fd);

	/* Open for read/write, then unlink the name. */
	fd = sys2(SYS_open, (long)"/uwo.txt", O_RDWR);
	if (fd < 0) {
		check(0, "unlink-open (open rw)");
		return;
	}

	long r = sys1(SYS_unlink, (long)"/uwo.txt");
	check(r == 0, "unlink-open (unlink ret)");

	/* Name should be gone. */
	char statbuf[144];
	r = sys2(SYS_stat, (long)"/uwo.txt", (long)statbuf);
	check(r == -ENOENT, "unlink-open (name gone)");

	/* Read from the still-open fd. */
	char buf[64];
	my_memset(buf, 0, sizeof(buf));
	long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
	check(n == dlen && my_memcmp(buf, data, dlen) == 0,
	      "unlink-open (read ok)");

	/* Write more data to the still-open fd. */
	const char *extra = "extra";
	int elen = 5;
	long wr = sys3(SYS_write, fd, (long)extra, elen);
	check(wr == elen, "unlink-open (write ok)");

	sys1(SYS_close, fd);
}

/* ------------------------------------------------------------------ */
/* Test 2: Hard link across directories. */
/*  */
/* Mkdir /hld1 /hld2, create /hld1/f, link to /hld2/g, verify data. */
/* Via /hld2/g, unlink /hld1/f, verify /hld2/g still readable. */
/* ------------------------------------------------------------------ */
static void
test_hardlink_across_dirs(void)
{
	const char *data = "cross-dir link\n";
	int dlen = 15;

	sys2(SYS_mkdir, (long)"/hld1", 0755);
	sys2(SYS_mkdir, (long)"/hld2", 0755);

	long fd =
	    sys3(SYS_open, (long)"/hld1/f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "xdir-link (create)");
		return;
	}
	sys3(SYS_write, fd, (long)data, dlen);
	sys1(SYS_close, fd);

	long r = sys2(SYS_link, (long)"/hld1/f", (long)"/hld2/g");
	if (r != 0) {
		check(0, "xdir-link (link)");
		return;
	}

	/* Verify /hld2/g has same content. */
	char buf[64];
	my_memset(buf, 0, sizeof(buf));
	fd = sys2(SYS_open, (long)"/hld2/g", O_RDONLY);
	if (fd < 0) {
		check(0, "xdir-link (open link)");
		return;
	}
	long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
	sys1(SYS_close, fd);
	check(n == dlen && my_memcmp(buf, data, dlen) == 0, "xdir-link (data)");

	/* Unlink original, link should survive. */
	r = sys1(SYS_unlink, (long)"/hld1/f");
	check(r == 0, "xdir-link (unlink orig)");

	my_memset(buf, 0, sizeof(buf));
	fd = sys2(SYS_open, (long)"/hld2/g", O_RDONLY);
	if (fd < 0) {
		check(0, "xdir-link (open after unlink)");
		return;
	}
	n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
	sys1(SYS_close, fd);
	check(n == dlen && my_memcmp(buf, data, dlen) == 0,
	      "xdir-link (survives unlink)");

	/* Cleanup. */
	sys1(SYS_unlink, (long)"/hld2/g");
	sys1(SYS_rmdir, (long)"/hld1");
	sys1(SYS_rmdir, (long)"/hld2");
}

/* ------------------------------------------------------------------ */
/* Test 3: Rename over existing file. */
/*  */
/* Create /ren_a with "hello", /ren_b with "world", rename /ren_a to. */
/* /Ren_b, verify /ren_b now contains "hello", /ren_a is ENOENT. */
/* ------------------------------------------------------------------ */
static void
test_rename_over(void)
{
	const char *da = "hello";
	int la = 5;
	const char *db = "world";
	int lb = 5;

	long fd =
	    sys3(SYS_open, (long)"/ren_a", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "rename-over (create a)");
		return;
	}
	sys3(SYS_write, fd, (long)da, la);
	sys1(SYS_close, fd);

	fd = sys3(SYS_open, (long)"/ren_b", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "rename-over (create b)");
		return;
	}
	sys3(SYS_write, fd, (long)db, lb);
	sys1(SYS_close, fd);

	long r = sys2(SYS_rename, (long)"/ren_a", (long)"/ren_b");
	check(r == 0, "rename-over (rename ret)");

	/* /Ren_b should now contain "hello". */
	char buf[64];
	my_memset(buf, 0, sizeof(buf));
	fd = sys2(SYS_open, (long)"/ren_b", O_RDONLY);
	if (fd < 0) {
		check(0, "rename-over (open b)");
		return;
	}
	long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
	sys1(SYS_close, fd);
	check(n == la && my_memcmp(buf, da, la) == 0, "rename-over (data)");

	/* /Ren_a should be gone. */
	fd = sys2(SYS_open, (long)"/ren_a", O_RDONLY);
	check(fd == -ENOENT, "rename-over (a ENOENT)");
	if (fd >= 0)
		sys1(SYS_close, fd);

	/* Cleanup. */
	sys1(SYS_unlink, (long)"/ren_b");
}

/* ------------------------------------------------------------------ */
/* Test 4: rmdir non-empty directory. */
/* ------------------------------------------------------------------ */
static void
test_rmdir_notempty(void)
{
	sys2(SYS_mkdir, (long)"/notempty", 0755);
	long fd = sys3(SYS_open, (long)"/notempty/file",
		       O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0)
		sys1(SYS_close, fd);

	long r = sys1(SYS_rmdir, (long)"/notempty");
	check(r == -ENOTEMPTY, "rmdir-notempty");

	/* Cleanup. */
	sys1(SYS_unlink, (long)"/notempty/file");
	sys1(SYS_rmdir, (long)"/notempty");
}

/* ------------------------------------------------------------------ */
/* Test 5: unlink a directory (should return -EISDIR or -EPERM) */
/* ------------------------------------------------------------------ */
static void
test_unlink_dir(void)
{
	sys2(SYS_mkdir, (long)"/unldir", 0755);
	long r = sys1(SYS_unlink, (long)"/unldir");
	/* Linux returns EISDIR; some implementations return EPERM. */
	check(r == -EISDIR || r == -EPERM, "unlink-dir (EISDIR/EPERM)");

	/* Cleanup. */
	sys1(SYS_rmdir, (long)"/unldir");
}

int
main(void)
{
	msg("=== vfs edge tests ===\n");
	test_unlink_while_open();
	test_hardlink_across_dirs();
	test_rename_over();
	test_rmdir_notempty();
	test_unlink_dir();
	msg("all vfs edge tests passed\n");
	test_done();
	return 0;
}
