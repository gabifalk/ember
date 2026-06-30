/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <unistd.h>
#include <sys/reboot.h>
#include "test_common.h"

int test_failures = 0;
int test_count = 0;

void
msg(const char *s)
{
	int len = 0;
	while (s[len])
		len++;
	write(1, s, len);
}

void
check(int ok, const char *name)
{
	test_count++;
	if (ok) {
		msg("  PASS ");
	} else {
		msg("  FAIL ");
		test_failures++;
	}
	msg(name);
	msg("\n");
}

void
print_int(int n)
{
	char buf[16];
	int i = 0;
	if (n == 0) {
		write(1, "0", 1);
		return;
	}
	if (n < 0) {
		write(1, "-", 1);
		n = -n;
	}
	while (n > 0) {
		buf[i++] = '0' + (n % 10);
		n /= 10;
	}
	while (i > 0)
		write(1, &buf[--i], 1);
}

void
check_errno(long result, long expected_neg_errno, const char *name)
{
	test_count++;
	if (result == expected_neg_errno) {
		msg("  PASS ");
	} else {
		msg("  FAIL ");
		test_failures++;
		msg("(got ");
		print_int((int)result);
		msg(" expected ");
		print_int((int)expected_neg_errno);
		msg(") ");
	}
	msg(name);
	msg("\n");
}

void
test_done(void)
{
	msg("--- ");
	print_int(test_count - test_failures);
	msg("/");
	print_int(test_count);
	msg(" passed ---\n");
	reboot(RB_POWER_OFF);
	_exit(test_failures);
}
