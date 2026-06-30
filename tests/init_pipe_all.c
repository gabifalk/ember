/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 * Consolidated pipe tests -- basic, full, edge.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read            0
#define __NR_write           1
#define __NR_close           3
#define __NR_pipe            22
#define __NR_dup2            33
#define __NR_nanosleep       35
#define __NR_fork            57
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_rt_sigaction    13
#define __NR_rt_sigreturn    15
#define __NR_pipe2           293

/* Aliases used by pipe_full tests. */
#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_CLOSE  3
#define SYS_PIPE2  293

/* Signal numbers. */
#define SIGPIPE 13

/* SA_RESTORER flag. */
#define SA_RESTORER 0x04000000

/* Error codes. */
#define EPIPE  32
#define EAGAIN 11

/* Flags. */
#define O_NONBLOCK 0x800

/* Chunk size for large transfer test. */
#define CHUNK_SIZE 1024
#define TOTAL_SIZE (64 * 1024 + 512)	/* 64.5 KB, exceeds pipe buffer. */

/* Ember pipe buffer size (from include/ember/pipe.h) */
#define PIPE_BUF_SIZE 65536

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

/* kernel_sigaction for x86_64. */
struct kernel_sigaction {
	void (*sa_handler) (int);
	unsigned long sa_flags;
	void (*sa_restorer) (void);
	unsigned long sa_mask;
};

__asm__(".type my_restorer, @function\n"
	"my_restorer:\n" "    mov $15, %rax\n" "    syscall\n");
extern void my_restorer(void);

/* Helpers. */
static long
do_pipe(int pipefd[2])
{
	return sys1(__NR_pipe, (long)pipefd);
}

static long
do_pipe2(int pipefd[2], int flags)
{
	return sys2(__NR_pipe2, (long)pipefd, flags);
}

static long
do_read(int fd, void *buf, long count)
{
	return sys3(__NR_read, fd, (long)buf, count);
}

static long
do_write(int fd, const void *buf, long count)
{
	return sys3(__NR_write, fd, (long)buf, count);
}

static long
do_close(int fd)
{
	return sys1(__NR_close, fd);
}

static long
do_fork(void)
{
	return sys0(__NR_fork);
}

static long
do_exit(int code)
{
	return sys1(__NR_exit, code);
}

static long
do_wait4(long pid, int *status, int options)
{
	return sys4(__NR_wait4, pid, (long)status, options, 0);
}

static long
do_dup2(int oldfd, int newfd)
{
	return sys2(__NR_dup2, oldfd, newfd);
}

static void
tiny_sleep(void)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, 10000000};		/* 10Ms. */
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/*
 * ========================================================================
 * pipe basic tests (from init_pipe.c)
 * ========================================================================
 */

/*
 * ---- Test 1: Large transfer through pipe ----
 * Write 64KB+ through a pipe in chunks, read it back, verify all bytes.
 */
static void
test_large_transfer(void)
{
	int pipefd[2];
	if (do_pipe(pipefd) < 0) {
		check(0, "large: pipe create");
		return;
	}

	long pid = do_fork();
	if (pid < 0) {
		check(0, "large: fork");
		return;
	}

	if (pid == 0) {
		/* Child: write TOTAL_SIZE bytes in chunks. */
		do_close(pipefd[0]);
		unsigned char val = 0;
		long remaining = TOTAL_SIZE;
		unsigned char wbuf[CHUNK_SIZE];
		while (remaining > 0) {
			long chunk =
			    remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
			for (long i = 0; i < chunk; i++)
				wbuf[i] = (unsigned char)((val + i) & 0xff);
			long off = 0;
			while (off < chunk) {
				long w =
				    do_write(pipefd[1], wbuf + off,
					     chunk - off);
				if (w <= 0)
					do_exit(1);
				off += w;
			}
			val = (unsigned char)((val + chunk) & 0xff);
			remaining -= chunk;
		}
		do_close(pipefd[1]);
		do_exit(0);
	}
	/* Parent: read all bytes and verify. */
	do_close(pipefd[1]);
	unsigned char expected = 0;
	long total_read = 0;
	int data_ok = 1;
	unsigned char rbuf[CHUNK_SIZE];

	while (total_read < TOTAL_SIZE) {
		long n = do_read(pipefd[0], rbuf, CHUNK_SIZE);
		if (n <= 0)
			break;
		for (long i = 0; i < n; i++) {
			if (rbuf[i] != (unsigned char)((expected + i) & 0xff))
				data_ok = 0;
		}
		expected = (unsigned char)((expected + n) & 0xff);
		total_read += n;
	}
	do_close(pipefd[0]);

	int status = 0;
	do_wait4(pid, &status, 0);
	int child_ok = ((status >> 8) & 0xff) == 0;

	check(total_read == TOTAL_SIZE, "large: byte count");
	check(data_ok, "large: data integrity");
	check(child_ok, "large: child exit");
}

