/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#pragma once

extern int test_failures;
extern int test_count;

void msg(const char *s);
void check(int ok, const char *name);
void print_int(int n);
void check_errno(long result, long expected_neg_errno, const char *name);
void test_done(void);
