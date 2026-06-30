/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "ember/console.h"
#include "ember/fbcon.h"
#include "ember/spinlock.h"
#include "ember/io.h"
#include "ember/proc.h"
#include "ember/signal.h"
#include "ember/sched.h"
#include "ember/percpu.h"

/* Syscall trace: when non-zero, timer dumps pid+syscall every N ticks. */
volatile int syscall_trace_interval;

static spinlock_t console_lock = SPINLOCK_INIT;
static int fbcon_enabled = 0;

void
console_init(boot_info_v1_t * bi)
{
	serial_init();
	fbcon_init(bi);
	fbcon_enabled = 1;
}

void
console_putc(char c)
{
	uint64_t flags = spin_lock_irqsave(&console_lock);
	serial_putc(c);
	if (fbcon_enabled)
		fbcon_putc(c);
	spin_unlock_irqrestore(&console_lock, flags);
}

void
console_write(const char *s)
{
	uint64_t flags = spin_lock_irqsave(&console_lock);
	serial_write(s);
	if (fbcon_enabled)
		fbcon_write(s);
	spin_unlock_irqrestore(&console_lock, flags);
}

void
console_poll_signals(void)
{
	while (serial_data_ready()) {
		int ch = (int)inb(0x3F8);
		if (ch < 0)
			break;
		if (ch == 0x14) {	/* Ctrl+T: toggle syscall tracer. */
			if (syscall_trace_interval) {
				syscall_trace_interval = 0;
				console_write("[trace off]\n");
			} else {
				syscall_trace_interval = 100;	/* Every 1s at 100Hz. */
				console_write("[trace on: 1s]\n");
			}
			continue;
		}
		if (ch == 0x03) {	/* Ctrl+C. */
			extern volatile int console_fg_pgid;
			int pgid = console_fg_pgid;
			if (pgid <= 0) {
				proc_t *cur = current_proc;
				pgid = cur ? cur->pgid : 0;
			}
			if (pgid > 0) {
				uint64_t sf = spin_lock_irqsave(&sched_lock);
				for (int i = 0; i < MAX_PROCS; i++) {
					if (procs[i].state != PROC_UNUSED
					    && procs[i].pgid == pgid) {
						procs[i].sig_pending |=
						    (1u << SIGINT);
						if (procs[i].state ==
						    PROC_SLEEPING)
							procs[i].state =
							    PROC_READY;
					}
				}
				spin_unlock_irqrestore(&sched_lock, sf);
			}
		} else {
			serial_ring_push((uint8_t) ch);
		}
	}
}

void
console_hex64(uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	uint64_t flags = spin_lock_irqsave(&console_lock);
	serial_write("0x");
	if (fbcon_enabled)
		fbcon_write("0x");
	for (int i = 60; i >= 0; i -= 4) {
		serial_putc(hex[(v >> i) & 0xF]);
		if (fbcon_enabled)
			fbcon_putc(hex[(v >> i) & 0xF]);
	}
	spin_unlock_irqrestore(&console_lock, flags);
}
