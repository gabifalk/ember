/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Helper: checks which FDs are open after exec.
 * Exit code is a bitmask:
 *   bit 0 (1): FD 3 open (expected: yes, non-CLOEXEC)
 *   bit 1 (2): FD 4 open (expected: no, was CLOEXEC)
 *   bit 2 (4): FD 5 open (expected: yes, dup'd non-CLOEXEC)
 */

#include <unistd.h>
#include <fcntl.h>

int
main(void)
{
	int result = 0;

	if (fcntl(3, F_GETFD) >= 0)
		result |= 1;
	if (fcntl(4, F_GETFD) >= 0)
		result |= 2;
	if (fcntl(5, F_GETFD) >= 0)
		result |= 4;

	write(1, "fd_checker ran\n", 15);
	return result;
}
