/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Brk syscall wrapper: returns the new/current program break. */
static long
sys_brk(unsigned long addr)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(12), "D"(addr)
			  :"rcx", "r11", "memory");
	return ret;
}

/* Test 1: brk(0) returns current break -- non-zero, in user space. */
static void
test_get_current_brk(void)
{
	long brk = sys_brk(0);
	/* Must be non-zero and in user-space (below 0x800000000000) */
	int ok = (brk > 0) && ((unsigned long)brk < 0x800000000000UL);
	check(ok, "brk(0) returns user-space address");
}

/* Test 2: grow brk by one page. */
static void
test_grow_one_page(void)
{
	long base = sys_brk(0);
	long target = base + 4096;
	long result = sys_brk(target);
	check(result == target, "grow brk by 4096");
}

/* Test 3: write to the newly allocated page and read back. */
static void
test_write_read(void)
{
	long base = sys_brk(0);
	long target = base + 4096;
	long result = sys_brk(target);
	if (result != target) {
		check(0, "write/read (grow failed)");
		return;
	}
	/* Write a pattern to the new page. */
	volatile unsigned char *p = (volatile unsigned char *)base;
	for (int i = 0; i < 4096; i++)
		p[i] = (unsigned char)(i & 0xff);
	/* Read back and verify. */
	int ok = 1;
	for (int i = 0; i < 4096; i++) {
		if (p[i] != (unsigned char)(i & 0xff)) {
			ok = 0;
			break;
		}
	}
	check(ok, "write/read new brk page");
}

/* Test 4: grow by 4 pages, write pattern to all, verify. */
static void
test_grow_multiple_pages(void)
{
	long base = sys_brk(0);
	long target = base + 16384;
	long result = sys_brk(target);
	if (result != target) {
		check(0, "grow 4 pages (brk failed)");
		return;
	}
	volatile unsigned char *p = (volatile unsigned char *)base;
	/* Write a pattern across all 4 pages. */
	for (int i = 0; i < 16384; i++)
		p[i] = (unsigned char)((i * 7 + 3) & 0xff);
	/* Verify. */
	int ok = 1;
	for (int i = 0; i < 16384; i++) {
		if (p[i] != (unsigned char)((i * 7 + 3) & 0xff)) {
			ok = 0;
			break;
		}
	}
	check(ok, "grow 4 pages write/verify");
}

/* Test 5: shrink brk, grow again, verify new pages are zeroed. */
static void
test_shrink_and_realloc_zeroed(void)
{
	/* Grow by 2 pages. */
	long base = sys_brk(0);
	long mid = base + 8192;
	long r1 = sys_brk(mid);
	if (r1 != mid) {
		check(0, "shrink/zero (initial grow failed)");
		return;
	}
	/* Write non-zero data to the second page. */
	volatile unsigned char *p = (volatile unsigned char *)(base + 4096);
	for (int i = 0; i < 4096; i++)
		p[i] = 0xAA;
	/* Shrink back to base. */
	long r2 = sys_brk(base);
	if (r2 != base) {
		check(0, "shrink/zero (shrink failed)");
		return;
	}
	/* Grow again by 2 pages. */
	long r3 = sys_brk(base + 8192);
	if (r3 != base + 8192) {
		check(0, "shrink/zero (regrow failed)");
		return;
	}
	/* Verify the second page is zeroed (security: kernel must zero pages) */
	int ok = 1;
	for (int i = 0; i < 4096; i++) {
		if (p[i] != 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "shrink+regrow pages zeroed");
}

/* Test 6: large grow (1 MB), write to first and last page. */
static void
test_large_grow(void)
{
	long base = sys_brk(0);
	long target = base + (1024 * 1024);	/* 1 MB. */
	long result = sys_brk(target);
	if (result != target) {
		check(0, "large grow 1MB (brk failed)");
		return;
	}
	/* Write to first page. */
	volatile unsigned long *first = (volatile unsigned long *)base;
	*first = 0xDEADBEEFCAFEBABEUL;
	/* Write to last page. */
	volatile unsigned long *last = (volatile unsigned long *)(target - 8);
	*last = 0x0123456789ABCDEFUL;
	/* Verify. */
	int ok = (*first == 0xDEADBEEFCAFEBABEUL) &&
	    (*last == 0x0123456789ABCDEFUL);
	check(ok, "large grow 1MB write first+last");
}

int
main(void)
{
	msg("=== brk tests ===\n");
	test_get_current_brk();
	test_grow_one_page();
	test_write_read();
	test_grow_multiple_pages();
	test_shrink_and_realloc_zeroed();
	test_large_grow();
	test_done();
	return 0;
}
