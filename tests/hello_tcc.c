/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
static long
my_write(int fd, const void *buf, long len)
{
	long ret;
	__asm__ __volatile__("syscall":"=a"(ret)
			     :"a"(1), "D"(fd), "S"(buf), "d"(len)
			     :"rcx", "r11", "memory");
	return ret;
}

static void
my_exit(int code)
{
	__asm__ __volatile__("syscall"::"a"(60), "D"(code):"rcx", "r11");
	for (;;) ;
}

void
_start(void)
{
	my_write(1, "hello from tcc!\n", 16);
	my_exit(0);
}
