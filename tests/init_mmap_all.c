/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 * Consolidated mmap tests -- adv, edge, fixed, mremap.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_mmap       9
#define __NR_mprotect   10
#define __NR_munmap     11
#define __NR_mremap     25

/* Mmap constants. */
#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FIXED       0x10
#define MAP_FIXED_NOREPLACE 0x100000

/* Mremap flags. */
#define MREMAP_MAYMOVE  1

#define PAGE_SIZE 4096

/* Error codes. */
#define EINVAL 22
#define ENOMEM 12
#define EEXIST 17

/* Raw syscall wrappers. */
static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10),
			  "r"(r8), "r"(r9)
			  :"rcx", "r11", "memory");
	return ret;
}

/* Helper: mmap anonymous RW. */
static long
do_mmap(long addr, long size, int prot, int flags)
{
	return sys6(__NR_mmap, addr, size, prot, flags, -1, 0);
}

/* Helper: mmap anonymous RW at kernel-chosen address. */
static long
mmap_anon(long size)
{
	return do_mmap(0, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS);
}

/* Helper: check if return value is an error (negative errno as unsigned) */
static int
is_error(long ret)
{
	return (unsigned long)ret > 0xfffffffffffff000UL;
}

/* Helper: mremap. */
static long
do_mremap(long old_addr, long old_size, long new_size, long flags)
{
	return sys4(__NR_mremap, old_addr, old_size, new_size, flags);
}

/*
 * ================================================================
 * mmap_adv tests
 * ================================================================
 */

/* ---- Test 1: MAP_ANONYMOUS basic ---- */
static void
test_anon_basic(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0
	      && (addr & 0xfff) == 0, "anon basic: mmap returned valid addr");
	if (addr <= 0)
		return;

	/* Write pattern and read back. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (int i = 0; i < PAGE_SIZE; i++)
		p[i] = (unsigned char)(i & 0xff);

	int ok = 1;
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (p[i] != (unsigned char)(i & 0xff)) {
			ok = 0;
			break;
		}
	}
	check(ok, "anon basic: write pattern read back");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 2: MAP_ANONYMOUS zeroed ---- */
