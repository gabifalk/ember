/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/ahci.h"
#include "ember/heap.h"
#include "ember/io.h"
#include "ember/mmu.h"
#include "ember/paging.h"
#include "ember/pmm.h"
#include "ember/console.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* PCI class code triple for AHCI controllers. */
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA      0x06
#define PCI_PROGIF_AHCI        0x01

static void
zero_mem(void *dst, uint64_t n)
{
	kmemzero(dst, n);
}

/* Error bits in PxIS worth checking. */
#define AHCI_PxIS_TFES (1u << 30)	/* Task File Error Status. */

/* Polling timeout (simple loop counter) */
#define AHCI_TIMEOUT 1000000

/* ---- Static state ---- */
static volatile ahci_hba_t *hba;
static volatile ahci_port_regs_t *active_port;
static int active_port_num;
static uint32_t ahci_sector_count;

/* DMA memory (allocated from PMM, accessed via HHDM) */
static ahci_cmd_header_t *cmd_list;
static ahci_recv_fis_t *recv_fis;
static ahci_cmd_table_t *cmd_table;
static uint64_t cmd_list_phys, recv_fis_phys, cmd_table_phys;

/*
 * DMA bounce buffer -- kernel BSS addresses can't be used for DMA directly
 * because virt_to_phys() only handles HHDM addresses, not kernel-base addresses.
 * We allocate a PMM page whose physical address is known.
 */
static uint8_t *bounce_buf;
static uint64_t bounce_phys;

/* ---- PCI helpers ---- */

static uint32_t
pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
	uint32_t addr = 0x80000000u | ((uint32_t) bus << 16)
	    | ((uint32_t) dev << 11)
	    | ((uint32_t) fn << 8)
	    | (off & 0xfc);
	outl(PCI_CONFIG_ADDR, addr);
	return inl(PCI_CONFIG_DATA);
}

static void
pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val)
{
	uint32_t addr = 0x80000000u | ((uint32_t) bus << 16)
	    | ((uint32_t) dev << 11)
	    | ((uint32_t) fn << 8)
	    | (off & 0xfc);
	outl(PCI_CONFIG_ADDR, addr);
	outl(PCI_CONFIG_DATA, val);
}

static uint16_t
pci_vendor_id(uint8_t bus, uint8_t dev, uint8_t fn)
{
	return (uint16_t) (pci_cfg_read32(bus, dev, fn, 0x00) & 0xffff);
}

/* ---- Debug printing ---- */

static void
console_hex32(uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	console_write("0x");
	for (int i = 28; i >= 0; i -= 4) {
		char c[2];
		c[0] = hex[(v >> i) & 0xf];
		c[1] = '\0';
		console_write(c);
	}
}

static void
console_dec(uint32_t v)
{
	char buf[12];
	int i = 0;
	if (v == 0) {
		console_write("0");
		return;
	}
	while (v > 0) {
		buf[i++] = '0' + (v % 10);
		v /= 10;
	}
	/* Reverse. */
	for (int j = i - 1; j >= 0; j--) {
		char c[2];
		c[0] = buf[j];
		c[1] = '\0';
		console_write(c);
	}
}

/* ---- AHCI command helpers ---- */

static void
ahci_build_h2d_fis(uint8_t * cfis, uint8_t command, uint64_t lba,
		   uint16_t count)
{
	zero_mem(cfis, 64);
	cfis[0] = FIS_TYPE_REG_H2D;	/* FIS type. */
	cfis[1] = 0x80;		/* Command bit set. */
	cfis[2] = command;	/* ATA command. */
	cfis[3] = 0;		/* Features low. */

	cfis[4] = (uint8_t) (lba & 0xFF);	/* LBA 7:0. */
	cfis[5] = (uint8_t) ((lba >> 8) & 0xFF);	/* LBA 15:8. */
	cfis[6] = (uint8_t) ((lba >> 16) & 0xFF);	/* LBA 23:16. */
	cfis[7] = 0x40;		/* Device: LBA mode. */

	cfis[8] = (uint8_t) ((lba >> 24) & 0xFF);	/* LBA 31:24. */
	cfis[9] = (uint8_t) ((lba >> 32) & 0xFF);	/* LBA 39:32. */
	cfis[10] = (uint8_t) ((lba >> 40) & 0xFF);	/* LBA 47:40. */
	cfis[11] = 0;		/* Features high. */

	cfis[12] = (uint8_t) (count & 0xFF);	/* Count low. */
	cfis[13] = (uint8_t) ((count >> 8) & 0xFF);	/* Count high. */
}

