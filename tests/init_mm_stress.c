/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Memory subsystem stress tests: PMM exhaustion/recovery, mremap of COW pages,
 * brk regression, and mmap fragmentation.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_mmap       9
#define __NR_mprotect   10
#define __NR_munmap     11
#define __NR_brk        12
#define __NR_mremap     25
#define __NR_fork       57
#define __NR_exit       60
#define __NR_wait4      61

/* Mmap/mprotect constants. */
#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

/* Mremap flags. */
#define MREMAP_MAYMOVE  1

#define PAGE_SIZE       4096
#define ENOMEM          12
#define EINVAL          22

/* Raw syscall wrappers. */
static long
sys0(long nr)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr):"rcx", "r11", "memory");
	return ret;
}

static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1):"rcx", "r11",
			  "memory");
	return ret;
}

static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2):"rcx",
			  "r11", "memory");
	return ret;
}

static long
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10):"rcx", "r11", "memory");
	return ret;
}

static long
sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10), "r"(r8), "r"(r9):"rcx", "r11",
			  "memory");
	return ret;
}

/* Helpers. */
static long
mmap_anon(long size)
{
	return sys6(__NR_mmap, 0, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static long
do_mremap(long old_addr, long old_size, long new_size, long flags)
{
	return sys4(__NR_mremap, old_addr, old_size, new_size, flags);
}

static long
sys_brk(unsigned long addr)
{
	return sys1(__NR_brk, (long)addr);
}

/* ---- Test 1: PMM exhaustion and recovery ---- */
#define CHUNK_SIZE (1024 * 1024)	/* 1 MB. */
#define MAX_CHUNKS 256

static long chunk_addrs[MAX_CHUNKS];

static void
test_pmm_exhaustion(void)
{
	int count = 0;

	/* Allocate 1MB chunks until failure. */
	for (int i = 0; i < MAX_CHUNKS; i++) {
		long addr = mmap_anon(CHUNK_SIZE);
		if (addr < 0 || (unsigned long)addr >= 0x800000000000UL) {
			break;
		}
		/* Touch the memory to ensure it is actually mapped. */
		volatile unsigned char *p = (volatile unsigned char *)addr;
		*p = 0x42;
		chunk_addrs[count++] = addr;
	}

	check(count > 0, "pmm exhaust: allocated >0 chunks");

	/* Free all chunks. */
	for (int i = 0; i < count; i++) {
		sys2(__NR_munmap, chunk_addrs[i], CHUNK_SIZE);
	}

	/* Try to allocate again -- should succeed after freeing. */
	long addr = mmap_anon(CHUNK_SIZE);
	check(addr > 0 && (unsigned long)addr < 0x800000000000UL,
	      "pmm exhaust: reclaim after free");

	if (addr > 0 && (unsigned long)addr < 0x800000000000UL) {
		/* Verify by writing and reading back. */
		volatile unsigned char *p = (volatile unsigned char *)addr;
		*p = 0xBE;
		check(*p == 0xBE, "pmm exhaust: write/read reclaimed mem");
		sys2(__NR_munmap, addr, CHUNK_SIZE);
	}
}

/* ---- Test 2: mremap grow ---- */
static void
test_mremap_grow(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "mremap grow: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned char *p = (volatile unsigned char *)addr;
	p[0] = 0xAA;

	long addr2 = do_mremap(addr, PAGE_SIZE, 4 * PAGE_SIZE, MREMAP_MAYMOVE);
	check(addr2 > 0 && (addr2 & 0xfff) == 0, "mremap grow: mremap ok");
	if (addr2 <= 0)
		return;

	volatile unsigned char *q = (volatile unsigned char *)addr2;
	check(q[0] == 0xAA, "mremap grow: first byte preserved");

	/* Write to last byte of last page. */
	q[4 * PAGE_SIZE - 1] = 0xBB;
	check(q[4 * PAGE_SIZE - 1] == 0xBB, "mremap grow: write last page");

	sys2(__NR_munmap, addr2, 4 * PAGE_SIZE);
}

/* ---- Test 3: mremap shrink ---- */
static void
test_mremap_shrink(void)
{
	long addr = mmap_anon(4 * PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "mremap shrink: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned char *p = (volatile unsigned char *)addr;
	p[0 * PAGE_SIZE] = 0x10;
	p[1 * PAGE_SIZE] = 0x20;
	p[2 * PAGE_SIZE] = 0x30;
	p[3 * PAGE_SIZE] = 0x40;

	long addr2 = do_mremap(addr, 4 * PAGE_SIZE, 2 * PAGE_SIZE, 0);
	check(addr2 > 0 && (addr2 & 0xfff) == 0, "mremap shrink: mremap ok");
	if (addr2 <= 0)
		return;

	volatile unsigned char *q = (volatile unsigned char *)addr2;
	check(q[0 * PAGE_SIZE] == 0x10, "mremap shrink: page 0 intact");
	check(q[1 * PAGE_SIZE] == 0x20, "mremap shrink: page 1 intact");

	sys2(__NR_munmap, addr2, 2 * PAGE_SIZE);
}

/* ---- Test 4: mremap of COW page ---- */
static void
test_mremap_cow(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "mremap cow: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned char *p = (volatile unsigned char *)addr;
	p[0] = 0xCC;

	long pid = sys0(__NR_fork);
	if (pid == 0) {
		/* Child: write triggers COW. */
		volatile unsigned char *cp = (volatile unsigned char *)addr;
		cp[0] = 0xDD;

		/* Mremap the COW-resolved page. */
		long addr2 =
		    do_mremap(addr, PAGE_SIZE, 2 * PAGE_SIZE, MREMAP_MAYMOVE);
		if (addr2 <= 0) {
			sys1(__NR_exit, 1);
		}

		volatile unsigned char *cq = (volatile unsigned char *)addr2;
		if (cq[0] != 0xDD) {
			sys1(__NR_exit, 2);
		}
		/* Write to second page. */
		cq[PAGE_SIZE] = 0xEE;
		if (cq[PAGE_SIZE] != 0xEE) {
			sys1(__NR_exit, 3);
		}

		sys1(__NR_exit, 0);
	}

	check(pid > 0, "mremap cow: fork ok");
	if (pid <= 0)
		return;

	/* Wait for child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	/* Linux wait status: exit code is in bits 15..8. */
	int exit_code = (status >> 8) & 0xff;
	check(exit_code == 0, "mremap cow: child succeeded");

	/* Parent's page should still have 0xCC. */
	check(p[0] == 0xCC, "mremap cow: parent data intact");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* ---- Test 5: brk down-then-up (zeroed pages) ---- */
static void
test_brk_zero_after_shrink(void)
{
	long base = sys_brk(0);
	check(base > 0, "brk zero: get base ok");
	if (base <= 0)
		return;

	/* Extend by 4 pages. */
	long extended = base + 4 * PAGE_SIZE;
	long r1 = sys_brk(extended);
	check(r1 == extended, "brk zero: extend ok");
	if (r1 != extended)
		return;

	/* Write 0xFF pattern to all 4 pages. */
	volatile unsigned char *p = (volatile unsigned char *)base;
	for (int i = 0; i < 4 * PAGE_SIZE; i++)
		p[i] = 0xFF;

	/* Shrink back to original. */
	long r2 = sys_brk(base);
	check(r2 == base, "brk zero: shrink ok");
	if (r2 != base)
		return;

	/* Extend again by 4 pages. */
	long r3 = sys_brk(extended);
	check(r3 == extended, "brk zero: re-extend ok");
	if (r3 != extended)
		return;

	/* Verify all pages are zeroed. */
	int ok = 1;
	for (int i = 0; i < 4 * PAGE_SIZE; i++) {
		if (p[i] != 0) {
			ok = 0;
			break;
		}
	}
	check(ok, "brk zero: re-extended pages are zeroed");
}

/* ---- Test 6: mmap + munmap fragmentation ---- */
static void
test_mmap_fragmentation(void)
{
	/* Allocate 8 contiguous pages. */
	long addr = mmap_anon(8 * PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "frag: mmap 8 pages ok");
	if (addr <= 0)
		return;

	/* Unmap every other page (pages at index 1, 3, 5, 7) */
	sys2(__NR_munmap, addr + 1 * PAGE_SIZE, PAGE_SIZE);
	sys2(__NR_munmap, addr + 3 * PAGE_SIZE, PAGE_SIZE);
	sys2(__NR_munmap, addr + 5 * PAGE_SIZE, PAGE_SIZE);
	sys2(__NR_munmap, addr + 7 * PAGE_SIZE, PAGE_SIZE);

	/* Try to mmap 2 contiguous pages -- should succeed somewhere. */
	long addr2 = mmap_anon(2 * PAGE_SIZE);
	check(addr2 > 0
	      && (addr2 & 0xfff) == 0, "frag: mmap 2 pages after holes ok");

	/* Cleanup remaining even pages. */
	sys2(__NR_munmap, addr + 0 * PAGE_SIZE, PAGE_SIZE);
	sys2(__NR_munmap, addr + 2 * PAGE_SIZE, PAGE_SIZE);
	sys2(__NR_munmap, addr + 4 * PAGE_SIZE, PAGE_SIZE);
	sys2(__NR_munmap, addr + 6 * PAGE_SIZE, PAGE_SIZE);
	if (addr2 > 0)
		sys2(__NR_munmap, addr2, 2 * PAGE_SIZE);
}

int
main(void)
{
	msg("=== mm stress tests ===\n");

	test_pmm_exhaustion();
	test_mremap_grow();
	test_mremap_shrink();
	test_mremap_cow();
	test_brk_zero_after_shrink();
	test_mmap_fragmentation();

	test_done();
	return 0;
}
