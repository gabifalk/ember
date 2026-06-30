/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/console.h"
#include "ember/io.h"
#include "ember/spinlock.h"

#define SERIAL_BASE 0x3F8

/* TX ring buffer: buffer writes and drain opportunistically. */
#define SERIAL_TX_RING_SIZE 4096
static uint8_t serial_tx_buf[SERIAL_TX_RING_SIZE];
static volatile int serial_tx_head;	/* Producer index. */
static volatile int serial_tx_tail;	/* Consumer index. */
static spinlock_t serial_tx_lock = SPINLOCK_INIT;

static inline int
uart_tx_ready(void)
{
	return (inb(SERIAL_BASE + 5) & 0x20) != 0;
}

/*
 * Drain as many bytes from the TX ring as the UART will accept right now.
 * Must be called with serial_tx_lock held.
 */
static void
serial_tx_drain_locked(void)
{
	while (serial_tx_head != serial_tx_tail && uart_tx_ready()) {
		outb(SERIAL_BASE, serial_tx_buf[serial_tx_tail]);
		serial_tx_tail = (serial_tx_tail + 1) % SERIAL_TX_RING_SIZE;
	}
}

void
qemu_poweroff(void)
{
	/* ACPI PM1a_CNT S5 shutdown -- works on q35 (ICH9) */
	outw(0x604, 0x2000);
	/* Fallback: QEMU isa-debug-exit device (if present) */
	outb(0xf4, 0x00);
}

void
serial_init(void)
{
	outb(SERIAL_BASE + 1, 0x00);
	outb(SERIAL_BASE + 3, 0x80);
	outb(SERIAL_BASE + 0, 0x01);
	outb(SERIAL_BASE + 1, 0x00);
	outb(SERIAL_BASE + 3, 0x03);
	outb(SERIAL_BASE + 2, 0xC7);
	outb(SERIAL_BASE + 4, 0x0B);
}

void
serial_putc(char c)
{
	uint64_t flags = spin_lock_irqsave(&serial_tx_lock);
	/* Opportunistically drain any buffered bytes first. */
	serial_tx_drain_locked();
	/* Fast path: UART ready and ring is empty -- send directly. */
	if (serial_tx_head == serial_tx_tail && uart_tx_ready()) {
		outb(SERIAL_BASE, (uint8_t) c);
	} else {
		/* Buffer the character; drop it if the ring is full. */
		int next = (serial_tx_head + 1) % SERIAL_TX_RING_SIZE;
		if (next != serial_tx_tail) {
			serial_tx_buf[serial_tx_head] = (uint8_t) c;
			serial_tx_head = next;
		}
	}
	spin_unlock_irqrestore(&serial_tx_lock, flags);
}

/*
 * Busy-wait until the TX ring is fully drained.
 * Call before poweroff to ensure all output reaches QEMU.
 */
void
serial_flush(void)
{
	uint64_t flags = spin_lock_irqsave(&serial_tx_lock);
	while (serial_tx_head != serial_tx_tail) {
		while (!uart_tx_ready()) {
			__asm__ __volatile__("pause");
		}
		outb(SERIAL_BASE, serial_tx_buf[serial_tx_tail]);
		serial_tx_tail = (serial_tx_tail + 1) % SERIAL_TX_RING_SIZE;
	}
	spin_unlock_irqrestore(&serial_tx_lock, flags);
}

void
serial_write(const char *s)
{
	while (*s) {
		if (*s == '\n') {
			serial_putc('\r');
		}
		serial_putc(*s++);
	}
}

int
serial_data_ready(void)
{
	return (inb(0x3F8 + 5) & 0x01) != 0;
}

/* Small ring buffer for chars consumed by signal polling but not yet read. */
#define SERIAL_RING_SIZE 64
static uint8_t serial_ring[SERIAL_RING_SIZE];
static volatile int serial_ring_head, serial_ring_tail;

void
serial_ring_push(uint8_t c)
{
	int next = (serial_ring_head + 1) % SERIAL_RING_SIZE;
	if (next != serial_ring_tail) {
		serial_ring[serial_ring_head] = c;
		serial_ring_head = next;
	}
}

/*
 * Intercept Ctrl+T (trace toggle) at the lowest level so it works
 * even when timer_handler/console_poll_signals rarely fires (SMP).
 */
extern volatile int syscall_trace_interval;

int
serial_getc(void)
{
	int c;
	/* First drain the ring buffer (chars from signal polling) */
	for (;;) {
		if (serial_ring_tail != serial_ring_head) {
			c = serial_ring[serial_ring_tail];
			serial_ring_tail =
			    (serial_ring_tail + 1) % SERIAL_RING_SIZE;
		} else if (serial_data_ready()) {
			c = (int)inb(0x3F8);
		} else {
			return -1;
		}
		if (c == 0x14) {	/* Ctrl+T: handle here, don't return to caller. */
			if (syscall_trace_interval) {
				syscall_trace_interval = 0;
				serial_write("[trace off]\n");
			} else {
				syscall_trace_interval = 100;
				serial_write("[trace on: 1s]\n");
			}
			continue;	/* Consume and try next char. */
		}
		return c;
	}
}
