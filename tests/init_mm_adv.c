/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Advanced memory management tests -- MAP_SHARED, mlock/munlock/msync/mincore
 * stubs, arch_prctl, clock_nanosleep, set_tid_address.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_mmap            9
#define __NR_munmap          11
#define __NR_msync           26
#define __NR_mincore         27
#define __NR_mlock           149
#define __NR_munlock         150
#define __NR_arch_prctl      158
#define __NR_clock_nanosleep 230
#define __NR_set_tid_address 218
#define __NR_getpid          39

/* Mmap / prot constants. */
#define PROT_READ    1
#define PROT_WRITE   2
#define MAP_PRIVATE  0x02
#define MAP_SHARED   0x01
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED   ((long)-1)
#define PAGE_SIZE    4096

/* arch_prctl subcodes. */
#define ARCH_SET_GS  0x1001
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003
#define ARCH_GET_GS  0x1004

/* Clock. */
#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME   1

/* Errno. */
#define EINVAL 22

struct timespec {
	long tv_sec;
	long tv_nsec;
};

/* --- Raw syscall wrappers --- */

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
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3):"rcx", "r11", "memory");
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

/* --- Tests --- */

static void
test_mmap_shared_anon(void)
{
	long addr = sys6(__NR_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	check(addr != MAP_FAILED
	      && addr > 0, "mmap shared anon: returns valid addr");
	if (addr > 0)
		sys2(__NR_munmap, addr, PAGE_SIZE);
}

static void
test_mmap_shared_write(void)
{
	long addr = sys6(__NR_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	check(addr != MAP_FAILED && addr > 0, "mmap shared write: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned char *p = (volatile unsigned char *)addr;
	*p = 0xAB;
	check(*p == 0xAB, "mmap shared write: read back matches");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

static void
test_mlock_stub(void)
{
	long addr = sys6(__NR_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(addr > 0, "mlock stub: mmap ok");
	if (addr <= 0)
		return;

	long r = sys2(__NR_mlock, addr, PAGE_SIZE);
	check(r == 0, "mlock stub: returns 0");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

static void
test_munlock_stub(void)
{
	long addr = sys6(__NR_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(addr > 0, "munlock stub: mmap ok");
	if (addr <= 0)
		return;

	long r = sys2(__NR_munlock, addr, PAGE_SIZE);
	check(r == 0, "munlock stub: returns 0");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

static void
test_msync_stub(void)
{
	long addr = sys6(__NR_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(addr > 0, "msync stub: mmap ok");
	if (addr <= 0)
		return;

	long r = sys3(__NR_msync, addr, PAGE_SIZE, 0);
	check(r == 0, "msync stub: returns 0");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

static void
test_mincore_resident(void)
{
	long addr = sys6(__NR_mmap, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(addr > 0, "mincore resident: mmap ok");
	if (addr <= 0)
		return;

	/* Touch the page so it's resident. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	*p = 1;

	unsigned char vec[1] = { 0 };
	long r = sys3(__NR_mincore, addr, PAGE_SIZE, (long)vec);
	check(r == 0, "mincore resident: returns 0");
	check((vec[0] & 1) != 0,
	      "mincore resident: page is resident (bit 0 set)");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

static void
test_arch_prctl_fs(void)
{
	/* Save original FS_BASE (libc may use TLS via FS segment) */
	unsigned long orig_fs = 0;
	sys2(__NR_arch_prctl, ARCH_GET_FS, (long)&orig_fs);

	/* Set, get, then restore before calling check() (which uses libc write) */
	long r_set = sys2(__NR_arch_prctl, ARCH_SET_FS, 0x12345678);
	unsigned long val = 0;
	long r_get = sys2(__NR_arch_prctl, ARCH_GET_FS, (long)&val);

	/* Restore FS_BASE before any libc calls. */
	sys2(__NR_arch_prctl, ARCH_SET_FS, orig_fs);

	check(r_set == 0, "arch_prctl fs: SET_FS returns 0");
	check(r_get == 0, "arch_prctl fs: GET_FS returns 0");
	check(val == 0x12345678, "arch_prctl fs: GET_FS value matches");
}

static void
test_arch_prctl_gs_not_impl(void)
{
	long r = sys2(__NR_arch_prctl, ARCH_SET_GS, 0);
	check(r == -EINVAL, "arch_prctl gs: SET_GS returns -EINVAL");

	unsigned long val = 0;
	r = sys2(__NR_arch_prctl, ARCH_GET_GS, (long)&val);
	check(r == -EINVAL, "arch_prctl gs: GET_GS returns -EINVAL");
}

static void
test_clock_nanosleep_relative(void)
{
	struct timespec ts = { 0, 1000000 };	/* 1Ms. */
	long r = sys4(__NR_clock_nanosleep, CLOCK_MONOTONIC, 0, (long)&ts, 0);
	check(r == 0, "clock_nanosleep relative: returns 0");
}

static void
test_clock_nanosleep_abstime_past(void)
{
	struct timespec ts = { 0, 0 };	/* Time 0 = in the past. */
	long r =
	    sys4(__NR_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME,
		 (long)&ts, 0);
	check(r == 0, "clock_nanosleep abstime past: returns 0");
}

static void
test_set_tid_address(void)
{
	long r = sys1(__NR_set_tid_address, 0);
	long pid = sys0(__NR_getpid);
	check(r == pid, "set_tid_address: returns current pid");
}

int
main(void)
{
	msg("=== mm_adv tests ===\n");

	test_mmap_shared_anon();
	test_mmap_shared_write();
	test_mlock_stub();
	test_munlock_stub();
	test_msync_stub();
	test_mincore_resident();
	test_arch_prctl_fs();
	test_arch_prctl_gs_not_impl();
	test_clock_nanosleep_relative();
	test_clock_nanosleep_abstime_past();
	test_set_tid_address();

	test_done();
	return 0;
}
