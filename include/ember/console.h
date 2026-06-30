/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_CONSOLE_H
#define EMBER_CONSOLE_H

#include "boot_info.h"

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_flush(void);
int serial_data_ready(void);
int serial_getc(void);
void serial_ring_push(uint8_t c);

void qemu_poweroff(void);
void console_poll_signals(void);

void console_init(boot_info_v1_t * bi);
void console_putc(char c);
void console_write(const char *s);
void console_hex64(uint64_t v);

#endif
