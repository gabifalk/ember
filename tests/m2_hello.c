/* SPDX-License-Identifier: GPL-2.0-or-later. */
/* Copyright (C) 2026 Gabi Falk. */
/*
 * M2-Planet C subset test program for Ember kernel.
 * Compiled with M2-Planet toolchain (not gcc).
 * Exercises M2libc syscalls: write (via putchar) and exit.
 */
#include <stdio.h>

int
main()
{
	putchar(72);		/* H. */
	putchar(101);		/* E. */
	putchar(108);		/* L. */
	putchar(108);		/* L. */
	putchar(111);		/* O. */
	putchar(32);		/*  */
	putchar(102);		/* F. */
	putchar(114);		/* R. */
	putchar(111);		/* O. */
	putchar(109);		/* M. */
	putchar(32);		/*  */
	putchar(77);		/* M. */
	putchar(50);		/* 2. */
	putchar(108);		/* L. */
	putchar(105);		/* I. */
	putchar(98);		/* B. */
	putchar(99);		/* C. */
	putchar(33);		/* ! */
	putchar(10);		/* \N. */

	return 0;
}
