/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include "test_common.h"

/* Test 1: open + read /hello.txt from ext2. */
static void
test_open_read(void)
{
	int fd = open("/hello.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "open+read");
		return;
	}
	char buf[256];
	int n = read(fd, buf, sizeof(buf));
	close(fd);
	check(n > 0, "open+read");
}

/* Test 2: create + write + read back. */
static void
test_create_write_read(void)
{
	const char *data = "hello from write\n";
	int dlen = 17;

	int fd = open("/test.txt", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "create+write+read");
		return;
	}
	write(fd, data, dlen);
	close(fd);

	fd = open("/test.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "create+write+read");
		return;
	}
	char buf[256];
	int n = read(fd, buf, sizeof(buf));
	close(fd);
	check(n == dlen && memcmp(buf, data, dlen) == 0, "create+write+read");
}

/* Test 3: fstat. */
static void
test_fstat(void)
{
	int fd = open("/hello.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "fstat");
		return;
	}
	struct stat st;
	int r = fstat(fd, &st);
	close(fd);
	check(r == 0 && st.st_size > 0, "fstat");
}

/* Test 4: lseek. */
static void
test_lseek(void)
{
	int fd = open("/hello.txt", O_RDONLY);
	if (fd < 0) {
		check(0, "lseek");
		return;
	}
	off_t pos = lseek(fd, 6, SEEK_SET);
	char buf[64];
	int n = read(fd, buf, sizeof(buf));
	close(fd);
	/* /Hello.txt contains "hello from ext2\n" -> offset 6 is "from ext2\n". */
	check(pos == 6 && n > 0, "lseek");
}

/* Test 5: mkdir. */
static void
test_mkdir(void)
{
	long r;
	__asm__ volatile ("syscall":"=a" (r):"a"(83), "D"("/testdir"),
			  "S"(0755):"rcx", "r11", "memory");
	if (r != 0) {
		check(0, "mkdir");
		return;
	}
	/* Open the directory. */
	int fd = open("/testdir", O_RDONLY | 0200000 /* O_DIRECTORY. */ );
	if (fd < 0) {
		check(0, "mkdir (open dir)");
		return;
	}
	/* Read entries with getdents64. */
	char buf[512];
	long n;
	__asm__ volatile ("syscall":"=a" (n):"a"(217), "D"(fd), "S"(buf),
			  "d"(512):"rcx", "r11", "memory");
	close(fd);

	/* Verify . and .. are present. */
	int found_dot = 0, found_dotdot = 0;
	long off = 0;
	while (off < n) {
		unsigned short reclen = *(unsigned short *)(buf + off + 16);
		char *dname = buf + off + 19;
		if (dname[0] == '.' && dname[1] == '\0')
			found_dot = 1;
		if (dname[0] == '.' && dname[1] == '.' && dname[2] == '\0')
			found_dotdot = 1;
		off += reclen;
	}
	check(found_dot && found_dotdot, "mkdir");
}

/* Test 6: getdents on root. */
static void
test_getdents(void)
{
	int fd = open("/", O_RDONLY);
	if (fd < 0) {
		check(0, "getdents");
		return;
	}
	char buf[1024];
	long n;
	__asm__ volatile ("syscall":"=a" (n):"a"(217), "D"(fd), "S"(buf),
			  "d"(1024):"rcx", "r11", "memory");
	close(fd);

	if (n <= 0) {
		check(0, "getdents");
		return;
	}
	/* Look for hello.txt in the listing. */
	int found = 0;
	long off = 0;
	while (off < n) {
		unsigned short reclen = *(unsigned short *)(buf + off + 16);
		char *dname = buf + off + 19;
		if (strcmp(dname, "hello.txt") == 0)
			found = 1;
		off += reclen;
	}
	check(found, "getdents");
}

/* Test 7: rmdir. */
static void
test_rmdir(void)
{
	/* Create empty dir, then remove it. */
	long r;
	__asm__ volatile ("syscall":"=a" (r):"a"(83), "D"("/emptydir"),
			  "S"(0755):"rcx", "r11", "memory");
	if (r != 0) {
		check(0, "rmdir (mkdir failed)");
		return;
	}
	__asm__ volatile ("syscall":"=a" (r):"a"(84), "D"("/emptydir"):"rcx",
			  "r11", "memory");
	if (r != 0) {
		check(0, "rmdir");
		return;
	}
	/* Verify it's gone. */
	int fd = open("/emptydir", O_RDONLY);
	if (fd >= 0) {
		close(fd);
		check(0, "rmdir (still exists)");
		return;
	}

	/* Create non-empty dir, verify rmdir fails. */
	__asm__ volatile ("syscall":"=a" (r):"a"(83), "D"("/notempty"),
			  "S"(0755):"rcx", "r11", "memory");
	if (r != 0) {
		check(0, "rmdir (mkdir notempty)");
		return;
	}
	/* Create a file inside. */
	fd = open("/notempty/file.txt", O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) {
		write(fd, "x", 1);
		close(fd);
	}
	__asm__ volatile ("syscall":"=a" (r):"a"(84), "D"("/notempty"):"rcx",
			  "r11", "memory");
	check(r != 0, "rmdir (ENOTEMPTY)");
}

int
main(void)
{
	msg("=== ext2 tests ===\n");
	test_open_read();
	test_create_write_read();
	test_fstat();
	test_lseek();
	test_mkdir();
	test_getdents();
	test_rmdir();
	test_done();
	return 0;
}
