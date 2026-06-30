/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <unistd.h>
#include <fcntl.h>
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

static long
sys6(long nr, long a, long b, long c, long d, long e, long f_arg)
{
	long ret;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f_arg;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
			  "r"(r9)
			  :"rcx", "r11", "memory");
	return ret;
}

/* Test 1: kexec_file_load from a regular file fd (baseline) */
static void
test_kexec_from_file(void)
{
	int fd = open("/init", O_RDONLY);
	if (fd < 0) {
		check(0, "kexec from file (open failed)");
		return;
	}

	/* kexec_file_load(kernel_fd, initrd_fd=-1, cmdline_len=0, cmdline=NULL, flags=KEXEC_FILE_NO_INITRD) */
	long ret = sys6(320, fd, -1, 0, 0, 0x04, 0);
	check(ret == 0, "kexec_file_load from file fd succeeds");
	close(fd);
}

/* Test 2: kexec_file_load from a memfd (the key integration test) */
static void
test_kexec_from_memfd(void)
{
	/* Read /init into memory. */
	int src = open("/init", O_RDONLY);
	if (src < 0) {
		check(0, "kexec from memfd (open /init failed)");
		return;
	}

	/* Create memfd. */
	long mfd = raw_syscall2(319, (long)"kernel", 0);
	if (mfd < 0) {
		check(0, "kexec from memfd (memfd_create failed)");
		close(src);
		return;
	}

	/* Copy /init into memfd. */
	char buf[4096];
	long n;
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		long nw = write((int)mfd, buf, (unsigned long)n);
		if (nw != n) {
			check(0, "kexec from memfd (write failed)");
			close(src);
			close((int)mfd);
			return;
		}
	}
	close(src);

	/* Rewind memfd. */
	lseek((int)mfd, 0, 0);

	/* kexec_file_load with memfd as kernel. */
	long ret = sys6(320, mfd, -1, 0, 0, 0x04, 0);
	check(ret == 0, "kexec_file_load from memfd succeeds");
	close((int)mfd);
}

int
main(void)
{
	msg("=== kexec-memfd tests ===\n");
	test_kexec_from_file();
	test_kexec_from_memfd();
	test_done();
	return 0;
}