static int
ahci_issue_cmd(volatile ahci_port_regs_t * port, int slot)
{
	port->ci = (1u << slot);

	for (int i = 0; i < AHCI_TIMEOUT; i++) {
		if ((port->ci & (1u << slot)) == 0)
			return 0;
		if (port->is & AHCI_PxIS_TFES)
			return -1;
	}
	console_write("AHCI: command timeout\n");
	return -1;
}

static void
ahci_stop_cmd(volatile ahci_port_regs_t * port)
{
	/* Clear ST. */
	port->cmd &= ~AHCI_CMD_ST;
	/* Wait for CR to clear. */
	for (int i = 0; i < AHCI_TIMEOUT; i++) {
		if ((port->cmd & AHCI_CMD_CR) == 0)
			break;
	}
	/* Clear FRE. */
	port->cmd &= ~AHCI_CMD_FRE;
	/* Wait for FR to clear. */
	for (int i = 0; i < AHCI_TIMEOUT; i++) {
		if ((port->cmd & AHCI_CMD_FR) == 0)
			break;
	}
}

static void
ahci_start_cmd(volatile ahci_port_regs_t * port)
{
	/* Wait until CR is clear before starting. */
	for (int i = 0; i < AHCI_TIMEOUT; i++) {
		if ((port->cmd & AHCI_CMD_CR) == 0)
			break;
	}
	port->cmd |= AHCI_CMD_FRE;
	port->cmd |= AHCI_CMD_ST;
}

/* ---- IDENTIFY DEVICE ---- */

static int
ahci_identify(volatile ahci_port_regs_t * port)
{
	/* Allocate a temporary page for IDENTIFY data (512 bytes) */
	uint64_t id_phys = pmm_alloc_page();
	if (id_phys == 0)
		return -1;
	uint16_t *id_data = (uint16_t *) phys_to_virt(id_phys);
	zero_mem(id_data, PAGE_SIZE);

	/* Build command. */
	zero_mem(cmd_table, sizeof(*cmd_table));
	ahci_build_h2d_fis(cmd_table->cfis, ATA_CMD_IDENTIFY, 0, 0);

	/* PRDT: point to identify buffer. */
	cmd_table->prdt[0].dba = (uint32_t) (id_phys & 0xFFFFFFFF);
	cmd_table->prdt[0].dbau = (uint32_t) (id_phys >> 32);
	cmd_table->prdt[0].dbc = 512 - 1;	/* Byte count minus 1. */

	/* Command header: CFL=5 (5 dwords), prdtl=1. */
	cmd_list[0].flags = 5;	/* CFL = 5 dwords. */
	cmd_list[0].prdtl = 1;
	cmd_list[0].prdbc = 0;

	/* Clear interrupt status. */
	port->is = port->is;

	if (ahci_issue_cmd(port, 0) < 0) {
		console_write("AHCI: IDENTIFY failed\n");
		return -1;
	}

	/* Parse sector count: words 100-103 for LBA48, words 60-61 for LBA28. */
	uint64_t lba48 = (uint64_t) id_data[100]
	    | ((uint64_t) id_data[101] << 16)
	    | ((uint64_t) id_data[102] << 32)
	    | ((uint64_t) id_data[103] << 48);

	if (lba48 > 0) {
		ahci_sector_count = (uint32_t) lba48;	/* Truncate to 32-bit for now. */
	} else {
		ahci_sector_count =
		    (uint32_t) id_data[60] | ((uint32_t) id_data[61] << 16);
	}

	return 0;
}

/* ---- Probe ---- */