static void
test_anon_zeroed(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "anon zeroed: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned char *p = (volatile unsigned char *)addr;
	int ok = 1;
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (p[i] != 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "anon zeroed: new page is zero-filled");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 3: MAP_FIXED ---- */
static void
test_map_fixed(void)
{
	/* First, get a page at a kernel-chosen address. */
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "map_fixed: initial mmap ok");
	if (addr <= 0)
		return;

	/* Write a pattern. */
	volatile unsigned long *p = (volatile unsigned long *)addr;
	*p = 0xAAAAAAAAAAAAAAAAUL;
	check(*p == 0xAAAAAAAAAAAAAAAAUL, "map_fixed: initial write ok");

	/* MAP_FIXED over the same address. */
	long addr2 = do_mmap(addr, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
	check(addr2 == addr, "map_fixed: MAP_FIXED returned same addr");

	/* New mapping should be zeroed (fresh anonymous page) */
	volatile unsigned long *p2 = (volatile unsigned long *)addr2;
	check(*p2 == 0, "map_fixed: new mapping is zeroed (old data gone)");

	/* Write new data and verify. */
	*p2 = 0xBBBBBBBBBBBBBBBBUL;
	check(*p2 == 0xBBBBBBBBBBBBBBBBUL, "map_fixed: new data visible");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 4: munmap ---- */
static void
test_munmap(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "munmap: mmap ok");
	if (addr <= 0)
		return;

	/* Write non-zero data. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (int i = 0; i < PAGE_SIZE; i++)
		p[i] = 0xCC;

	/* Unmap it. */
	long r = sys2(__NR_munmap, addr, PAGE_SIZE);
	check(r == 0, "munmap: returns 0");

	/* Map again (same size, kernel picks address -- may or may not reuse) */
	long addr2 = mmap_anon(PAGE_SIZE);
	check(addr2 > 0 && (addr2 & 0xfff) == 0, "munmap: re-mmap ok");
	if (addr2 <= 0)
		return;

	/* New mapping must be zeroed regardless of address. */
	volatile unsigned char *p2 = (volatile unsigned char *)addr2;
	int ok = 1;
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (p2[i] != 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "munmap: re-mapped page is zeroed (no stale data)");

	sys2(__NR_munmap, addr2, PAGE_SIZE);
}

/* ---- Test 5: large anonymous (1 MB) ---- */
static void
test_large_anon(void)
{
	long size = 1024 * 1024;	/* 1 MB. */
	long addr = mmap_anon(size);
	check(addr > 0 && (addr & 0xfff) == 0, "large anon: mmap 1MB ok");
	if (addr <= 0)
		return;

	/* Write to first page. */
	volatile unsigned long *first = (volatile unsigned long *)addr;
	*first = 0xDEADBEEFCAFEBABEUL;

	/* Write to last page. */
	volatile unsigned long *last =
	    (volatile unsigned long *)(addr + size - 8);
	*last = 0x0123456789ABCDEFUL;

	/* Verify. */
	int ok = (*first == 0xDEADBEEFCAFEBABEUL) &&
	    (*last == 0x0123456789ABCDEFUL);
	check(ok, "large anon: write first+last page");

	sys2(__NR_munmap, addr, size);
}

/*
 * ================================================================
 * mmap_edge tests
 * ================================================================
 */

/* ---- Test 1: MAP_FIXED overlapping two adjacent pages ---- */
static void
test_map_fixed_overlap(void)
{
	/* Allocate two adjacent pages at a known address. */
	long base = 0x300000000L;
	long addr1 = do_mmap(base, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
	check(addr1 == base, "overlap: first page mmap ok");
	if (is_error(addr1))
		return;

	long addr2 =
	    do_mmap(base + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
	check(addr2 == base + PAGE_SIZE, "overlap: second page mmap ok");
	if (is_error(addr2))
		return;

	/* Write patterns to both pages. */
	volatile unsigned char *p1 = (volatile unsigned char *)base;
	volatile unsigned char *p2 =
	    (volatile unsigned char *)(base + PAGE_SIZE);
	p1[0] = 0xAA;
	p1[PAGE_SIZE - 1] = 0xBB;
	p2[0] = 0xCC;
	p2[PAGE_SIZE - 1] = 0xDD;

	/* MAP_FIXED a single page overlapping the second page. */
	long addr3 =
	    do_mmap(base + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
	check(addr3 == base + PAGE_SIZE, "overlap: replacement mmap ok");

	/* Replacement page should be zeroed. */
	volatile unsigned char *p3 = (volatile unsigned char *)addr3;
	check(p3[0] == 0, "overlap: replacement page is zeroed");

	/* First page should be unaffected. */
	check(p1[0] == 0xAA, "overlap: first page byte 0 intact");
	check(p1[PAGE_SIZE - 1] == 0xBB,
	      "overlap: first page last byte intact");

	sys2(__NR_munmap, base, PAGE_SIZE * 2);
}

/* ---- Test 2: Zero-length mmap returns -EINVAL ---- */
static void
test_zero_length_mmap(void)
{
	long ret = do_mmap(0, 0, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS);
	check(ret == -EINVAL, "zero_len: mmap(0,0,...) returns -EINVAL");
}

/* ---- Test 3: mprotect on unmapped region returns -ENOMEM ---- */
static void
test_mprotect_unmapped(void)
{
	/* Allocate and then unmap a page. */
	long addr = mmap_anon(PAGE_SIZE);
	check(!is_error(addr), "mprotect_unmap: initial mmap ok");
	if (is_error(addr))
		return;

	long r = sys2(__NR_munmap, addr, PAGE_SIZE);
	check(r == 0, "mprotect_unmap: munmap ok");

	/* Mprotect on the now-unmapped address should fail. */
	r = sys3(__NR_mprotect, addr, PAGE_SIZE, PROT_READ);
	check(r == -ENOMEM, "mprotect_unmap: mprotect returns -ENOMEM");
}

/* ---- Test 4: mmap with non-page-aligned size returns page-aligned addr ---- */
static void
test_alignment(void)
{
	long addr = mmap_anon(100);	/* Not page-aligned size. */
	check(!is_error(addr), "align: mmap(100) succeeds");
	if (is_error(addr))
		return;

	check((addr & (PAGE_SIZE - 1)) == 0,
	      "align: returned addr is page-aligned");

	/* Should be able to write to the full page. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	p[0] = 0x11;
	p[PAGE_SIZE - 1] = 0x22;
	check(p[0] == 0x11
	      && p[PAGE_SIZE - 1] == 0x22, "align: full page accessible");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 5: Multiple small mmaps -- VMA table stress ---- */
static void
test_multiple_mmaps(void)
{
#define NUM_REGIONS 32
	long addrs[NUM_REGIONS];
	int all_ok = 1;

	/* Allocate 32 small regions. */
	for (int i = 0; i < NUM_REGIONS; i++) {
		addrs[i] = mmap_anon(PAGE_SIZE);
		if (is_error(addrs[i])) {
			all_ok = 0;
			break;
		}
	}
	check(all_ok, "multi: allocated 32 regions");

	/* Write unique data to each region. */
	for (int i = 0; i < NUM_REGIONS; i++) {
		if (is_error(addrs[i]))
			continue;
		volatile unsigned char *p = (volatile unsigned char *)addrs[i];
		p[0] = (unsigned char)(i & 0xff);
		p[1] = (unsigned char)((i * 7 + 3) & 0xff);
	}

	/* Verify all data. */
	int verify_ok = 1;
	for (int i = 0; i < NUM_REGIONS; i++) {
		if (is_error(addrs[i]))
			continue;
		volatile unsigned char *p = (volatile unsigned char *)addrs[i];
		if (p[0] != (unsigned char)(i & 0xff) ||
		    p[1] != (unsigned char)((i * 7 + 3) & 0xff)) {
			verify_ok = 0;
			break;
		}
	}
	check(verify_ok, "multi: all 32 regions have correct data");

	/* Munmap all. */
	int unmap_ok = 1;
	for (int i = 0; i < NUM_REGIONS; i++) {
		if (is_error(addrs[i]))
			continue;
		long r = sys2(__NR_munmap, addrs[i], PAGE_SIZE);
		if (r != 0)
			unmap_ok = 0;
	}
	check(unmap_ok, "multi: all 32 regions unmapped ok");
#undef NUM_REGIONS
}

/*
 * ================================================================
 * mmap_fixed tests
 * ================================================================
 */

/* ---- Test 1: MAP_FIXED replaces existing mapping ---- */
static void
test_map_fixed_replace(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0
	      && (addr & 0xfff) == 0, "fixed_replace: initial mmap ok");
	if (addr <= 0)
		return;

	/* Write 0xAA to the page. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	*p = 0xAA;
	check(*p == 0xAA, "fixed_replace: initial write ok");

	/* MAP_FIXED at the same address to replace it. */
	long addr2 = do_mmap(addr, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
	check(addr2 == addr, "fixed_replace: MAP_FIXED returned same addr");

	/* New mapping should be zeroed (old data gone) */
	volatile unsigned char *p2 = (volatile unsigned char *)addr2;
	check(*p2 == 0, "fixed_replace: new mapping is zeroed");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 2: MAP_FIXED_NOREPLACE fails on existing mapping ---- */
static void
test_map_fixed_noreplace_fail(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0
	      && (addr & 0xfff) == 0, "noreplace_fail: initial mmap ok");
	if (addr <= 0)
		return;

	/* Write a pattern so we can verify it survives. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	*p = 0xBB;

	/* Try MAP_FIXED_NOREPLACE at the same address -- should fail with -EEXIST. */
	long ret = do_mmap(addr, PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE);
	check(is_error(ret), "noreplace_fail: returns error");
	check(ret == -EEXIST, "noreplace_fail: error is EEXIST");

	/* Original mapping should still be accessible. */
	check(*p == 0xBB, "noreplace_fail: original data intact");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 3: MAP_FIXED_NOREPLACE succeeds on unmapped address ---- */
static void
test_map_fixed_noreplace_success(void)
{
	/* Get a page, note the address, then unmap it. */
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "noreplace_ok: initial mmap ok");
	if (addr <= 0)
		return;

	long saved = addr;
	long r = sys2(__NR_munmap, addr, PAGE_SIZE);
	check(r == 0, "noreplace_ok: munmap ok");

	/* MAP_FIXED_NOREPLACE at the now-free address. */
	long addr2 = do_mmap(saved, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE);
	check(addr2 == saved, "noreplace_ok: got requested addr");
	if (is_error(addr2))
		return;

	/* Write and read back. */
	volatile unsigned char *p = (volatile unsigned char *)addr2;
	*p = 0xCC;
	check(*p == 0xCC, "noreplace_ok: write/read ok");

	sys2(__NR_munmap, addr2, PAGE_SIZE);
}

/* ---- Test 4: MAP_FIXED at a chosen high user address ---- */
static void
test_map_fixed_at_chosen_addr(void)
{
	long target = 0x200000000L;	/* 8 GiB, well within user space. */

	long addr = do_mmap(target, PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
	check(addr == target, "fixed_chosen: got exact addr");
	if (is_error(addr))
		return;

	/* Write and read back. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	*p = 0xDD;
	check(*p == 0xDD, "fixed_chosen: write/read ok");

	/* Verify page is otherwise zeroed. */
	int ok = 1;
	for (int i = 1; i < PAGE_SIZE; i++) {
		if (p[i] != 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "fixed_chosen: rest of page is zeroed");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/*
 * ================================================================
 * mremap tests
 * ================================================================
 */

/* ---- Test 1: grow from 4096 to 8192, verify old data + new zeroed ---- */
static void
test_grow(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "grow: mmap ok");
	if (addr <= 0)
		return;

	/* Fill first page with 0xAA. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (int i = 0; i < PAGE_SIZE; i++)
		p[i] = 0xAA;

	/* Grow to 2 pages. */
	long addr2 = do_mremap(addr, PAGE_SIZE, 2 * PAGE_SIZE, MREMAP_MAYMOVE);
	check(addr2 > 0
	      && (addr2 & 0xfff) == 0, "grow: mremap returned valid addr");
	if (addr2 <= 0)
		return;

	/* Verify original page still has 0xAA. */
	volatile unsigned char *q = (volatile unsigned char *)addr2;
	int ok = 1;
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (q[i] != 0xAA) {
			ok = 0;
			break;
		}
	}
	check(ok, "grow: original data preserved");

	/* Verify new page is zeroed. */
	ok = 1;
	for (int i = PAGE_SIZE; i < 2 * PAGE_SIZE; i++) {
		if (q[i] != 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "grow: new page is zeroed");

	sys2(__NR_munmap, addr2, 2 * PAGE_SIZE);
}

/* ---- Test 2: shrink from 8192 to 4096 ---- */
static void
test_shrink(void)
{
	long addr = mmap_anon(2 * PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "shrink: mmap ok");
	if (addr <= 0)
		return;

	/* Fill both pages with pattern. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (int i = 0; i < 2 * PAGE_SIZE; i++)
		p[i] = (unsigned char)(i & 0xff);

	/* Shrink to 1 page. */
	long addr2 = do_mremap(addr, 2 * PAGE_SIZE, PAGE_SIZE, 0);
	check(addr2 > 0
	      && (addr2 & 0xfff) == 0, "shrink: mremap returned valid addr");
	if (addr2 <= 0)
		return;

	/* Verify remaining page has correct pattern. */
	volatile unsigned char *q = (volatile unsigned char *)addr2;
	int ok = 1;
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (q[i] != (unsigned char)(i & 0xff)) {
			ok = 0;
			break;
		}
	}
	check(ok, "shrink: remaining data preserved");

	sys2(__NR_munmap, addr2, PAGE_SIZE);
}

/* ---- Test 3: large grow with MREMAP_MAYMOVE (16384 to 32768) ---- */
static void
test_large_move(void)
{
	long addr = mmap_anon(4 * PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "large_move: mmap ok");
	if (addr <= 0)
		return;

	/* Write recognizable pattern to all 4 pages. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (int i = 0; i < 4 * PAGE_SIZE; i++)
		p[i] = (unsigned char)((i * 7 + 3) & 0xff);

	/* Grow to 8 pages with MREMAP_MAYMOVE. */
	long addr2 =
	    do_mremap(addr, 4 * PAGE_SIZE, 8 * PAGE_SIZE, MREMAP_MAYMOVE);
	check(addr2 > 0
	      && (addr2 & 0xfff) == 0,
	      "large_move: mremap returned valid addr");
	if (addr2 <= 0)
		return;

	/* Verify original data preserved (even if address changed) */
	volatile unsigned char *q = (volatile unsigned char *)addr2;
	int ok = 1;
	for (int i = 0; i < 4 * PAGE_SIZE; i++) {
		if (q[i] != (unsigned char)((i * 7 + 3) & 0xff)) {
			ok = 0;
			break;
		}
	}
	check(ok, "large_move: data preserved after grow");

	sys2(__NR_munmap, addr2, 8 * PAGE_SIZE);
}

/* ---- Test 4: mremap with new_size=0 should fail ---- */
static void
test_zero_size(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "zero_size: mmap ok");
	if (addr <= 0)
		return;

	long ret = do_mremap(addr, PAGE_SIZE, 0, 0);
	check(ret < 0, "zero_size: mremap new_size=0 fails");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/*
 * ================================================================
 * main
 * ================================================================
 */

int
main(void)
{
	msg("=== mmap_all tests ===\n");

	msg("--- mmap adv ---\n");
	test_anon_basic();
	test_anon_zeroed();
	test_map_fixed();
	test_munmap();
	test_large_anon();

	msg("--- mmap edge ---\n");
	test_map_fixed_overlap();
	test_zero_length_mmap();
	test_mprotect_unmapped();
	test_alignment();
	test_multiple_mmaps();

	msg("--- mmap fixed ---\n");
	test_map_fixed_replace();
	test_map_fixed_noreplace_fail();
	test_map_fixed_noreplace_success();
	test_map_fixed_at_chosen_addr();

	msg("--- mremap ---\n");
	test_grow();
	test_shrink();
	test_large_move();
	test_zero_size();

	test_done();
	return 0;
}
