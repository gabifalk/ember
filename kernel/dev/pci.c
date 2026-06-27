/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "ember/pci.h"
#include "ember/io.h"

uint32_t
pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
	uint32_t addr = 0x80000000u | ((uint32_t) bus << 16)
	    | ((uint32_t) dev << 11)
	    | ((uint32_t) fn << 8)
	    | (off & 0xfc);
	outl(PCI_CONFIG_ADDR, addr);
	return inl(PCI_CONFIG_DATA);
}

void
pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val)
{
	uint32_t addr = 0x80000000u | ((uint32_t) bus << 16)
	    | ((uint32_t) dev << 11)
	    | ((uint32_t) fn << 8)
	    | (off & 0xfc);
	outl(PCI_CONFIG_ADDR, addr);
	outl(PCI_CONFIG_DATA, val);
}

uint16_t
pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
	uint32_t v = pci_cfg_read32(bus, dev, fn, off & 0xfc);
	return (uint16_t) ((v >> ((off & 2) * 8)) & 0xffff);
}

void
pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val)
{
	uint32_t v = pci_cfg_read32(bus, dev, fn, off & 0xfc);
	uint32_t shift = (off & 2) * 8;
	v &= ~(0xffffu << shift);
	v |= ((uint32_t) val) << shift;
	pci_cfg_write32(bus, dev, fn, off & 0xfc, v);
}

static uint16_t
vendor_id(uint8_t bus, uint8_t dev, uint8_t fn)
{
	return (uint16_t) (pci_cfg_read32(bus, dev, fn, 0x00) & 0xffff);
}

pci_dev_t
pci_find_by_class(uint8_t class, uint8_t subclass, uint8_t progif)
{
	pci_dev_t r = { 0, 0, 0, 0 };
	for (uint16_t bus = 0; bus < 256; bus++) {
		for (uint8_t dev = 0; dev < 32; dev++) {
			for (uint8_t fn = 0; fn < 8; fn++) {
				if (vendor_id((uint8_t) bus, dev, fn) == 0xffff)
					continue;
				uint32_t cc = pci_cfg_read32((uint8_t) bus, dev, fn, 0x08);
				uint8_t pi = (uint8_t) ((cc >> 8) & 0xff);
				uint8_t sc = (uint8_t) ((cc >> 16) & 0xff);
				uint8_t cl = (uint8_t) ((cc >> 24) & 0xff);
				if (cl == class && sc == subclass && pi == progif) {
					r.bus = (uint8_t) bus;
					r.dev = dev;
					r.fn = fn;
					r.found = 1;
					return r;
				}
			}
		}
	}
	return r;
}

pci_dev_t
pci_find_by_id(uint16_t vendor, uint16_t device)
{
	pci_dev_t r = { 0, 0, 0, 0 };
	for (uint16_t bus = 0; bus < 256; bus++) {
		for (uint8_t dev = 0; dev < 32; dev++) {
			for (uint8_t fn = 0; fn < 8; fn++) {
				uint32_t id = pci_cfg_read32((uint8_t) bus, dev, fn, 0x00);
				if ((id & 0xffff) == 0xffff)
					continue;
				if ((id & 0xffff) == vendor
				    && ((id >> 16) & 0xffff) == device) {
					r.bus = (uint8_t) bus;
					r.dev = dev;
					r.fn = fn;
					r.found = 1;
					return r;
				}
			}
		}
	}
	return r;
}

uint64_t
pci_bar(pci_dev_t d, int bar, int *is_mmio)
{
	uint8_t off = (uint8_t) (0x10 + bar * 4);
	uint32_t v = pci_cfg_read32(d.bus, d.dev, d.fn, off);
	if (v & 0x1) {		/* I/O space BAR. */
		if (is_mmio)
			*is_mmio = 0;
		return (uint64_t) (v & ~0x3u);
	}
	if (is_mmio)
		*is_mmio = 1;
	uint8_t type = (uint8_t) ((v >> 1) & 0x3);
	uint64_t base = (uint64_t) (v & ~0xFu);
	if (type == 0x2) {	/* 64-bit BAR: high dword in next slot. */
		uint32_t hi = pci_cfg_read32(d.bus, d.dev, d.fn, (uint8_t) (off + 4));
		base |= ((uint64_t) hi) << 32;
	}
	return base;
}

void
pci_enable_bus_master(pci_dev_t d)
{
	uint16_t cmd = pci_cfg_read16(d.bus, d.dev, d.fn, 0x04);
	cmd |= (1 << 2);	/* Bus Master Enable. */
	cmd |= (1 << 1);	/* Memory Space Enable - intentional in this generic helper;
				 * harmless for AHCI whose MMIO decode is already enabled by firmware. */
	pci_cfg_write16(d.bus, d.dev, d.fn, 0x04, cmd);
}

uint8_t
pci_interrupt_line(pci_dev_t d)
{
	return (uint8_t) (pci_cfg_read32(d.bus, d.dev, d.fn, 0x3c) & 0xff);
}

uint8_t
pci_interrupt_pin(pci_dev_t d)
{
	return (uint8_t) ((pci_cfg_read32(d.bus, d.dev, d.fn, 0x3c) >> 8) & 0xff);
}