/* ---- Test 2: SIGPIPE on closed read end ---- */
static volatile int sigpipe_count;

static void
sigpipe_handler(int sig)
{
	(void)sig;
	sigpipe_count++;
}

static void
test_sigpipe(void)
{
	/* Install SIGPIPE handler. */
	struct kernel_sigaction sa;
	for (unsigned long i = 0; i < sizeof(sa); i++)
		((char *)&sa)[i] = 0;
	sa.sa_handler = sigpipe_handler;
	sa.sa_flags = SA_RESTORER;
	sa.sa_restorer = my_restorer;

	long r = sys4(__NR_rt_sigaction, SIGPIPE, (long)&sa, 0, 8);
	check(r == 0, "sigpipe: sigaction install");

	int pipefd[2];
	if (do_pipe(pipefd) < 0) {
		check(0, "sigpipe: pipe create");
		return;
	}
	/* Close read end. */
	do_close(pipefd[0]);

	/* Write to pipe with no readers -- should get SIGPIPE and -EPIPE. */
	sigpipe_count = 0;
	char buf[4] = "test";
	long w = do_write(pipefd[1], buf, 4);

	check(sigpipe_count == 1, "sigpipe: handler ran");
	check(w == -EPIPE, "sigpipe: write returned -EPIPE");

	do_close(pipefd[1]);
}

/*
 * ---- Test 3: Pipe buffer full/drain ----
 * Fill pipe buffer completely, then drain and verify all data.
 */
static void
test_fill_drain(void)
{
	int pipefd[2];
	if (do_pipe(pipefd) < 0) {
		check(0, "fill: pipe create");
		return;
	}
	/* Fork: child fills, parent drains. */
	long pid = do_fork();
	if (pid < 0) {
		check(0, "fill: fork");
		return;
	}

	if (pid == 0) {
		/* Child: write exactly 64KB (pipe buffer size) in small chunks. */
		do_close(pipefd[0]);
		unsigned char wbuf[256];
		long total_written = 0;
		long target = 65536;	/* PIPE_BUF_SIZE. */
		while (total_written < target) {
			long chunk = target - total_written;
			if (chunk > 256)
				chunk = 256;
			for (long i = 0; i < chunk; i++)
				wbuf[i] =
				    (unsigned char)((total_written + i) & 0xff);
			long w = do_write(pipefd[1], wbuf, chunk);
			if (w <= 0)
				do_exit(1);
			total_written += w;
		}
		do_close(pipefd[1]);
		do_exit(0);
	}
	/* Parent: small sleep to let child fill buffer, then drain. */
	do_close(pipefd[1]);
	tiny_sleep();

	unsigned char rbuf[256];
	long total_read = 0;
	int data_ok = 1;

	while (1) {
		long n = do_read(pipefd[0], rbuf, 256);
		if (n <= 0)
			break;
		for (long i = 0; i < n; i++) {
			if (rbuf[i] != (unsigned char)((total_read + i) & 0xff))
				data_ok = 0;
		}
		total_read += n;
	}
	do_close(pipefd[0]);

	int status = 0;
	do_wait4(pid, &status, 0);

	check(total_read == 65536, "fill: drained all 64KB");
	check(data_ok, "fill: data integrity");
}

/*
 * ---- Test 4: Close-while-blocked-read ----
 * Fork, child closes write end after delay, parent reads until EOF (returns 0).
 */
