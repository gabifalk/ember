/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

#define USER_ADDR_MAX  0x800000000000ULL	/* End of lower canonical half. */
#define MAX_RW_SIZE    0x7ffff000ULL	/* Max single read/write size. */

/* -- Sys_read per-type helpers ---------------------------------------- */

/*
 * Read from console (line-buffered, with echo, Ctrl-C/D handling).
 * Returns bytes read (>=0) or -errno.
 */
static int64_t
read_console(fd_entry_t * entry, char *buf, uint64_t count)
{
	if (count == 0)
		return 0;
	uint64_t nread = 0;
	for (;;) {
		int ch = serial_getc();
		if (ch >= 0) {
			char c = (char)ch;
			if (c == CTRL_C) {
				console_signal_fg(SIGINT);
				if (nread > 0)
					break;
				return -EINTR;
			}
			if (c == CTRL_D) {
				break;	/* Return nread (0 = EOF) */
			}
			/* Normalize \r to \n. */
			if (c == '\r')
				c = '\n';
			if (console_echo)
				console_putc(c);
			USER_ACCESS_BEGIN();
			buf[nread] = c;
			USER_ACCESS_END();
			nread++;
			/* Line-buffered: stop at newline. */
			if (c == '\n' || nread >= count)
				break;
			continue;
		}
		if (nread > 0)
			break;
		/* No data yet. */
		if (entry->desc->flags & O_NONBLOCK)
			return -EAGAIN;
		if (current_proc) {
			uint32_t pend =
			    current_proc->sig_pending & ~current_proc->sig_mask;
			if (pend)
				return -EINTR;
		}
		sched_sleep(SCHED_TICK_CHAN);
	}
	return (int64_t) nread;
}

/* Read from /dev/null -- always EOF. */
static int64_t
read_devnull(void)
{
	return 0;
}

/* Read from /dev/zero -- fills buffer with zeroes. */
static int64_t
read_devzero(char *buf, uint64_t count)
{
	USER_ACCESS_BEGIN();
	for (uint64_t i = 0; i < count; i++)
		buf[i] = 0;
	USER_ACCESS_END();
	return (int64_t) count;
}

/* Read from /dev/urandom -- fills buffer with pseudo-random bytes. */
static int64_t
read_devrandom(char *buf, uint64_t count)
{
	extern volatile uint64_t kernel_ticks;
	static uint64_t rng_state;
	if (!rng_state)
		rng_state = kernel_ticks;
	USER_ACCESS_BEGIN();
	for (uint64_t i = 0; i < count; i++) {
		rng_state =
		    rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
		buf[i] = (char)(rng_state >> 33);
	}
	USER_ACCESS_END();
	return (int64_t) count;
}

/* Read from pipe (blocking). Returns bytes read (>=0) or -errno. */
static int64_t
read_pipe(fd_entry_t * entry, char *buf, uint64_t count)
{
	if (!entry->desc->pipe || count == 0)
		return 0;
	pipe_t *p = (pipe_t *) entry->desc->pipe;
	int rc = pipe_wait_readable(p, entry->desc);
	if (rc < 0)
		return (int64_t) rc;
	if (pipe_is_empty(p))
		return 0;
	USER_ACCESS_BEGIN();
	uint64_t nread = pipe_read(p, buf, count);
	USER_ACCESS_END();
	sched_wakeup(p->wake_chan);
	return (int64_t) nread;
}

/* Read from VFS file at current offset (advances offset). */
static int64_t
read_file(fd_entry_t * entry, char *buf, uint64_t count)
{
	USER_ACCESS_BEGIN();
	spin_lock(&entry->desc->lock);
	uint64_t nread =
	    vfs_read(entry->desc->node, entry->desc->offset, buf, count);
	entry->desc->offset += nread;
	spin_unlock(&entry->desc->lock);
	USER_ACCESS_END();
	return (int64_t) nread;
}

