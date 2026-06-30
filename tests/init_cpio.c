/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test_common.h"

/* Helper: strcmp (avoid pulling in string.h) */
static int
streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* Test 1: basic write syscall. */
static void
test_write(void)
{
	long r = write(1, "    (stdout works)\n", 19);
	check(r == 19, "write");
}

/* Test: stat("/init") returns 0 with reasonable st_size. */
static void
test_stat(void)
{
	/* Struct stat is 144 bytes on x86_64; st_size is at offset 48. */
	char stbuf[144];
	for (int i = 0; i < 144; i++)
		stbuf[i] = 0;
	long r;
	__asm__ volatile ("syscall":"=a" (r)
			  :"a"(4), "D"("/init"), "S"(stbuf)
			  :"rcx", "r11", "memory");
	long st_size = *(long *)(stbuf + 48);
	check(r == 0 && st_size > 0, "stat");
}

/* Test: getcwd() returns "/". */
static void
test_getcwd(void)
{
	char buf[256];
	for (int i = 0; i < 256; i++)
		buf[i] = 0;
	long r;
	__asm__ volatile ("syscall":"=a" (r)
			  :"a"(79), "D"(buf), "S"(256)
			  :"rcx", "r11", "memory");
	check(r > 0 && buf[0] == '/' && buf[1] == '\0', "getcwd");
}

/* Test: getdents64 on "/" finds "init" entry. */
static void
test_getdents(void)
{
	int fd = open("/", O_RDONLY);
	if (fd < 0) {
		check(0, "getdents (open)");
		return;
	}
	char buf[1024];
	long n;
	__asm__ volatile ("syscall":"=a" (n)
			  :"a"(217), "D"(fd), "S"(buf), "d"(1024)
			  :"rcx", "r11", "memory");
	close(fd);

	if (n <= 0) {
		check(0, "getdents");
		return;
	}
	int found = 0;
	long off = 0;
	while (off < n) {
		unsigned short reclen = *(unsigned short *)(buf + off + 16);
		char *dname = buf + off + 19;
		if (streq(dname, "init"))
			found = 1;
		off += reclen;
	}
	check(found, "getdents");
}

/* Test: access("/init", F_OK) == 0, access("/nonexistent", F_OK) == -ENOENT. */
static void
test_access(void)
{
	long r1, r2;
	__asm__ volatile ("syscall":"=a" (r1)
			  :"a"(21), "D"("/init"), "S"(0)	/* F_OK=0. */
			  :"rcx", "r11", "memory");
	__asm__ volatile ("syscall":"=a" (r2)
			  :"a"(21), "D"("/nonexistent"), "S"(0)
			  :"rcx", "r11", "memory");
	check(r1 == 0 && r2 == -2 /* -ENOENT. */ , "access");
}

/* Test: chdir("/") returns 0. */
static void
test_chdir(void)
{
	long r;
	__asm__ volatile ("syscall":"=a" (r)
			  :"a"(80), "D"("/")
			  :"rcx", "r11", "memory");
	check(r == 0, "chdir");
}

/* Test 2: fork + wait. */
static void
test_fork(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		check(0, "fork");
		return;
	}
	if (pid == 0) {
		/* Child. */
		_exit(42);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	/* WEXITSTATUS. */
	int code = (status >> 8) & 0xff;
	check(code == 42, "fork+wait");
}

/* Test 3: fork + exec /hello. */
static void
test_exec(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		check(0, "exec");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello", 0 };
		execve("/hello", argv, (char *const[]) {
		       0});
		_exit(99);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "exec");
}

/* Test 4: pipe -- parent reads, child writes. */
static void
test_pipe(void)
{
	int pipefd[2];
	if (pipe(pipefd) < 0) {
		check(0, "pipe");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		check(0, "pipe");
		return;
	}
	if (pid == 0) {
		/* Child: close read end, write "OK", exit. */
		close(pipefd[0]);
		write(pipefd[1], "OK", 2);
		close(pipefd[1]);
		_exit(0);
	}
	/* Parent: close write end, read from pipe. */
	close(pipefd[1]);
	char buf[4] = { 0, 0, 0, 0 };
	long n = read(pipefd[0], buf, sizeof(buf));
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	check(n == 2 && buf[0] == 'O' && buf[1] == 'K', "pipe");
}

int
main(void)
{
	msg("=== cpio tests ===\n");
	test_write();
	test_stat();
	test_getcwd();
	test_getdents();
	test_access();
	test_chdir();
	test_fork();
	test_exec();
	test_pipe();
	test_done();
	return 0;
}
