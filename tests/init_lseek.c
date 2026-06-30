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

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_lseek  8
#define SYS_pipe2  293

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

#define ESPIPE 29

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
do_read(long fd, void *buf, long count)
{
	return sys3(SYS_read, fd, (long)buf, count);
}

static long
do_write(long fd, const void *buf, long count)
{
	return sys3(SYS_write, fd, (long)buf, count);
}

static long
do_lseek(long fd, long offset, long whence)
{
	return sys3(SYS_lseek, fd, offset, whence);
}

static long
do_pipe2(int *pipefd, long flags)
{
	return sys2(SYS_pipe2, (long)pipefd, flags);
}

/* Test 1: SEEK_SET - write data, seek to 0, read back verifies same data. */
static void
test_seek_set(void)
{
	long fd =
	    do_open("/test_seek_set.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "seek_set (open)");
		return;
	}

	const char *data = "ABCDEFGH";
	long w = do_write(fd, data, 8);
	if (w != 8) {
		do_close(fd);
		check(0, "seek_set (write)");
		return;
	}

	/* Seek back to beginning. */
	long pos = do_lseek(fd, 0, SEEK_SET);
	if (pos != 0) {
		do_close(fd);
		check(0, "seek_set (pos)");
		return;
	}

	/* Read back and verify. */
	char buf[8];
	long n = do_read(fd, buf, 8);
	do_close(fd);

	int ok = (n == 8);
	int i;
	for (i = 0; i < 8 && ok; i++)
		if (buf[i] != data[i])
			ok = 0;
	check(ok, "seek_set");
}

/*
 * Test 2: SEEK_CUR - write data, seek back -3 from current, read 3 bytes,
 * verify they match the last 3 written.
 */
static void
test_seek_cur(void)
{
	long fd =
	    do_open("/test_seek_cur.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "seek_cur (open)");
		return;
	}

	const char *data = "ABCDEFGH";
	long w = do_write(fd, data, 8);
	if (w != 8) {
		do_close(fd);
		check(0, "seek_cur (write)");
		return;
	}

	/* Current position is 8. Seek back -3 to position 5. */
	long pos = do_lseek(fd, -3, SEEK_CUR);
	if (pos != 5) {
		do_close(fd);
		check(0, "seek_cur (pos)");
		return;
	}

	/* Read 3 bytes: should get "FGH" (last 3 written) */
	char buf[3];
	long n = do_read(fd, buf, 3);
	do_close(fd);

	check(n == 3 && buf[0] == 'F' && buf[1] == 'G'
	      && buf[2] == 'H', "seek_cur");
}

/*
 * Test 3: SEEK_END - write known data, seek to end verifies offset equals
 * file size, then seek to (end - 3) and read 3 bytes.
 */
static void
test_seek_end(void)
{
	long fd =
	    do_open("/test_seek_end.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "seek_end (open)");
		return;
	}

	const char *data = "ABCDEFGH";
	do_write(fd, data, 8);

	/* Seek to end: offset should equal file size (8) */
	long pos = do_lseek(fd, 0, SEEK_END);
	if (pos != 8) {
		do_close(fd);
		check(0, "seek_end (size)");
		return;
	}

	/* Seek to (end - 3), i.e. position 5. */
	pos = do_lseek(fd, -3, SEEK_END);
	if (pos != 5) {
		do_close(fd);
		check(0, "seek_end (pos)");
		return;
	}

	/* Read 3 bytes: should get "FGH". */
	char buf[3];
	long n = do_read(fd, buf, 3);
	do_close(fd);

	check(n == 3 && buf[0] == 'F' && buf[1] == 'G'
	      && buf[2] == 'H', "seek_end");
}

/*
 * Test 4: Seek past end + write creates a hole.
 * Seek to offset 100, write "hi", read back at 100 verifies "hi",
 * read at offset 50 verifies zero bytes (hole)
 */
static void
test_sparse(void)
{
	long fd = do_open("/test_sparse.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "sparse (open)");
		return;
	}

	/* Seek to offset 100. */
	long pos = do_lseek(fd, 100, SEEK_SET);
	if (pos != 100) {
		do_close(fd);
		check(0, "sparse (seek)");
		return;
	}

	/* Write "hi" at offset 100. */
	long w = do_write(fd, "hi", 2);
	if (w != 2) {
		do_close(fd);
		check(0, "sparse (write)");
		return;
	}

	/* Read back at offset 100 to verify "hi". */
	pos = do_lseek(fd, 100, SEEK_SET);
	if (pos != 100) {
		do_close(fd);
		check(0, "sparse (seek2)");
		return;
	}

	char buf[2];
	long n = do_read(fd, buf, 2);
	if (n != 2 || buf[0] != 'h' || buf[1] != 'i') {
		do_close(fd);
		check(0, "sparse (readback)");
		return;
	}

	/* Read at offset 50 to verify zero bytes (hole) */
	pos = do_lseek(fd, 50, SEEK_SET);
	if (pos != 50) {
		do_close(fd);
		check(0, "sparse (seek3)");
		return;
	}

	char zbuf[4];
	n = do_read(fd, zbuf, 4);
	do_close(fd);

	int ok = (n == 4);
	int i;
	for (i = 0; i < 4 && ok; i++)
		if (zbuf[i] != 0)
			ok = 0;
	check(ok, "sparse");
}

/* Test 5: lseek on pipe returns -ESPIPE. */
static void
test_lseek_pipe(void)
{
	int pipefd[2];
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "lseek_pipe (pipe2)");
		return;
	}

	long pos = do_lseek(pipefd[0], 0, SEEK_SET);
	do_close(pipefd[0]);
	do_close(pipefd[1]);

	check(pos == -ESPIPE, "lseek_pipe");
}

int
main(void)
{
	msg("=== lseek tests ===\n");
	test_seek_set();
	test_seek_cur();
	test_seek_end();
	test_sparse();
	test_lseek_pipe();
	test_done();
	return 0;
}