int
ahci_probe(void)
{
	uint8_t found_bus = 0, found_dev = 0, found_fn = 0;
	int found = 0;

	/* PCI scan: find AHCI controller. */
	for (uint16_t bus = 0; bus < 256 && !found; bus++) {
		for (uint8_t dev = 0; dev < 32 && !found; dev++) {
			for (uint8_t fn = 0; fn < 8 && !found; fn++) {
				if (pci_vendor_id((uint8_t) bus, dev, fn) ==
				    0xffff)
					continue;

				uint32_t classreg =
				    pci_cfg_read32((uint8_t) bus, dev, fn,
						   0x08);
				uint8_t class_code = (uint8_t) (classreg >> 24);
				uint8_t subclass = (uint8_t) (classreg >> 16);
				uint8_t prog_if = (uint8_t) (classreg >> 8);
				if (class_code != PCI_CLASS_MASS_STORAGE
				    || subclass != PCI_SUBCLASS_SATA
				    || prog_if != PCI_PROGIF_AHCI) {
					continue;
				}

				found_bus = (uint8_t) bus;
				found_dev = dev;
				found_fn = fn;
				found = 1;
			}
		}
	}

	if (!found)
		return -1;

	/* Enable PCI Bus Master. */
	uint32_t pci_cmd = pci_cfg_read32(found_bus, found_dev, found_fn, 0x04);
	pci_cmd |= (1u << 2);	/* Bus Master Enable. */
	pci_cfg_write32(found_bus, found_dev, found_fn, 0x04, pci_cmd);

	/* Map ABAR (BAR5) */
	uint32_t bar5 = pci_cfg_read32(found_bus, found_dev, found_fn, 0x24);
	if ((bar5 & 0x1u) != 0) {
		console_write("AHCI: BAR5 is I/O space; unsupported\n");
		return -1;
	}
	uint64_t abar_phys = (uint64_t) (bar5 & ~0x0fu);
	if (abar_phys == 0) {
		console_write("AHCI: ABAR is zero\n");
		return -1;
	}

	/* Map ABAR into HHDM. */
	{
		uint64_t abar_base = abar_phys & ~(PAGE_SIZE - 1);
		uint64_t pml4 = read_cr3() & PTE_ADDR_MASK;
		paging_map_range(pml4, HHDM_BASE + abar_base, abar_base,
				 8 * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
	}

	hba = (volatile ahci_hba_t *)phys_to_virt(abar_phys);

	/* Enable AHCI mode. */
	hba->ghc |= AHCI_GHC_AE;

	/* Scan ports for first SATA disk. */
	uint32_t pi = hba->pi;
	active_port = 0;
	active_port_num = -1;

	for (int p = 0; p < 32; p++) {
		if (!(pi & (1u << p)))
			continue;

		volatile ahci_port_regs_t *port = &hba->ports[p];
		uint32_t ssts = port->ssts;
		uint32_t det = ssts & AHCI_SSTS_DET_MASK;

		if (det != AHCI_SSTS_DET_PRESENT)
			continue;

		uint32_t sig = port->sig;
		if (sig != SATA_SIG_ATA)
			continue;

		active_port = port;
		active_port_num = p;
		break;
	}

	if (!active_port) {
		console_write("AHCI: no SATA disk found\n");
		return -1;
	}

	/* Stop command engine on selected port. */
	ahci_stop_cmd(active_port);

	/* Allocate DMA memory (3 single pages + 16 contiguous for bounce) */
	cmd_list_phys = pmm_alloc_page();
	recv_fis_phys = pmm_alloc_page();
	cmd_table_phys = pmm_alloc_page();
	bounce_phys = pmm_alloc_pages(16);
	if (cmd_list_phys == 0 || recv_fis_phys == 0 || cmd_table_phys == 0
	    || bounce_phys == 0) {
		console_write("AHCI: failed to allocate DMA memory\n");
		return -1;
	}

	cmd_list = (ahci_cmd_header_t *) phys_to_virt(cmd_list_phys);
	recv_fis = (ahci_recv_fis_t *) phys_to_virt(recv_fis_phys);
	cmd_table = (ahci_cmd_table_t *) phys_to_virt(cmd_table_phys);
	bounce_buf = (uint8_t *) phys_to_virt(bounce_phys);

	zero_mem(cmd_list, PAGE_SIZE);
	zero_mem((void *)recv_fis, PAGE_SIZE);
	zero_mem(cmd_table, PAGE_SIZE);

	/* Set port CLB/FB to physical addresses. */
	active_port->clb = (uint32_t) (cmd_list_phys & 0xFFFFFFFF);
	active_port->clbu = (uint32_t) (cmd_list_phys >> 32);
	active_port->fb = (uint32_t) (recv_fis_phys & 0xFFFFFFFF);
	active_port->fbu = (uint32_t) (recv_fis_phys >> 32);

	/* Clear errors and interrupt status. */
	active_port->serr = 0xFFFFFFFF;
	active_port->is = 0xFFFFFFFF;

	/* Set command header 0 to point to our command table. */
	cmd_list[0].ctba = (uint32_t) (cmd_table_phys & 0xFFFFFFFF);
	cmd_list[0].ctbau = (uint32_t) (cmd_table_phys >> 32);

	/* Start command engine. */
	ahci_start_cmd(active_port);

	/* Issue IDENTIFY DEVICE. */
	if (ahci_identify(active_port) < 0) {
		return -1;
	}

	/* Print result. */
	uint32_t size_mib = ahci_sector_count / 2048;
	console_write("AHCI: port ");
	console_dec((uint32_t) active_port_num);
	console_write(": SATA disk, ");
	console_dec(size_mib);
	console_write(" MiB\n");

	return 0;
}

/* ---- Read DMA EXT ---- */

int
ahci_read_blocks(uint32_t lba, uint8_t count, void *buf)
{
	if (!active_port || count == 0)
		return -1;

	/*
	 * Bounce buffer is 16 pages (65536 bytes = 128 sectors).
	 * Loop in chunks to support larger transfers.
	 */
	uint8_t *dst = (uint8_t *) buf;
	uint32_t remaining = count;
	uint32_t cur_lba = lba;

	while (remaining > 0) {
		uint8_t chunk = remaining > 128 ? 128 : (uint8_t) remaining;
		uint32_t bytes = (uint32_t) chunk * 512;

		zero_mem(cmd_table, sizeof(*cmd_table));
		ahci_build_h2d_fis(cmd_table->cfis, ATA_CMD_READ_DMA_EXT,
				   (uint64_t) cur_lba, chunk);

		cmd_table->prdt[0].dba = (uint32_t) (bounce_phys & 0xFFFFFFFF);
		cmd_table->prdt[0].dbau = (uint32_t) (bounce_phys >> 32);
		cmd_table->prdt[0].dbc = bytes - 1;

		cmd_list[0].flags = 5;
		cmd_list[0].prdtl = 1;
		cmd_list[0].prdbc = 0;

		active_port->is = active_port->is;

		if (ahci_issue_cmd(active_port, 0) < 0)
			return -1;

		kmemcpy(dst, bounce_buf, bytes);

		dst += bytes;
		cur_lba += chunk;
		remaining -= chunk;
	}

	return 0;
}

/* ---- Write DMA EXT ---- */

int
ahci_write_blocks(uint32_t lba, uint8_t count, const void *buf)
{
	if (!active_port || count == 0)
		return -1;

	const uint8_t *src = (const uint8_t *)buf;
	uint32_t remaining = count;
	uint32_t cur_lba = lba;

	while (remaining > 0) {
		uint8_t chunk = remaining > 128 ? 128 : (uint8_t) remaining;
		uint32_t bytes = (uint32_t) chunk * 512;

		kmemcpy(bounce_buf, src, bytes);

		zero_mem(cmd_table, sizeof(*cmd_table));
		ahci_build_h2d_fis(cmd_table->cfis, ATA_CMD_WRITE_DMA_EXT,
				   (uint64_t) cur_lba, chunk);

		cmd_table->prdt[0].dba = (uint32_t) (bounce_phys & 0xFFFFFFFF);
		cmd_table->prdt[0].dbau = (uint32_t) (bounce_phys >> 32);
		cmd_table->prdt[0].dbc = bytes - 1;

		cmd_list[0].flags = 5 | (1 << 6);
		cmd_list[0].prdtl = 1;
		cmd_list[0].prdbc = 0;

		active_port->is = active_port->is;

		if (ahci_issue_cmd(active_port, 0) < 0)
			return -1;

		src += bytes;
		cur_lba += chunk;
		remaining -= chunk;
	}

	return 0;
}

int
ahci_flush(void)
{
	if (!active_port)
		return -1;

	zero_mem(cmd_table, sizeof(*cmd_table));
	ahci_build_h2d_fis(cmd_table->cfis, ATA_CMD_FLUSH_EXT, 0, 0);

	cmd_list[0].flags = 5;
	cmd_list[0].prdtl = 0;
	cmd_list[0].prdbc = 0;

	active_port->is = active_port->is;

	return ahci_issue_cmd(active_port, 0);
}
