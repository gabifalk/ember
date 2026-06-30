/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

/* Test 1: exec a static-PIE (ET_DYN) binary. */
static void
test_pie_exec(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		check(0, "pie-exec");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello_pie", 0 };
		execve("/hello_pie", argv, (char *const[]) {
		       0});
		_exit(99);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "pie-exec");
}

/* Test 2: exec a regular static (ET_EXEC) binary for comparison. */
static void
test_static_exec(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		check(0, "static-exec");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello", 0 };
		execve("/hello", argv, (char *const[]) {
		       0});
		_exit(99);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "static-exec");
}

int
main(void)
{
	msg("=== PIE tests ===\n");
	test_pie_exec();
	test_static_exec();
	test_done();
	return 0;
}
