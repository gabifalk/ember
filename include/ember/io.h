/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_IO_H
#define EMBER_IO_H

#include <stdint.h>

static inline void
outb(uint16_t port, uint8_t val)
{
	__asm__ __volatile__("outb %0, %1"::"a"(val), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
	uint8_t ret;
	__asm__ __volatile__("inb %1, %0":"=a"(ret):"Nd"(port));
	return ret;
}

static inline void
outw(uint16_t port, uint16_t val)
{
	__asm__ __volatile__("outw %0, %1"::"a"(val), "Nd"(port));
}

static inline uint16_t
inw(uint16_t port)
{
	uint16_t ret;
	__asm__ __volatile__("inw %1, %0":"=a"(ret):"Nd"(port));
	return ret;
}

static inline void
outl(uint16_t port, uint32_t val)
{
	__asm__ __volatile__("outl %0, %1"::"a"(val), "Nd"(port));
}

static inline uint32_t
inl(uint16_t port)
{
	uint32_t ret;
	__asm__ __volatile__("inl %1, %0":"=a"(ret):"Nd"(port));
	return ret;
}

static inline void
insw(uint16_t port, void *buf, uint32_t count)
{
	__asm__ __volatile__("rep insw":"+D"(buf), "+c"(count)
			     :"d"(port)
			     :"memory");
}

static inline void
outsw(uint16_t port, const void *buf, uint32_t count)
{
	__asm__ __volatile__("rep outsw":"+S"(buf), "+c"(count)
			     :"d"(port)
			     :"memory");
}

#endif
