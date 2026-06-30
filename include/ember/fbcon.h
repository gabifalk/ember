/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_FBCON_H
#define EMBER_FBCON_H

#include <stdint.h>
#include "../boot_info.h"

void fbcon_init(boot_info_v1_t * bi);
void fbcon_putc(char c);
void fbcon_write(const char *s);

#endif