/* -- Sys_read -------------------------------------------------------- */

uint64_t
sys_read(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	char *buf = (char *)f->rsi;
	uint64_t count = f->rdx;
	if (count > MAX_RW_SIZE)
		count = MAX_RW_SIZE;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	if (!buf && count > 0) {
		f->rax = SYSCALL_ERR(EFAULT);
		return f->rax;
	}
	/* Validate user buffer range. */
	if (count > 0) {
		uintptr_t bstart = (uintptr_t) buf;
		uintptr_t bend = bstart + count;
		if (bend < bstart || bend > USER_ADDR_MAX) {
			f->rax = SYSCALL_ERR(EFAULT);
			return f->rax;
		}
	}
	int64_t ret;
	switch (entry->desc->type) {
	case FD_TYPE_CONSOLE:
		ret = read_console(entry, buf, count);
		break;
	case FD_TYPE_DEVNULL:
		ret = read_devnull();
		break;
	case FD_TYPE_DEVZERO:
		ret = read_devzero(buf, count);
		break;
	case FD_TYPE_DEVRANDOM:
		ret = read_devrandom(buf, count);
		break;
	case FD_TYPE_PIPE_READ:
		ret = read_pipe(entry, buf, count);
		break;
	case FD_TYPE_FILE:
		ret = read_file(entry, buf, count);
		break;
	case FD_TYPE_PROC_IOMEM:{
			char *buf_start = (char *)entry->desc->node;
			uint64_t total_len = 0;
			while (buf_start[total_len])
				total_len++;
			uint64_t pos = entry->desc->offset;
			if (pos >= total_len) {
				ret = 0;
				break;
			}
			uint64_t avail = total_len - pos;
			uint64_t to_copy = (count < avail) ? count : avail;
			USER_ACCESS_BEGIN();
			char *dest = (char *)(uintptr_t) buf;
			uint64_t ci;
			for (ci = 0; ci < to_copy; ci++)
				dest[ci] = buf_start[pos + ci];
			USER_ACCESS_END();
			entry->desc->offset += to_copy;
			ret = (int64_t) to_copy;
			break;
		}
	default:
		ret = -EBADF;
		break;
	}
	f->rax = (uint64_t) ret;
	return f->rax;
}

/* -- Sys_write per-type helpers --------------------------------------- */

/* Write to console. Always succeeds, returns len. */
static int64_t
write_console(const char *buf, uint64_t len)
{
	USER_ACCESS_BEGIN();
	write_user_buf(buf, len);
	USER_ACCESS_END();
	return (int64_t) len;
}

/* Write to /dev/null or /dev/zero -- discard, return len. */
static int64_t
write_devnull(uint64_t len)
{
	return (int64_t) len;
}

/*
 * Write to pipe (loops until all bytes written, matching Linux behavior).
 * Returns bytes written (>0) or -errno.
 */
static int64_t
write_pipe(fd_entry_t * entry, const char *buf, uint64_t len)
{
	if (!entry->desc->pipe) {
		if (current_proc)
			current_proc->sig_pending |= (1u << SIGPIPE);
		return -EPIPE;
	}
	pipe_t *p = (pipe_t *) entry->desc->pipe;
	uint64_t total_written = 0;
	while (total_written < len) {
		int rc = pipe_wait_writable(p, entry->desc);
		if (rc < 0) {
			if (total_written > 0)
				return (int64_t) total_written;
			return (int64_t) rc;
		}
		if (pipe_no_readers(p)) {
			if (total_written > 0)
				return (int64_t) total_written;
			if (current_proc)
				current_proc->sig_pending |= (1u << SIGPIPE);
			return -EPIPE;
		}
		USER_ACCESS_BEGIN();
		uint64_t nw =
		    pipe_write(p, buf + total_written, len - total_written);
		USER_ACCESS_END();
		sched_wakeup(p->wake_chan);
		total_written += nw;
	}
	return (int64_t) total_written;
}

