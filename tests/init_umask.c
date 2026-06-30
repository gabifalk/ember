/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Raw syscall wrappers (no libc beyond write/reboot in test_common) */
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

#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_stat   4
#define SYS_access 21
#define SYS_umask  95

/* Open flags. */
#define O_WRONLY  1
#define O_CREAT   0100
#define O_TRUNC   01000

/* Access modes. */
#define F_OK  0
#define R_OK  4
#define W_OK  2

/* Errno. */
#define ENOENT 2

/* Struct stat offsets (x86_64 Linux) */
#define STAT_MODE_OFF  24

/* 144-Byte stat buffer (enough for x86_64 struct stat) */
typedef char statbuf_t[144];

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
do_stat(const char *path, void *buf)
{
	return sys2(SYS_stat, (long)path, (long)buf);
}

static long
do_umask(long mask)
{
	return sys1(SYS_umask, mask);
}

static long
do_access(const char *path, long mode)
{
	return sys2(SYS_access, (long)path, mode);
}

static unsigned int
get_st_mode(const void *buf)
{
	return *(const unsigned int *)((const char *)buf + STAT_MODE_OFF);
}

/* Helper: create a file, write one byte, close. */
static long
create_file(const char *path, long mode)
{
	long fd = do_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return fd;
	do_write(fd, "x", 1);
	do_close(fd);
	return 0;
}

/*
 * Test 1: umask returns old -- call umask(0022), then umask(0077),
 * verify second call returns 0022.
 */
static void
test_umask_returns_old(void)
{
	do_umask(0022);
	long old = do_umask(0077);
	check(old == 0022, "umask returns old (0022)");
}

/*
 * Test 2: umask affects creat -- set umask(0022), create file with mode 0666,
 * stat it, verify actual mode is 0644. Then umask(0077), create with 0777,
 * verify 0700.
 */
static void
test_umask_affects_creat(void)
{
	do_umask(0022);
	long r = create_file("/umask_t2a.txt", 0666);
	if (r < 0) {
		check(0, "umask_creat (open1)");
		return;
	}

	statbuf_t st;
	r = do_stat("/umask_t2a.txt", &st);
	if (r < 0) {
		check(0, "umask_creat (stat1)");
		return;
	}
	unsigned int mode = get_st_mode(&st) & 0777;
	check(mode == 0644, "umask 0022: 0666 -> 0644");

	do_umask(0077);
	r = create_file("/umask_t2b.txt", 0777);
	if (r < 0) {
		check(0, "umask_creat (open2)");
		return;
	}

	r = do_stat("/umask_t2b.txt", &st);
	if (r < 0) {
		check(0, "umask_creat (stat2)");
		return;
	}
	mode = get_st_mode(&st) & 0777;
	check(mode == 0700, "umask 0077: 0777 -> 0700");
}

/*
 * Test 3: access F_OK -- create a file, verify access returns 0.
 * Verify access("/nonexistent", F_OK) returns -ENOENT.
 */
static void
test_access_f_ok(void)
{
	do_umask(0);
	long r = create_file("/access_t3.txt", 0644);
	if (r < 0) {
		check(0, "access_f_ok (create)");
		return;
	}

	r = do_access("/access_t3.txt", F_OK);
	check(r == 0, "access F_OK existing");

	r = do_access("/nonexistent", F_OK);
	check(r == -ENOENT, "access F_OK nonexistent");
}

/*
 * Test 4: access R_OK/W_OK -- create a file with mode 0644,
 * verify access returns 0 for R_OK and W_OK (we're root).
 */
static void
test_access_rw(void)
{
	do_umask(0);
	long r = create_file("/access_t4.txt", 0644);
	if (r < 0) {
		check(0, "access_rw (create)");
		return;
	}

	r = do_access("/access_t4.txt", R_OK);
	check(r == 0, "access R_OK");

	r = do_access("/access_t4.txt", W_OK);
	check(r == 0, "access W_OK (root)");
}

/*
 * Test 5: umask restore -- set to 0, create file with 0777,
 * verify mode is 0777.
 */
static void
test_umask_restore(void)
{
	do_umask(0);
	long r = create_file("/umask_t5.txt", 0777);
	if (r < 0) {
		check(0, "umask_restore (create)");
		return;
	}

	statbuf_t st;
	r = do_stat("/umask_t5.txt", &st);
	if (r < 0) {
		check(0, "umask_restore (stat)");
		return;
	}
	unsigned int mode = get_st_mode(&st) & 0777;
	check(mode == 0777, "umask 0: 0777 -> 0777");
}

int
main(void)
{
	msg("=== umask/access tests ===\n");
	test_umask_returns_old();
	test_umask_affects_creat();
	test_access_f_ok();
	test_access_rw();
	test_umask_restore();
	test_done();
	return 0;
}
