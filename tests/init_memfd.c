/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include "test_common.h"

static long
raw_syscall2(long nr, long a, long b)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a), "S"(b)
			  :"rcx", "r11", "memory");
	return ret;
}

/* Test 1: memfd_create returns a valid fd. */
static void
test_create(void)
{
	long fd = raw_syscall2(319, (long)"test", 0);
	check(fd >= 0, "memfd_create returns valid fd");
	if (fd >= 0)
		close((int)fd);
}

/* Test 2: memfd_create with bad flags returns -EINVAL. */
static void
test_bad_flags(void)
{
	long fd = raw_syscall2(319, (long)"test", 0xFF);
	check(fd == -22, "memfd_create bad flags returns -EINVAL");
}

/* Test 3: write then read back data. */
static void
test_write_read(void)
{
	long fd = raw_syscall2(319, (long)"buf", 0);
	if (fd < 0) {
		check(0, "memfd write/read (create failed)");
		return;
	}

	const char data[] = "hello memfd";
	long nw = write((int)fd, data, sizeof(data));
	check(nw == (long)sizeof(data), "memfd write returns correct count");

	/* Seek back to start. */
	long pos = lseek((int)fd, 0, 0 /* SEEK_SET. */ );
	check(pos == 0, "memfd lseek to 0");

	char buf[64];
	for (int i = 0; i < 64; i++)
		buf[i] = 0;
	long nr = read((int)fd, buf, sizeof(buf));
	check(nr == (long)sizeof(data), "memfd read returns correct count");

	/* Verify content. */
	int match = 1;
	for (unsigned i = 0; i < sizeof(data); i++) {
		if (buf[i] != data[i]) {
			match = 0;
			break;
		}
	}
	check(match, "memfd read data matches written data");

	close((int)fd);
}

/* Test 4: fstat reports correct size. */
static void
test_fstat_size(void)
{
	long fd = raw_syscall2(319, (long)"sz", 0);
	if (fd < 0) {
		check(0, "memfd fstat (create failed)");
		return;
	}

	/* Write 100 bytes. */
	char data[100];
	for (int i = 0; i < 100; i++)
		data[i] = (char)i;
	write((int)fd, data, 100);

	/* Fstat. */
	struct stat st;
	int r = fstat((int)fd, &st);
	check(r == 0, "memfd fstat succeeds");
	check(st.st_size == 100, "memfd fstat size == 100");

	close((int)fd);
}

/* Test 5: multiple writes grow the buffer. */
static void
test_grow(void)
{
	long fd = raw_syscall2(319, (long)"grow", 0);
	if (fd < 0) {
		check(0, "memfd grow (create failed)");
		return;
	}

	char block[1024];
	for (int i = 0; i < 1024; i++)
		block[i] = (char)(i & 0xFF);

	/* Write 8 * 1024 = 8192 bytes (forces buffer growth past 4096) */
	for (int i = 0; i < 8; i++) {
		long nw = write((int)fd, block, 1024);
		if (nw != 1024) {
			check(0, "memfd grow write");
			close((int)fd);
			return;
		}
	}

	/* Seek back and read first 1024. */
	lseek((int)fd, 0, 0);
	char readback[1024];
	long nr = read((int)fd, readback, 1024);
	check(nr == 1024, "memfd grow read 1024");

	int match = 1;
	for (int i = 0; i < 1024; i++) {
		if (readback[i] != block[i]) {
			match = 0;
			break;
		}
	}
	check(match, "memfd grow first block matches");

	close((int)fd);
}

int
main(void)
{
	msg("=== memfd tests ===\n");
	test_create();
	test_bad_flags();
	test_write_read();
	test_fstat_size();
	test_grow();
	test_done();
	return 0;
}
