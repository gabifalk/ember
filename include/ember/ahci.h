/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_AHCI_H
#define EMBER_AHCI_H

#include <stdint.h>

/* ---- HBA port registers (0x100 + port*0x80) ---- */
typedef struct {
	volatile uint32_t clb;	/* 0X00 Command List Base (low) */
	volatile uint32_t clbu;	/* 0X04 Command List Base (high) */
	volatile uint32_t fb;	/* 0X08 FIS Base (low) */
	volatile uint32_t fbu;	/* 0X0C FIS Base (high) */
	volatile uint32_t is;	/* 0X10 Interrupt Status. */
	volatile uint32_t ie;	/* 0X14 Interrupt Enable. */
	volatile uint32_t cmd;	/* 0X18 Command and Status. */
	volatile uint32_t reserved0;	/* 0X1C. */
	volatile uint32_t tfd;	/* 0X20 Task File Data. */
	volatile uint32_t sig;	/* 0X24 Signature. */
	volatile uint32_t ssts;	/* 0X28 SATA Status. */
	volatile uint32_t sctl;	/* 0X2C SATA Control. */
	volatile uint32_t serr;	/* 0X30 SATA Error. */
	volatile uint32_t sact;	/* 0X34 SATA Active. */
	volatile uint32_t ci;	/* 0X38 Command Issue. */
	volatile uint32_t sntf;	/* 0X3C SATA Notification. */
	volatile uint32_t fbs;	/* 0X40 FIS-based Switching. */
	volatile uint32_t reserved1[11];	/* 0X44-0x6F. */
	volatile uint32_t vendor[4];	/* 0X70-0x7F. */
} ahci_port_regs_t;

/* Full HBA memory layout. */
typedef struct {
	volatile uint32_t cap;	/* 0X00. */
	volatile uint32_t ghc;	/* 0X04. */
	volatile uint32_t is;	/* 0X08. */
	volatile uint32_t pi;	/* 0X0C. */
	volatile uint32_t vs;	/* 0X10. */
	volatile uint32_t ccc_ctl;	/* 0X14. */
	volatile uint32_t ccc_pts;	/* 0X18. */
	volatile uint32_t em_loc;	/* 0X1C. */
	volatile uint32_t em_ctl;	/* 0X20. */
	volatile uint32_t cap2;	/* 0X24. */
	volatile uint32_t bohc;	/* 0X28. */
	volatile uint8_t reserved[0x100 - 0x2C];
	ahci_port_regs_t ports[32];	/* 0X100+. */
} ahci_hba_t;

/* Command List entry (32 bytes) */
typedef struct {
	uint16_t flags;		/* CFL (bits 0-4), ATAPI, W, P, R, B, C, PMP. */
	uint16_t prdtl;		/* PRDT length (entries) */
	volatile uint32_t prdbc;	/* PRD Byte Count (set by HBA on completion) */
	uint32_t ctba;		/* Command Table Base (low) */
	uint32_t ctbau;		/* Command Table Base (high) */
	uint32_t reserved[4];
} __attribute__ ((packed)) ahci_cmd_header_t;

/* PRDT entry (16 bytes) */
typedef struct {
	uint32_t dba;		/* Data Base Address (low) */
	uint32_t dbau;		/* Data Base Address (high) */
	uint32_t reserved;
	uint32_t dbc;		/* Byte Count (bit 0 must be 1; max 4MB; bit 31 = interrupt) */
} __attribute__ ((packed)) ahci_prdt_entry_t;

/* Command Table (128-byte header + PRDTs) */
typedef struct {
	uint8_t cfis[64];
	uint8_t acmd[16];
	uint8_t reserved[48];
	ahci_prdt_entry_t prdt[8];	/* Enough for 8x4MB = 32MB max per command. */
} __attribute__ ((packed)) ahci_cmd_table_t;

/* Received FIS (256 bytes) */
typedef struct {
	uint8_t dsfis[28];	/* DMA Setup FIS. */
	uint8_t pad0[4];
	uint8_t psfis[20];	/* PIO Setup FIS. */
	uint8_t pad1[12];
	uint8_t rfis[20];	/* D2H Register FIS. */
	uint8_t pad2[4];
	uint8_t sdbfis[8];	/* Set Device Bits FIS. */
	uint8_t ufis[64];	/* Unknown FIS. */
	uint8_t reserved[96];
} __attribute__ ((packed)) ahci_recv_fis_t;

/* PxCMD bits. */
#define AHCI_CMD_ST   (1u << 0)	/* Start. */
#define AHCI_CMD_FRE  (1u << 4)	/* FIS Receive Enable. */
#define AHCI_CMD_FR   (1u << 14)	/* FIS Receive Running. */
#define AHCI_CMD_CR   (1u << 15)	/* Command List Running. */

/* PxTFD bits. */
#define AHCI_TFD_BSY  (1u << 7)
#define AHCI_TFD_DRQ  (1u << 3)
#define AHCI_TFD_ERR  (1u << 0)

/* PxSSTS DET field. */
#define AHCI_SSTS_DET_MASK    0x0F
#define AHCI_SSTS_DET_PRESENT 3	/* Device present + Phy communication established. */

/* SATA signature. */
#define SATA_SIG_ATA  0x00000101	/* SATA disk. */

/* GHC bits. */
#define AHCI_GHC_AE   (1u << 31)	/* AHCI Enable. */

/* FIS types. */
#define FIS_TYPE_REG_H2D 0x27

/* ATA commands. */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_EXT     0xEA

/* Public API. */
int ahci_probe(void);
int ahci_read_blocks(uint32_t lba, uint8_t count, void *buf);
int ahci_write_blocks(uint32_t lba, uint8_t count, const void *buf);
int ahci_flush(void);

#endif
