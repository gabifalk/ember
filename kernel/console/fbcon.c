/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "ember/fbcon.h"
#include "ember/mmu.h"

#define FONT_W 8
#define FONT_H 8
#define CELL_H 16

static uint32_t *fb;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_stride;
static uint32_t fb_format;

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t cols;
static uint32_t rows;

static uint32_t fg_color = 0x00FFFFFF;
static uint32_t bg_color = 0x00000000;

/* 8X8 basic font for digits and uppercase letters; lowercase maps to uppercase. */
static const uint8_t glyph_space[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t glyph_qmark[8] =
    { 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00 };

static const uint8_t glyph_0[8] =
    { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_1[8] =
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 };
static const uint8_t glyph_2[8] =
    { 0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00 };
static const uint8_t glyph_3[8] =
    { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_4[8] =
    { 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 };
static const uint8_t glyph_5[8] =
    { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_6[8] =
    { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_7[8] =
    { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 };
static const uint8_t glyph_8[8] =
    { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_9[8] =
    { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 };

static const uint8_t glyph_A[8] =
    { 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00 };
static const uint8_t glyph_B[8] =
    { 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 };
static const uint8_t glyph_C[8] =
    { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_D[8] =
    { 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 };
static const uint8_t glyph_E[8] =
    { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 };
static const uint8_t glyph_F[8] =
    { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 };
static const uint8_t glyph_G[8] =
    { 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_H[8] =
    { 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 };
static const uint8_t glyph_I[8] =
    { 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 };
static const uint8_t glyph_J[8] =
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00 };
static const uint8_t glyph_K[8] =
    { 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00 };
static const uint8_t glyph_L[8] =
    { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 };
static const uint8_t glyph_M[8] =
    { 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00 };
static const uint8_t glyph_N[8] =
    { 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00 };
static const uint8_t glyph_O[8] =
    { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_P[8] =
    { 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 };
static const uint8_t glyph_Q[8] =
    { 0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00 };
static const uint8_t glyph_R[8] =
    { 0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00 };
static const uint8_t glyph_S[8] =
    { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_T[8] =
    { 0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 };
static const uint8_t glyph_U[8] =
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 };
static const uint8_t glyph_V[8] =
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 };
static const uint8_t glyph_W[8] =
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 };
static const uint8_t glyph_X[8] =
    { 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 };
static const uint8_t glyph_Y[8] =
    { 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x3C, 0x00 };
static const uint8_t glyph_Z[8] =
    { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00 };

static const uint8_t glyph_dot[8] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 };
static const uint8_t glyph_colon[8] =
    { 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00 };
static const uint8_t glyph_dash[8] =
    { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t glyph_underscore[8] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00 };
static const uint8_t glyph_slash[8] =
    { 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00 };

static const uint8_t *
get_glyph(char c)
{
	if (c == ' ')
		return glyph_space;
	if (c == '?')
		return glyph_qmark;
	if (c == '.')
		return glyph_dot;
	if (c == ':')
		return glyph_colon;
	if (c == '-')
		return glyph_dash;
	if (c == '_')
		return glyph_underscore;
	if (c == '/')
		return glyph_slash;

	if (c >= '0' && c <= '9') {
		static const uint8_t *digits[10] = {
			glyph_0, glyph_1, glyph_2, glyph_3, glyph_4,
			glyph_5, glyph_6, glyph_7, glyph_8, glyph_9
		};
		return digits[c - '0'];
	}

	if (c >= 'a' && c <= 'z')
		c -= 32;
	if (c >= 'A' && c <= 'Z') {
		static const uint8_t *letters[26] = {
			glyph_A, glyph_B, glyph_C, glyph_D, glyph_E, glyph_F,
			    glyph_G,
			glyph_H, glyph_I, glyph_J, glyph_K, glyph_L, glyph_M,
			    glyph_N,
			glyph_O, glyph_P, glyph_Q, glyph_R, glyph_S, glyph_T,
			    glyph_U,
			glyph_V, glyph_W, glyph_X, glyph_Y, glyph_Z
		};
		return letters[c - 'A'];
	}

	return glyph_qmark;
}

static inline uint32_t
pack_color(uint8_t r, uint8_t g, uint8_t b)
{
	if (fb_format == 0) {	/* RGBX. */
		return ((uint32_t) r) | ((uint32_t) g << 8) | ((uint32_t) b <<
							       16);
	}
	return ((uint32_t) b) | ((uint32_t) g << 8) | ((uint32_t) r << 16);	/* BGRX. */
}

static void
fbcon_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
	fb[y * fb_stride + x] = color;
}

static void
fbcon_scroll(void)
{
	uint32_t row_bytes = fb_stride * 4;
	uint32_t scroll_px = CELL_H;
	uint32_t total_rows = fb_height;
	uint8_t *dst = (uint8_t *) fb;
	uint8_t *src = (uint8_t *) fb + scroll_px * row_bytes;
	uint32_t copy_bytes = (total_rows - scroll_px) * row_bytes;

	for (uint32_t i = 0; i < copy_bytes; i++) {
		dst[i] = src[i];
	}

	uint32_t clear_start = (total_rows - scroll_px) * row_bytes;
	uint32_t clear_bytes = scroll_px * row_bytes;
	for (uint32_t i = 0; i < clear_bytes; i += 4) {
		*(uint32_t *) (dst + clear_start + i) = bg_color;
	}
}

void
fbcon_init(boot_info_v1_t * bi)
{
	if (!bi || !bi->fb_phys_base || bi->fb_bpp != 32) {
		fb = NULL;
		return;
	}
	fb = (uint32_t *) phys_to_virt(bi->fb_phys_base);
	fb_width = bi->fb_width;
	fb_height = bi->fb_height;
	fb_stride = bi->fb_stride_pixels;
	fb_format = bi->fb_format;

	cols = fb_width / FONT_W;
	rows = fb_height / CELL_H;
	cursor_x = 0;
	cursor_y = 0;

	fg_color = pack_color(0xFF, 0xFF, 0xFF);
	bg_color = pack_color(0x00, 0x00, 0x00);
}

void
fbcon_disable(void)
{
	fb = NULL;
}

void
fbcon_putc(char c)
{
	if (!fb)
		return;

	if (c == '\n') {
		cursor_x = 0;
		cursor_y++;
	} else if (c == '\r') {
		cursor_x = 0;
	} else {
		const uint8_t *g = get_glyph(c);
		uint32_t px = cursor_x * FONT_W;
		uint32_t py = cursor_y * CELL_H;

		for (uint32_t row = 0; row < FONT_H; row++) {
			uint8_t bits = g[row];
			for (uint32_t col = 0; col < FONT_W; col++) {
				uint32_t color =
				    (bits & (1u << (7 - col))) ? fg_color :
				    bg_color;
				fbcon_put_pixel(px + col, py + row * 2, color);
				fbcon_put_pixel(px + col, py + row * 2 + 1,
						color);
			}
		}

		cursor_x++;
		if (cursor_x >= cols) {
			cursor_x = 0;
			cursor_y++;
		}
	}

	if (cursor_y >= rows) {
		fbcon_scroll();
		cursor_y = rows - 1;
	}
}

void
fbcon_write(const char *s)
{
	if (!s)
		return;
	while (*s) {
		fbcon_putc(*s++);
	}
}