/* Write to VFS file at current offset (O_APPEND aware, advances offset). */
static int64_t
write_file(fd_entry_t * entry, const char *buf, uint64_t len)
{
	USER_ACCESS_BEGIN();
	spin_lock(&entry->desc->lock);
	if (entry->desc->flags & O_APPEND)
		entry->desc->offset = entry->desc->node->size;
	uint64_t nwritten =
	    vfs_write(entry->desc->node, entry->desc->offset, buf, len);
	entry->desc->offset += nwritten;
	spin_unlock(&entry->desc->lock);
	USER_ACCESS_END();
	return (int64_t) nwritten;
}

/* -- Sys_write ------------------------------------------------------- */

uint64_t
sys_write(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	const char *buf = (const char *)f->rsi;
	uint64_t len = f->rdx;
	if (len > MAX_RW_SIZE)
		len = MAX_RW_SIZE;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	if (!buf && len > 0) {
		f->rax = SYSCALL_ERR(EFAULT);
		return f->rax;
	}
	/* Validate user buffer range. */
	if (len > 0) {
		uintptr_t bstart = (uintptr_t) buf;
		uintptr_t bend = bstart + len;
		if (bend < bstart || bend > USER_ADDR_MAX) {
			f->rax = SYSCALL_ERR(EFAULT);
			return f->rax;
		}
	}
	int64_t ret;
	switch (entry->desc->type) {
	case FD_TYPE_CONSOLE:
		ret = write_console(buf, len);
		break;
	case FD_TYPE_DEVNULL:	/* Fall through. */
	case FD_TYPE_DEVZERO:	/* Fall through. */
	case FD_TYPE_DEVRANDOM:
		ret = write_devnull(len);
		break;
	case FD_TYPE_PROC_IOMEM:
		ret = -EINVAL;
		break;
	case FD_TYPE_PIPE_WRITE:
		ret = write_pipe(entry, buf, len);
		break;
	case FD_TYPE_FILE:
		ret = write_file(entry, buf, len);
		break;
	default:
		ret = -EBADF;
		break;
	}
	f->rax = (uint64_t) ret;
	return f->rax;
}

