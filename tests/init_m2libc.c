/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

/* Test: fork + exec M2libc-compiled binary. */
static void
test_m2libc_exec(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		check(0, "m2libc-exec");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/m2_hello", 0 };
		execve("/m2_hello", argv, (char *const[]) {
		       0});
		_exit(99);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "m2libc-exec");
}

int
main(void)
{
	msg("=== m2libc tests ===\n");
	test_m2libc_exec();
	test_done();
	return 0;
}