static void
test_close_eof(void)
{
	int pipefd[2];
	if (do_pipe(pipefd) < 0) {
		check(0, "eof: pipe create");
		return;
	}

	long pid = do_fork();
	if (pid < 0) {
		check(0, "eof: fork");
		return;
	}

	if (pid == 0) {
		/* Child: close read end, sleep briefly, then close write end. */
		do_close(pipefd[0]);
		tiny_sleep();
		do_close(pipefd[1]);
		do_exit(0);
	}
	/* Parent: close write end, read should block then return 0 (EOF) */
	do_close(pipefd[1]);
	char buf[16];
	long n = do_read(pipefd[0], buf, sizeof(buf));
	do_close(pipefd[0]);

	int status = 0;
	do_wait4(pid, &status, 0);

	check(n == 0, "eof: read returned 0 (EOF)");
}

/*
 * ---- Test 5: dup2 pipe to stdout ----
 * dup2 pipe write end to fd 1, write via fd 1, read from pipe read end.
 */
static void
test_dup2_pipe(void)
{
	int pipefd[2];
	if (do_pipe(pipefd) < 0) {
		check(0, "dup2: pipe create");
		return;
	}
	/*
	 * Save original stdout by dup'ing it (use dup2 to a high fd)
	 * We'll just fork and do the dup2 in the child to avoid clobbering
	 * the parent's stdout (which we need for test output).
	 */
	long pid = do_fork();
	if (pid < 0) {
		check(0, "dup2: fork");
		return;
	}

	if (pid == 0) {
		/* Child: dup2 write end to fd 1, write via fd 1. */
		do_close(pipefd[0]);
		long r = do_dup2(pipefd[1], 1);
		if (r < 0)
			do_exit(1);
		/* Close original write end (fd 1 is now the pipe) */
		do_close(pipefd[1]);
		/* Write via stdout (fd 1) which is now the pipe. */
		const char *data = "piped!";
		long w = do_write(1, data, 6);
		if (w != 6)
			do_exit(2);
		do_close(1);
		do_exit(0);
	}
	/* Parent: close write end, read from read end. */
	do_close(pipefd[1]);
	char rbuf[16];
	for (int i = 0; i < 16; i++)
		rbuf[i] = 0;
	long total = 0;
	while (total < 6) {
		long n = do_read(pipefd[0], rbuf + total, 16 - total);
		if (n <= 0)
			break;
		total += n;
	}
	do_close(pipefd[0]);

	int status = 0;
	do_wait4(pid, &status, 0);
	int code = (status >> 8) & 0xff;

	int data_ok = (total == 6 &&
		       rbuf[0] == 'p' && rbuf[1] == 'i' && rbuf[2] == 'p' &&
		       rbuf[3] == 'e' && rbuf[4] == 'd' && rbuf[5] == '!');

	check(code == 0, "dup2: child exit");
	check(data_ok, "dup2: read piped data");
}

/*
 * ========================================================================
 * pipe full tests (from init_pipe_full.c)
 * ========================================================================
 */

/*
 * ---- Test 1: Write until pipe is full with O_NONBLOCK ----
 * Create a nonblocking pipe, write in 512-byte chunks until EAGAIN.
 * Verify total bytes written equals PIPE_BUF_SIZE.
 */
static void
test_write_until_full(void)
{
	int fds[2];
	long r = do_pipe2(fds, O_NONBLOCK);
	check(r == 0, "full: pipe2 O_NONBLOCK");

	char wbuf[512];
	for (int i = 0; i < 512; i++)
		wbuf[i] = (char)(i & 0xff);

	long total = 0;
	int got_eagain = 0;
	for (;;) {
		long w = do_write(fds[1], wbuf, 512);
		if (w == -EAGAIN) {
			got_eagain = 1;
			break;
		}
		if (w <= 0)
			break;
		total += w;
	}

	check(got_eagain, "full: write returned -EAGAIN when full");
	check(total == PIPE_BUF_SIZE, "full: total written == PIPE_BUF_SIZE");

	do_close(fds[0]);
	do_close(fds[1]);
}

/*
 * ---- Test 2: Read drains pipe ----
 * Fill the pipe, then read all data back, verify exact byte count.
 */