uint64_t
sys_writev(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	const struct iovec *iov_user = (const struct iovec *)f->rsi;
	uint64_t iovcnt = f->rdx;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	USER_ACCESS_BEGIN();

	uint64_t total = 0;
	for (uint64_t i = 0; i < iovcnt; i++) {
		const char *base =
		    (const char *)(uintptr_t) iov_user[i].iov_base;
		uint64_t len = iov_user[i].iov_len;
		if (len == 0)
			continue;
		if (entry->desc->type == FD_TYPE_CONSOLE) {
			write_user_buf(base, len);
			total += len;
		} else if (entry->desc->type == FD_TYPE_DEVNULL
			   || entry->desc->type == FD_TYPE_DEVZERO
			   || entry->desc->type == FD_TYPE_DEVRANDOM) {
			total += len;
		} else if (entry->desc->type == FD_TYPE_PIPE_WRITE) {
			if (!entry->desc->pipe) {
				USER_ACCESS_END();
				if (current_proc)
					current_proc->sig_pending |=
					    (1u << SIGPIPE);
				f->rax = SYSCALL_ERR(EPIPE);
				return f->rax;
			}
			pipe_t *p = (pipe_t *) entry->desc->pipe;
			/* Loop until all bytes of this iovec written. */
			uint64_t iov_written = 0;
			while (iov_written < len) {
				for (;;) {
					spin_lock(&p->lock);
					int ready = (p->count < PIPE_BUF_SIZE
						     || p->readers == 0);
					spin_unlock(&p->lock);
					if (ready)
						break;
					if (entry->desc->flags & O_NONBLOCK) {
						USER_ACCESS_END();
						total += iov_written;
						if (total > 0) {
							f->rax = total;
							return total;
						}
						f->rax = SYSCALL_ERR(EAGAIN);
						return f->rax;
					}
					uint64_t sf2 =
					    spin_lock_irqsave(&sched_lock);
					if (current_proc) {
						uint32_t pend =
						    current_proc->
						    sig_pending &
						    ~current_proc->sig_mask;
						if (pend) {
							spin_unlock_irqrestore
							    (&sched_lock, sf2);
							USER_ACCESS_END();
							total += iov_written;
							if (total > 0) {
								f->rax = total;
								return total;
							}
							f->rax =
							    SYSCALL_ERR(EINTR);
							return f->rax;
						}
					}
					current_proc->state = PROC_SLEEPING;
					current_proc->wait_chan = p->wake_chan;
					spin_unlock_irqrestore(&sched_lock,
							       sf2);
					/* Recheck after registering sleep to prevent lost wakeup. */
					{
						spin_lock(&p->lock);
						int recheck =
						    (p->count < PIPE_BUF_SIZE
						     || p->readers == 0);
						spin_unlock(&p->lock);
						if (recheck) {
							sf2 =
							    spin_lock_irqsave
							    (&sched_lock);
							if (current_proc->
							    state ==
							    PROC_SLEEPING)
								current_proc->
								    state =
								    PROC_READY;
							spin_unlock_irqrestore
							    (&sched_lock, sf2);
							continue;
						}
					}
					schedule();
				}
				spin_lock(&p->lock);
				int no_readers = (p->readers == 0);
				spin_unlock(&p->lock);
				if (no_readers) {
					USER_ACCESS_END();
					total += iov_written;
					if (total > 0) {
						f->rax = total;
						return total;
					}
					if (current_proc)
						current_proc->sig_pending |=
						    (1u << SIGPIPE);
					f->rax = SYSCALL_ERR(EPIPE);
					return f->rax;
				}
				uint64_t nw =
				    pipe_write(p, base + iov_written,
					       len - iov_written);
				sched_wakeup(p->wake_chan);
				iov_written += nw;
			}
			total += iov_written;
		} else if (entry->desc->type == FD_TYPE_FILE) {
			spin_lock(&entry->desc->lock);
			uint64_t nw =
			    vfs_write(entry->desc->node, entry->desc->offset,
				      base, len);
			entry->desc->offset += nw;
			spin_unlock(&entry->desc->lock);
			total += nw;
			if (nw < len)
				break;
		} else {
			USER_ACCESS_END();
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
	}

	USER_ACCESS_END();
	f->rax = total;
	return total;
}

uint64_t
sys_readv(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	const struct iovec *iov_user = (const struct iovec *)f->rsi;
	uint64_t iovcnt = f->rdx;
	fd_entry_t *entry = syscall_fd_get(fd, f);
	if (!entry)
		return f->rax;
	/* For pipes: blocking loop first. */
	if (entry->desc->type == FD_TYPE_PIPE_READ) {
		if (!entry->desc->pipe) {
			f->rax = 0;
			return 0;
		}
		pipe_t *p = (pipe_t *) entry->desc->pipe;
		int rc = pipe_wait_readable(p, entry->desc);
		if (rc < 0) {
			f->rax = (uint64_t) rc;
			return f->rax;
		}
		if (pipe_is_empty(p)) {
			f->rax = 0;
			return 0;
		}
		USER_ACCESS_BEGIN();
		uint64_t total = 0;
		for (uint64_t i = 0; i < iovcnt; i++) {
			char *base = (char *)(uintptr_t) iov_user[i].iov_base;
			uint64_t len = iov_user[i].iov_len;
			if (len == 0)
				continue;
			uint64_t nr = pipe_read(p, base, len);
			total += nr;
			if (nr < len)
				break;
		}
		USER_ACCESS_END();
		sched_wakeup(p->wake_chan);
		f->rax = total;
		return total;
	}

	USER_ACCESS_BEGIN();

	uint64_t total = 0;
	for (uint64_t i = 0; i < iovcnt; i++) {
		char *base = (char *)(uintptr_t) iov_user[i].iov_base;
		uint64_t len = iov_user[i].iov_len;
		if (len == 0)
			continue;
		if (entry->desc->type == FD_TYPE_DEVNULL) {
			break;	/* EOF. */
		} else if (entry->desc->type == FD_TYPE_CONSOLE) {
			/* Blocking serial read for console readv. */
			int got_data = 0;
			for (uint64_t j = 0; j < len; j++) {
				int ch;
				for (;;) {
					ch = serial_getc();
					if (ch >= 0)
						break;
					if (got_data || total > 0)
						goto readv_console_done;
					USER_ACCESS_END();
					if (current_proc
					    && (current_proc->
						sig_pending & ~current_proc->
						sig_mask)) {
						f->rax =
						    total >
						    0 ? total :
						    SYSCALL_ERR(EINTR);
						return f->rax;
					}
					sched_sleep(SCHED_TICK_CHAN);
					if (_ua_ucr3)
						write_cr3(_ua_ucr3);
				}
				char c = (char)ch;
				if (c == CTRL_C) {
					USER_ACCESS_END();
					console_signal_fg(SIGINT);
					f->rax =
					    total >
					    0 ? total : SYSCALL_ERR(EINTR);
					return f->rax;
				}
				if (c == CTRL_D) {
					goto readv_console_done;
				}
				if (c == '\r')
					c = '\n';
				if (console_echo)
					console_putc(c);
				base[j] = c;
				total++;
				got_data = 1;
				if (c == '\n')
					goto readv_console_done;
			}
 readv_console_done:
			if (total > 0)
				break;	/* Return what we have. */
			break;
		} else if (entry->desc->type == FD_TYPE_DEVZERO) {
			for (uint64_t j = 0; j < len; j++)
				base[j] = 0;
			total += len;
		} else if (entry->desc->type == FD_TYPE_DEVRANDOM) {
			extern volatile uint64_t kernel_ticks;
			static uint64_t rv_rng;
			if (!rv_rng)
				rv_rng = kernel_ticks;
			for (uint64_t j = 0; j < len; j++) {
				rv_rng =
				    rv_rng * 6364136223846793005ULL +
				    1442695040888963407ULL;
				base[j] = (char)(rv_rng >> 33);
			}
			total += len;
		} else if (entry->desc->type == FD_TYPE_FILE) {
			spin_lock(&entry->desc->lock);
			uint64_t nr =
			    vfs_read(entry->desc->node, entry->desc->offset,
				     base, len);
			entry->desc->offset += nr;
			spin_unlock(&entry->desc->lock);
			total += nr;
			if (nr < len)
				break;
		} else {
			break;
		}
	}

	USER_ACCESS_END();
	f->rax = total;
	return total;
}

uint64_t
sys_pread64(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	char *buf = (char *)f->rsi;
	uint64_t count = f->rdx;
	uint64_t offset = f->r10;
	fd_entry_t *entry = fd_get(fd);
	if (!entry || entry->desc->type != FD_TYPE_FILE) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	USER_ACCESS_BEGIN();
	uint64_t nread = vfs_read(entry->desc->node, offset, buf, count);
	USER_ACCESS_END();
	f->rax = nread;
	return nread;
}

uint64_t
sys_pwrite64(syscall_frame_t * f)
{
	int fd = (int)f->rdi;
	const char *buf = (const char *)f->rsi;
	uint64_t count = f->rdx;
	uint64_t offset = f->r10;
	fd_entry_t *entry = fd_get(fd);
	if (!entry || entry->desc->type != FD_TYPE_FILE) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	USER_ACCESS_BEGIN();
	uint64_t nwritten = vfs_write(entry->desc->node, offset, buf, count);
	USER_ACCESS_END();
	f->rax = nwritten;
	return nwritten;
}

/* -- Sendfile read helpers ------------------------------------------- */

/* Read from a VFS file into kernel buffer.  Returns bytes read (0 = EOF). */
static int64_t
sendfile_read_file(fd_entry_t * in, uint64_t offset, char *buf, uint64_t chunk)
{
	return (int64_t) vfs_read(in->desc->node, offset, buf, chunk);
}

/*
 * Read one line from console into kernel buffer (line-buffered with echo).
 * Returns bytes read (>0), 0 on Ctrl-D at start (EOF), or -errno.
 */
static int64_t
sendfile_read_console(char *buf, uint64_t chunk)
{
	uint64_t nr = 0;
	for (uint64_t i = 0; i < chunk; i++) {
		int ch;
		for (;;) {
			ch = serial_getc();
			if (ch >= 0)
				break;
			if (current_proc
			    && (current_proc->sig_pending & ~current_proc->
				sig_mask)) {
				if (nr > 0)
					return (int64_t) nr;
				return -EINTR;
			}
			sched_sleep(SCHED_TICK_CHAN);
		}
		char c = (char)ch;
		if (c == CTRL_C) {
			console_signal_fg(SIGINT);
			if (nr > 0)
				return (int64_t) nr;
			return -EINTR;
		}
		if (c == CTRL_D) {
			return (int64_t) nr;	/* 0 If at start = EOF. */
		}
		if (c == '\r')
			c = '\n';
		if (console_echo)
			console_putc(c);
		buf[nr++] = c;
		if (c == '\n')
			break;
	}
	return (int64_t) nr;
}

/*
 * Read from a pipe into kernel buffer.
 * Returns bytes read (>0), 0 on pipe-closed/empty, or -errno on signal.
 */
static int64_t
sendfile_read_pipe(fd_entry_t * in, char *buf, uint64_t chunk)
{
	if (!in->desc->pipe)
		return 0;
	pipe_t *p = (pipe_t *) in->desc->pipe;

	/* Block until data available or EOF. */
	for (;;) {
		spin_lock(&p->lock);
		int ready = (p->count > 0 || p->writers == 0);
		spin_unlock(&p->lock);
		if (ready)
			break;
		uint64_t sf2 = spin_lock_irqsave(&sched_lock);
		if (current_proc
		    && (current_proc->sig_pending & ~current_proc->sig_mask)) {
			spin_unlock_irqrestore(&sched_lock, sf2);
			break;
		}
		current_proc->state = PROC_SLEEPING;
		current_proc->wait_chan = p->wake_chan;
		spin_unlock_irqrestore(&sched_lock, sf2);
		/* Recheck after registering sleep to prevent lost wakeup. */
		{
			spin_lock(&p->lock);
			int recheck = (p->count > 0 || p->writers == 0);
			spin_unlock(&p->lock);
			if (recheck) {
				sf2 = spin_lock_irqsave(&sched_lock);
				if (current_proc->state == PROC_SLEEPING)
					current_proc->state = PROC_READY;
				spin_unlock_irqrestore(&sched_lock, sf2);
				continue;
			}
		}
		schedule();
	}
	if (p->count == 0)
		return 0;
	int64_t nr = (int64_t) pipe_read(p, buf, chunk);
	sched_wakeup(p->wake_chan);
	return nr;
}

/* -- Sendfile write helpers ------------------------------------------ */

/* Write kernel buffer to console.  Always succeeds, returns nr. */
static int64_t
sendfile_write_console(const char *buf, uint64_t nr)
{
	for (uint64_t i = 0; i < nr; i++)
		console_putc(buf[i]);
	return (int64_t) nr;
}

/* Write kernel buffer to VFS file.  Returns bytes written. */
static int64_t
sendfile_write_file(fd_entry_t * out, const char *buf, uint64_t nr)
{
	spin_lock(&out->desc->lock);
	uint64_t nw = vfs_write(out->desc->node, out->desc->offset, buf, nr);
	out->desc->offset += nw;
	spin_unlock(&out->desc->lock);
	return (int64_t) nw;
}

/* Write kernel buffer to pipe.  Returns bytes written, or -EPIPE. */
static int64_t
sendfile_write_pipe(fd_entry_t * out, const char *buf, uint64_t nr)
{
	if (!out->desc->pipe) {
		if (current_proc)
			current_proc->sig_pending |= (1u << SIGPIPE);
		return -EPIPE;
	}
	int64_t nw = (int64_t) pipe_write((pipe_t *) out->desc->pipe, buf, nr);
	sched_wakeup(((pipe_t *) out->desc->pipe)->wake_chan);
	return nw;
}

/* -- Sendfile orchestrator ------------------------------------------- */

uint64_t
sys_sendfile(syscall_frame_t * f)
{
	int out_fd = (int)f->rdi;
	int in_fd = (int)f->rsi;
	int64_t *user_offset = (int64_t *) f->rdx;
	uint64_t count = f->r10;
	fd_entry_t *out_entry = fd_get(out_fd);
	fd_entry_t *in_entry = fd_get(in_fd);
	if (!out_entry || !in_entry) {
		f->rax = SYSCALL_ERR(EBADF);
		return f->rax;
	}
	uint64_t offset;
	if (user_offset) {
		USER_ACCESS_BEGIN();
		offset = (uint64_t) * user_offset;
		USER_ACCESS_END();
	} else {
		spin_lock(&in_entry->desc->lock);
		offset = in_entry->desc->offset;
		spin_unlock(&in_entry->desc->lock);
	}

	char tmp[512];
	uint64_t total = 0;
	while (total < count) {
		uint64_t chunk = count - total;
		if (chunk > sizeof(tmp))
			chunk = sizeof(tmp);

		/* Read from input. */
		int64_t nr;
		if (in_entry->desc->type == FD_TYPE_FILE)
			nr = sendfile_read_file(in_entry, offset, tmp, chunk);
		else if (in_entry->desc->type == FD_TYPE_CONSOLE)
			nr = sendfile_read_console(tmp, chunk);
		else if (in_entry->desc->type == FD_TYPE_PIPE_READ)
			nr = sendfile_read_pipe(in_entry, tmp, chunk);
		else
			break;
		if (nr < 0) {
			/* Error -- return partial transfer or the error. */
			if (total > 0)
				break;
			f->rax = (uint64_t) nr;
			return f->rax;
		}
		if (nr == 0)
			break;

		/* Write to output. */
		int64_t nw;
		if (out_entry->desc->type == FD_TYPE_CONSOLE)
			nw = sendfile_write_console(tmp, (uint64_t) nr);
		else if (out_entry->desc->type == FD_TYPE_FILE)
			nw = sendfile_write_file(out_entry, tmp, (uint64_t) nr);
		else if (out_entry->desc->type == FD_TYPE_PIPE_WRITE)
			nw = sendfile_write_pipe(out_entry, tmp, (uint64_t) nr);
		else if (out_entry->desc->type == FD_TYPE_DEVNULL
			 || out_entry->desc->type == FD_TYPE_DEVZERO)
			nw = nr;
		else
			break;
		if (nw < 0) {
			/* Write error -- return partial transfer or the error. */
			if (total > 0)
				break;
			f->rax = (uint64_t) nw;
			return f->rax;
		}

		offset += (uint64_t) nr;
		total += (uint64_t) nw;
		if (nw < nr)
			break;
	}

	if (user_offset) {
		USER_ACCESS_BEGIN();
		*user_offset = (int64_t) offset;
		USER_ACCESS_END();
	} else {
		spin_lock(&in_entry->desc->lock);
		in_entry->desc->offset = offset;
		spin_unlock(&in_entry->desc->lock);
	}
	f->rax = total;
	return total;
}
