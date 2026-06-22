/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/*
 * Freestanding init (no libc) for the VMA-authority redesign.  Built with the
 * native gcc at -nostdlib -no-pie -static so it has a real read-execute .text
 * segment and a separate read-write data segment -- something the monolithic
 * RWX M2-Planet binaries cannot express.  Exercises the paths the M2 tests
 * could not reach:
 *
 *   wx-negative          write to read-only .text must SIGSEGV (W^X enforced)
 *   mprotect-revoke      mprotect(-W) then write must SIGSEGV (no stale COW)
 *   mprotect-reenable    mprotect(-W) then mprotect(+W) then write must succeed
 *   mprotect-cow-isolation
 *                        a page read-only at fork, then mprotect(+W) in the
 *                        child while the frame is still shared, must copy --
 *                        not alias the write back into the parent.  This is the
 *                        exact bug that motivated the redesign.
 *
 * Each subtest forks; the child performs the faulting/COW operation and the
 * parent observes the child's exit status (low 7 bits = terminating signal).
 */

typedef unsigned long u64;

#define SYS_WRITE 1
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_FORK 57
#define SYS_EXIT 60
#define SYS_WAIT4 61

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define SIGSEGV 11

static long
syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 asm("r10") = a4;
	register long r8 asm("r8") = a5;
	register long r9 asm("r9") = a6;
	asm volatile ("syscall"
		      : "=a"(ret)
		      : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
		      "r"(r9)
		      : "rcx", "r11", "memory");
	return ret;
}

static unsigned
slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

static void
puts(const char *s)
{
	syscall6(SYS_WRITE, 1, (long)s, slen(s), 0, 0, 0);
}

static void
sys_exit(int code)
{
	syscall6(SYS_EXIT, code, 0, 0, 0, 0, 0);
}

static long
sys_fork(void)
{
	return syscall6(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

static long
sys_mprotect(void *addr, u64 len, int prot)
{
	return syscall6(SYS_MPROTECT, (long)addr, len, prot, 0, 0, 0);
}

static unsigned char *
map_rw_page(void)
{
	long r = syscall6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r < 0 && r > -4096)
		return 0;
	return (unsigned char *)r;
}

static int
wait_status(long pid)
{
	int status = 0;
	syscall6(SYS_WAIT4, pid, (long)&status, 0, 0, 0, 0);
	return status;
}

static int
killed_by_signal(int status)
{
	return (status & 0x7f) != 0;
}

static int
exited_clean(int status)
{
	return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0;
}

/* A real function so its address lives in the read-execute .text segment. */
__attribute__((noinline, used))
static int
code_canary(void)
{
	return 0x1234;
}

static int
test_wx_negative(void)
{
	long pid = sys_fork();
	if (pid == 0) {
		u64 a = (u64)&code_canary;
		*(volatile unsigned char *)a = 0;	/* write to code */
		sys_exit(0);	/* reached only if W^X is broken */
	}
	if (killed_by_signal(wait_status(pid))) {
		puts("PASS wx-negative\n");
		return 0;
	}
	puts("FAIL wx-negative: code was writable\n");
	return 1;
}

static int
test_mprotect_revoke(void)
{
	unsigned char *p = map_rw_page();
	if (!p) {
		puts("FAIL mprotect-revoke: mmap\n");
		return 1;
	}
	p[0] = 1;
	long pid = sys_fork();
	if (pid == 0) {
		sys_mprotect(p, 4096, PROT_READ);
		p[0] = 2;	/* write to now read-only page */
		sys_exit(0);
	}
	if (killed_by_signal(wait_status(pid))) {
		puts("PASS mprotect-revoke\n");
		return 0;
	}
	puts("FAIL mprotect-revoke: write after -W allowed\n");
	return 1;
}

static int
test_mprotect_reenable(void)
{
	unsigned char *p = map_rw_page();
	if (!p) {
		puts("FAIL mprotect-reenable: mmap\n");
		return 1;
	}
	p[0] = 5;
	long pid = sys_fork();
	if (pid == 0) {
		sys_mprotect(p, 4096, PROT_READ);
		sys_mprotect(p, 4096, PROT_READ | PROT_WRITE);
		p[0] = 7;
		sys_exit(p[0] == 7 ? 0 : 1);
	}
	if (exited_clean(wait_status(pid))) {
		puts("PASS mprotect-reenable\n");
		return 0;
	}
	puts("FAIL mprotect-reenable\n");
	return 1;
}

static int
test_mprotect_cow_isolation(void)
{
	unsigned char *p = map_rw_page();
	if (!p) {
		puts("FAIL mprotect-cow-isolation: mmap\n");
		return 1;
	}
	p[0] = 100;
	sys_mprotect(p, 4096, PROT_READ);	/* read-only before fork */
	long pid = sys_fork();			/* shared read-only frame */
	if (pid == 0) {
		/* +W on a still-shared frame must defer to COW, not alias. */
		sys_mprotect(p, 4096, PROT_READ | PROT_WRITE);
		p[0] = 200;
		sys_exit(0);
	}
	wait_status(pid);
	if (p[0] == 100) {
		puts("PASS mprotect-cow-isolation\n");
		return 0;
	}
	puts("FAIL mprotect-cow-isolation: child write aliased into parent\n");
	return 1;
}

int
main(void)
{
	int fails = 0;
	puts("=== wx/mprotect tests ===\n");
	fails += test_wx_negative();
	fails += test_mprotect_revoke();
	fails += test_mprotect_reenable();
	fails += test_mprotect_cow_isolation();
	if (fails == 0)
		puts("RESULT: all tests passed\n");
	else
		puts("RESULT: FAILED\n");
	return fails;
}

__attribute__((force_align_arg_pointer))
void
_start(void)
{
	sys_exit(main());
}
