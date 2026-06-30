/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/ata.h"
#include "ember/io.h"
#include "ember/console.h"

/* Primary ATA channel registers. */
#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECT_CNT   0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALT_STATUS 0x3F6

/* Status bits. */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

/* Commands. */
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

static int ata_present;

static int
ata_wait_ready(void)
{
	for (int i = 0; i < 10000000; i++) {
		uint8_t s = inb(ATA_STATUS);
		if (!(s & ATA_SR_BSY))
			return 0;
	}
	return -1;
}

static int
ata_wait_drq(void)
{
	for (int i = 0; i < 10000000; i++) {
		uint8_t s = inb(ATA_STATUS);
		if (s & ATA_SR_ERR)
			return -1;
		if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ))
			return 0;
	}
	return -1;
}

static void
ata_400ns_delay(void)
{
	/* Read alt status 4 times (~400ns delay) */
	inb(ATA_ALT_STATUS);
	inb(ATA_ALT_STATUS);
	inb(ATA_ALT_STATUS);
	inb(ATA_ALT_STATUS);
}

int
ata_init(void)
{
	uint16_t identify[256];

	ata_present = 0;

	/* Select master drive. */
	outb(ATA_DRIVE_HEAD, 0xA0);
	ata_400ns_delay();

	/* Zero out registers. */
	outb(ATA_SECT_CNT, 0);
	outb(ATA_LBA_LO, 0);
	outb(ATA_LBA_MID, 0);
	outb(ATA_LBA_HI, 0);

	/* Send IDENTIFY. */
	outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
	ata_400ns_delay();

	uint8_t status = inb(ATA_STATUS);
	if (status == 0) {
		console_write("ATA: no drive on primary master\n");
		return -1;
	}

	/* Wait for BSY to clear. */
	if (ata_wait_ready() < 0) {
		console_write("ATA: timeout waiting for IDENTIFY\n");
		return -1;
	}

	/* Check for non-ATA (ATAPI/SATA) */
	if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) {
		console_write("ATA: not an ATA device\n");
		return -1;
	}

	/* Wait for DRQ. */
	if (ata_wait_drq() < 0) {
		console_write("ATA: IDENTIFY error\n");
		return -1;
	}

	/* Read identify data. */
	insw(ATA_DATA, identify, 256);

	/* LBA28 sector count from words 60-61. */
	uint32_t sectors =
	    (uint32_t) identify[60] | ((uint32_t) identify[61] << 16);
	console_write("ATA: primary master found, ");

	/* Print size in MiB. */
	uint32_t mib = sectors / 2048;
	char num[12];
	int pos = 0;
	if (mib == 0) {
		num[pos++] = '0';
	} else {
		uint32_t tmp = mib;
		char rev[12];
		int rp = 0;
		while (tmp > 0) {
			rev[rp++] = '0' + (char)(tmp % 10);
			tmp /= 10;
		}
		for (int i = rp - 1; i >= 0; i--)
			num[pos++] = rev[i];
	}
	num[pos] = '\0';
	console_write(num);
	console_write(" MiB\n");

	ata_present = 1;
	return 0;
}

int
ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
	if (!ata_present)
		return -1;

	if (ata_wait_ready() < 0)
		return -1;

	/* Select drive + LBA mode + top 4 bits of LBA. */
	outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
	outb(ATA_SECT_CNT, count);
	outb(ATA_LBA_LO, (uint8_t) (lba & 0xFF));
	outb(ATA_LBA_MID, (uint8_t) ((lba >> 8) & 0xFF));
	outb(ATA_LBA_HI, (uint8_t) ((lba >> 16) & 0xFF));

	/* Send READ SECTORS command. */
	outb(ATA_COMMAND, ATA_CMD_READ_PIO);

	uint16_t *p = (uint16_t *) buf;
	uint8_t n = count == 0 ? 0 : count;	/* Count=0 means 256 sectors, but we don't support that. */
	for (uint8_t i = 0; i < n; i++) {
		if (ata_wait_drq() < 0)
			return -1;
		insw(ATA_DATA, p, 256);
		p += 256;
	}

	return 0;
}

int
ata_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
	if (!ata_present)
		return -1;

	if (ata_wait_ready() < 0) {
		console_write("ATA: write wait_ready pre fail\n");
		return -1;
	}

	/* Select drive + LBA mode + top 4 bits of LBA. */
	outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
	outb(ATA_SECT_CNT, count);
	outb(ATA_LBA_LO, (uint8_t) (lba & 0xFF));
	outb(ATA_LBA_MID, (uint8_t) ((lba >> 8) & 0xFF));
	outb(ATA_LBA_HI, (uint8_t) ((lba >> 16) & 0xFF));

	/* Send WRITE SECTORS command. */
	outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);
	ata_400ns_delay();

	const uint16_t *p = (const uint16_t *)buf;
	uint8_t n = count == 0 ? 0 : count;
	for (uint8_t i = 0; i < n; i++) {
		if (ata_wait_drq() < 0) {
			console_write("ATA: write wait_drq fail\n");
			return -1;
		}
		outsw(ATA_DATA, p, 256);
		ata_400ns_delay();
		p += 256;
	}

	/* Wait for device to finish accepting last sector. */
	if (ata_wait_ready() < 0) {
		console_write("ATA: write wait_ready post fail\n");
		return -1;
	}

	return 0;
}

int
ata_flush(void)
{
	if (!ata_present)
		return -1;
	if (ata_wait_ready() < 0)
		return -1;
	outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
	if (ata_wait_ready() < 0) {
		console_write("ATA: flush fail\n");
		return -1;
	}
	return 0;
}
