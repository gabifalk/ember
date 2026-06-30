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
			  :"a"(nr), "S"(a2), "D"(a1)
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
#define SYS_fstat  5
#define SYS_mkdir  83
#define SYS_chmod  90

/* Open flags. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_CREAT   0100

/* Stat field constants. */
#define S_IFREG   0100000
#define S_IFDIR   0040000
#define S_IFMT    0170000

/* Struct stat offsets (x86_64 Linux) */
#define STAT_INO_OFF   8
#define STAT_MODE_OFF  24
#define STAT_NLINK_OFF 16
#define STAT_SIZE_OFF  48

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
do_fstat(long fd, void *buf)
{
	return sys2(SYS_fstat, fd, (long)buf);
}

static long
do_mkdir(const char *path, long mode)
{
	return sys2(SYS_mkdir, (long)path, mode);
}

static long
do_chmod(const char *path, long mode)
{
	return sys2(SYS_chmod, (long)path, mode);
}

static long
get_st_size(const void *buf)
{
	return *(const long *)((const char *)buf + STAT_SIZE_OFF);
}

static unsigned int
get_st_mode(const void *buf)
{
	return *(const unsigned int *)((const char *)buf + STAT_MODE_OFF);
}

static long
get_st_nlink(const void *buf)
{
	return *(const long *)((const char *)buf + STAT_NLINK_OFF);
}

static long
get_st_ino(const void *buf)
{
	return *(const long *)((const char *)buf + STAT_INO_OFF);
}

/*
 * Test 1: st_size after write -- create file, write 100 bytes, stat,
 * write 50 more, stat again.
 */
static void
test_size_after_write(void)
{
	long fd = do_open("/stat_sz.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "size_after_write (open)");
		return;
	}

	/* Write 100 bytes. */
	char buf[100];
	int i;
	for (i = 0; i < 100; i++)
		buf[i] = 'A';
	do_write(fd, buf, 100);
	do_close(fd);

	statbuf_t st;
	long r = do_stat("/stat_sz.txt", &st);
	if (r < 0) {
		check(0, "size_after_write (stat1)");
		return;
	}
	long sz1 = get_st_size(&st);
	check(sz1 == 100, "size_after_write (100)");

	/* Append 50 more bytes. */
	fd = do_open("/stat_sz.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "size_after_write (reopen)");
		return;
	}
	/* Seek to end via lseek(8) -- SEEK_END=2. */
	long pos;
	__asm__ volatile ("syscall":"=a" (pos)
			  :"a"(8), "D"(fd), "S"(0), "d"(2)
			  :"rcx", "r11", "memory");
	char buf2[50];
	for (i = 0; i < 50; i++)
		buf2[i] = 'B';
	do_write(fd, buf2, 50);
	do_close(fd);

	r = do_stat("/stat_sz.txt", &st);
	if (r < 0) {
		check(0, "size_after_write (stat2)");
		return;
	}
	long sz2 = get_st_size(&st);
	check(sz2 == 150, "size_after_write (150)");
}

/* Test 2: st_mode for regular file -- create with 0644, verify S_ISREG and perms. */
static void
test_mode_regular(void)
{
	long fd = do_open("/stat_reg.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "mode_regular (open)");
		return;
	}
	do_write(fd, "x", 1);
	do_close(fd);

	statbuf_t st;
	long r = do_stat("/stat_reg.txt", &st);
	if (r < 0) {
		check(0, "mode_regular (stat)");
		return;
	}

	unsigned int mode = get_st_mode(&st);
	check((mode & S_IFMT) == S_IFREG, "mode_regular (S_ISREG)");
	check((mode & 0777) == 0644, "mode_regular (0644)");
}

/* Test 3: st_mode for directory -- mkdir with 0755, verify S_ISDIR. */
static void
test_mode_directory(void)
{
	long r = do_mkdir("/stat_dir", 0755);
	if (r < 0) {
		check(0, "mode_directory (mkdir)");
		return;
	}

	statbuf_t st;
	r = do_stat("/stat_dir", &st);
	if (r < 0) {
		check(0, "mode_directory (stat)");
		return;
	}

	unsigned int mode = get_st_mode(&st);
	check((mode & S_IFMT) == S_IFDIR, "mode_directory (S_ISDIR)");
}

/* Test 4: chmod changes st_mode -- create 0644, chmod to 0755, verify. */
static void
test_chmod_changes(void)
{
	long fd = do_open("/stat_chm.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "chmod_changes (open)");
		return;
	}
	do_write(fd, "y", 1);
	do_close(fd);

	long r = do_chmod("/stat_chm.txt", 0755);
	if (r < 0) {
		check(0, "chmod_changes (chmod)");
		return;
	}

	statbuf_t st;
	r = do_stat("/stat_chm.txt", &st);
	if (r < 0) {
		check(0, "chmod_changes (stat)");
		return;
	}

	unsigned int mode = get_st_mode(&st);
	check((mode & 0777) == 0755, "chmod_changes (0755)");
}

/* Test 5: st_nlink for directory -- mkdir, verify nlink >= 2 (. and ..) */
static void
test_dir_nlink(void)
{
	long r = do_mkdir("/stat_nlk", 0755);
	if (r < 0) {
		check(0, "dir_nlink (mkdir)");
		return;
	}

	statbuf_t st;
	r = do_stat("/stat_nlk", &st);
	if (r < 0) {
		check(0, "dir_nlink (stat)");
		return;
	}

	long nlink = get_st_nlink(&st);
	check(nlink >= 2, "dir_nlink (>= 2)");
}

/*
 * Test 6: fstat matches stat -- open file, fstat, stat same path,
 * verify st_size and st_ino match.
 */
static void
test_fstat_matches_stat(void)
{
	long fd = do_open("/stat_reg.txt", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "fstat_matches (open)");
		return;
	}

	statbuf_t st_f, st_s;
	long r1 = do_fstat(fd, &st_f);
	do_close(fd);
	long r2 = do_stat("/stat_reg.txt", &st_s);

	if (r1 < 0 || r2 < 0) {
		check(0, "fstat_matches (calls)");
		return;
	}

	long sz_f = get_st_size(&st_f);
	long sz_s = get_st_size(&st_s);
	long ino_f = get_st_ino(&st_f);
	long ino_s = get_st_ino(&st_s);

	check(sz_f == sz_s, "fstat_matches (st_size)");
	check(ino_f == ino_s, "fstat_matches (st_ino)");
}

/* Test 7: st_size == 0 for new empty file. */
static void
test_empty_file_size(void)
{
	long fd = do_open("/stat_empty.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "empty_size (open)");
		return;
	}
	do_close(fd);

	statbuf_t st;
	long r = do_stat("/stat_empty.txt", &st);
	if (r < 0) {
		check(0, "empty_size (stat)");
		return;
	}

	long sz = get_st_size(&st);
	check(sz == 0, "empty_size (== 0)");
}

int
main(void)
{
	msg("=== stat tests ===\n");
	test_size_after_write();
	test_mode_regular();
	test_mode_directory();
	test_chmod_changes();
	test_dir_nlink();
	test_fstat_matches_stat();
	test_empty_file_size();
	test_done();
	return 0;
}