static void
test_read_drains(void)
{
	int fds[2];
	long r = do_pipe2(fds, O_NONBLOCK);
	check(r == 0, "drain: pipe2 O_NONBLOCK");

	/* Fill the pipe. */
	char wbuf[512];
	for (int i = 0; i < 512; i++)
		wbuf[i] = (char)(i & 0xff);

	long total_written = 0;
	for (;;) {
		long w = do_write(fds[1], wbuf, 512);
		if (w == -EAGAIN)
			break;
		if (w <= 0)
			break;
		total_written += w;
	}

	/* Read all data back. */
	char rbuf[512];
	long total_read = 0;
	for (;;) {
		long n = do_read(fds[0], rbuf, 512);
		if (n == -EAGAIN)
			break;
		if (n <= 0)
			break;
		total_read += n;
	}

	check(total_read == total_written, "drain: read back all bytes");
	check(total_read == PIPE_BUF_SIZE, "drain: total == PIPE_BUF_SIZE");

	do_close(fds[0]);
	do_close(fds[1]);
}

/*
 * ---- Test 3: Partial write near capacity ----
 * Drain the pipe, write 65000 bytes (nearly full), then try to write
 * 1000 more. With O_NONBLOCK, should get a short write (536 bytes)
 * since only PIPE_BUF_SIZE - 65000 = 536 bytes remain.
 */
static void
test_partial_write(void)
{
	int fds[2];
	long r = do_pipe2(fds, O_NONBLOCK);
	check(r == 0, "partial: pipe2 O_NONBLOCK");

	/* Fill pipe with 65000 bytes (512-byte chunks, then remainder) */
	char wbuf[512];
	for (int i = 0; i < 512; i++)
		wbuf[i] = (char)(i & 0xff);

	long target = 65000;
	long filled = 0;
	while (filled < target) {
		long chunk = target - filled;
		if (chunk > 512)
			chunk = 512;
		long w = do_write(fds[1], wbuf, chunk);
		if (w <= 0)
			break;
		filled += w;
	}
	check(filled == target, "partial: filled 65000 bytes");

	/* Now try to write 1000 bytes -- only 536 should fit. */
	long remaining_space = PIPE_BUF_SIZE - filled;	/* 536. */
	char extra[1000];
	for (int i = 0; i < 1000; i++)
		extra[i] = (char)(i & 0xff);

	long w = do_write(fds[1], extra, 1000);
	/* Should get a short write of exactly remaining_space bytes. */
	check(w == remaining_space, "partial: short write of remaining space");

	/* Now pipe should be completely full -- next write returns EAGAIN. */
	long w2 = do_write(fds[1], extra, 1);
	check(w2 == -EAGAIN, "partial: full pipe returns -EAGAIN");

	do_close(fds[0]);
	do_close(fds[1]);
}

/* ---- Test 4: Read from empty pipe with O_NONBLOCK returns -EAGAIN ---- */
static void
test_read_empty_eagain(void)
{
	int fds[2];
	long r = do_pipe2(fds, O_NONBLOCK);
	check(r == 0, "empty: pipe2 O_NONBLOCK");

	/* Pipe is empty -- read should return -EAGAIN. */
	char rbuf[64];
	long n = do_read(fds[0], rbuf, 64);
	check(n == -EAGAIN, "empty: read returns -EAGAIN");

	/* Write some data, read it all back, then verify empty again. */
	long w = do_write(fds[1], "hello", 5);
	check(w == 5, "empty: write 5 bytes");

	n = do_read(fds[0], rbuf, 64);
	check(n == 5, "empty: read 5 bytes back");

	/* Now empty again. */
	n = do_read(fds[0], rbuf, 64);
	check(n == -EAGAIN, "empty: read after drain returns -EAGAIN");

	do_close(fds[0]);
	do_close(fds[1]);
}

/*
 * ========================================================================
 * pipe edge tests (from init_pipe_edge.c)
 * ========================================================================
 */

/* ---- Test 1: Non-blocking read from empty pipe returns -EAGAIN ---- */
static void
test_nonblock_read_empty(void)
{
	int fds[2];
	long r = do_pipe2(fds, O_NONBLOCK);
	check(r == 0, "nb_read: pipe2 O_NONBLOCK ok");

	char buf[64];
	r = do_read(fds[0], buf, 64);
	check(r == -EAGAIN, "nb_read: empty pipe returns -EAGAIN");

	do_close(fds[0]);
	do_close(fds[1]);
}

