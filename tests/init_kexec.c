/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <fcntl.h>
#include <unistd.h>
#include "test_common.h"

/* Inline syscall helpers. */
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

/* Test 1: open /proc/iomem succeeds. */
static void
test_iomem_open(void)
{
	int fd = open("/proc/iomem", O_RDONLY);
	check(fd >= 0, "open /proc/iomem");
	if (fd >= 0)
		close(fd);
}

/* Test 2: read /proc/iomem returns data containing "System RAM". */
static void
test_iomem_read(void)
{
	int fd = open("/proc/iomem", O_RDONLY);
	if (fd < 0) {
		check(0, "iomem read (open failed)");
		return;
	}

	char buf[4096];
	long n = read(fd, buf, sizeof(buf) - 1);
	check(n > 0, "iomem read returns data");

	if (n > 0) {
		buf[n] = '\0';
		/* Search for "System RAM" in output. */
		int found = 0;
		for (long i = 0; i + 9 < n; i++) {
			if (buf[i] == 'S' && buf[i + 1] == 'y'
			    && buf[i + 2] == 's' && buf[i + 3] == 't'
			    && buf[i + 4] == 'e' && buf[i + 5] == 'm'
			    && buf[i + 6] == ' ' && buf[i + 7] == 'R'
			    && buf[i + 8] == 'A' && buf[i + 9] == 'M') {
				found = 1;
				break;
			}
		}
		check(found, "iomem contains System RAM");
	}
	close(fd);
}

/* Test 3: second read after EOF returns 0. */
static void
test_iomem_eof(void)
{
	int fd = open("/proc/iomem", O_RDONLY);
	if (fd < 0) {
		check(0, "iomem eof (open failed)");
		return;
	}

	char buf[8192];
	/* Read all content. */
	long total = 0;
	long n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		total += n;
	check(total > 0, "iomem read some data");
	/* Next read should return 0 (EOF) */
	n = read(fd, buf, sizeof(buf));
	check(n == 0, "iomem EOF returns 0");
	close(fd);
}

/* Test 4: kexec_file_load with invalid fd returns error. */
static void
test_kexec_bad_fd(void)
{
	/* kexec_file_load(999, -1, 0, NULL, KEXEC_FILE_NO_INITRD) */
	long ret = sys6(320, 999, -1, 0, 0, 0x04, 0);
	check(ret < 0, "kexec_file_load bad fd returns error");
}

/* Test 5: kexec_file_load with unsupported flags returns -EINVAL. */
static void
test_kexec_bad_flags(void)
{
	/* Flags = 0xFF (unsupported bits set) */
	long ret = sys6(320, 0, -1, 0, 0, 0xFF, 0);
	check(ret == -22, "kexec_file_load bad flags returns -EINVAL");
}

int
main(void)
{
	msg("=== kexec tests ===\n");
	test_iomem_open();
	test_iomem_read();
	test_iomem_eof();
	test_kexec_bad_fd();
	test_kexec_bad_flags();
	test_done();
	return 0;
}
