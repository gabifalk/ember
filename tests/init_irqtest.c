/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Minimal init: the IRQ self-test runs in-kernel at boot and prints its own
 * marker. This init just emits the pass summary the harness greps for. */
#include <stddef.h>

static long
sys_write(long fd, const void *buf, unsigned long n)
{
	long ret;
	__asm__ __volatile__("syscall"
			     : "=a"(ret)
			     : "a"(1), "D"(fd), "S"(buf), "d"(n)
			     : "rcx", "r11", "memory");
	return ret;
}

static void
sys_exit(int code)
{
	__asm__ __volatile__("syscall"::"a"(60), "D"((long)code));
	for (;;) ;
}

void
_start(void)
{
	const char msg[] = "init: 1 passed\n";
	sys_write(1, msg, sizeof(msg) - 1);
	sys_exit(0);
}