/* ---- Test 2: Non-blocking write when pipe is full returns -EAGAIN ---- */
static void
test_nonblock_write_full(void)
{
	int fds[2];
	long r = do_pipe2(fds, O_NONBLOCK);
	check(r == 0, "nb_write: pipe2 O_NONBLOCK ok");

	/* Fill the pipe buffer completely. */
	char wbuf[512];
	for (int i = 0; i < 512; i++)
		wbuf[i] = (char)(i & 0xff);

	long total = 0;
	for (;;) {
		long w = do_write(fds[1], wbuf, 512);
		if (w == -EAGAIN)
			break;
		if (w <= 0)
			break;
		total += w;
	}
	check(total == PIPE_BUF_SIZE, "nb_write: filled pipe to capacity");

	/* One more write should return -EAGAIN. */
	r = do_write(fds[1], wbuf, 1);
	check(r == -EAGAIN, "nb_write: full pipe returns -EAGAIN");

	do_close(fds[0]);
	do_close(fds[1]);
}

/* ---- Test 3: Read zero bytes from pipe returns 0 ---- */
static void
test_read_zero(void)
{
	int fds[2];
	long r = do_pipe2(fds, 0);
	check(r == 0, "read0: pipe2 ok");

	char buf[1];
	r = do_read(fds[0], buf, 0);
	check(r == 0, "read0: read(fd, buf, 0) returns 0");

	do_close(fds[0]);
	do_close(fds[1]);
}

/* ---- Test 4: Write zero bytes to pipe returns 0 ---- */
static void
test_write_zero(void)
{
	int fds[2];
	long r = do_pipe2(fds, 0);
	check(r == 0, "write0: pipe2 ok");

	char buf[1];
	r = do_write(fds[1], buf, 0);
	check(r == 0, "write0: write(fd, buf, 0) returns 0");

	do_close(fds[0]);
	do_close(fds[1]);
}

/* ---- Test 5: Multiple independent pipes ---- */
static void
test_multiple_pipes(void)
{
#define NUM_PIPES 8
	int pfds[NUM_PIPES][2];
	int create_ok = 1;

	/* Create 8 pipes. */
	for (int i = 0; i < NUM_PIPES; i++) {
		long r = do_pipe2(pfds[i], O_NONBLOCK);
		if (r != 0) {
			create_ok = 0;
			break;
		}
	}
	check(create_ok, "multi: created 8 pipes");

	/* Write unique byte to each pipe. */
	int write_ok = 1;
	for (int i = 0; i < NUM_PIPES; i++) {
		char val = (char)(0x40 + i);
		long w = do_write(pfds[i][1], &val, 1);
		if (w != 1) {
			write_ok = 0;
			break;
		}
	}
	check(write_ok, "multi: wrote unique byte to each pipe");

	/* Read back from each pipe and verify independence. */
	int read_ok = 1;
	for (int i = 0; i < NUM_PIPES; i++) {
		char val = 0;
		long n = do_read(pfds[i][0], &val, 1);
		if (n != 1 || val != (char)(0x40 + i)) {
			read_ok = 0;
			break;
		}
	}
	check(read_ok, "multi: each pipe returned correct unique byte");

	/* Verify all pipes are now empty. */
	int empty_ok = 1;
	for (int i = 0; i < NUM_PIPES; i++) {
		char val;
		long n = do_read(pfds[i][0], &val, 1);
		if (n != -EAGAIN) {
			empty_ok = 0;
			break;
		}
	}
	check(empty_ok, "multi: all pipes empty after read");

	/* Close all. */
	for (int i = 0; i < NUM_PIPES; i++) {
		do_close(pfds[i][0]);
		do_close(pfds[i][1]);
	}
#undef NUM_PIPES
}

/*
 * ========================================================================
 * main
 * ========================================================================
 */

int
main(void)
{
	msg("=== pipe_all tests ===\n");

	msg("--- pipe basic ---\n");
	test_large_transfer();
	test_sigpipe();
	test_fill_drain();
	test_close_eof();
	test_dup2_pipe();

	msg("--- pipe full ---\n");
	test_write_until_full();
	test_read_drains();
	test_partial_write();
	test_read_empty_eagain();

	msg("--- pipe edge ---\n");
	test_nonblock_read_empty();
	test_nonblock_write_full();
	test_read_zero();
	test_write_zero();
	test_multiple_pipes();

	test_done();
	return 0;
}
