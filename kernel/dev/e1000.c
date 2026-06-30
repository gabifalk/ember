/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/e1000.h"
#include "ember/netdev.h"
#include "ember/net/ipv4.h"
#include "ember/pci.h"
#include "ember/irq.h"
#include "ember/pmm.h"
#include "ember/mmu.h"
#include "ember/paging.h"
#include "ember/lapic.h"
#include "ember/vectors.h"
#include "ember/console.h"

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_RST  (1u << 26)
#define CTRL_SLU  (1u << 6)
#define CTRL_ASDE (1u << 5)
#define RCTL_EN   (1u << 1)
#define RCTL_BAM  (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)
#define IMS_RXT0  (1u << 7)

#define RX_DESCS 32
#define TX_DESCS 8
#define RX_BUF   2048
#define TX_BUF   2048

struct rx_desc { uint64_t addr; uint16_t length; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

#define RXD_DD  (1u << 0)
#define TXD_DD  (1u << 0)
#define TXD_EOP (1u << 0)
#define TXD_IFCS (1u << 1)
#define TXD_RS  (1u << 3)

static volatile uint8_t *mmio;
static volatile struct rx_desc *rx_ring;
static volatile struct tx_desc *tx_ring;
static uint64_t rx_ring_phys, tx_ring_phys;
static uint8_t *rx_buf[RX_DESCS];
static uint8_t *tx_buf[TX_DESCS];
static uint64_t rx_buf_phys[RX_DESCS], tx_buf_phys[TX_DESCS];
static uint16_t rx_head;      /* next descriptor the driver inspects */
static uint16_t tx_tail;      /* next TX descriptor to fill */
static uint16_t tx_reclaim;   /* oldest in-flight descriptor index */
static netdev_t e1000_netdev;

static inline uint32_t mmio_r(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static inline void mmio_w(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }

static void
read_mac(uint8_t mac[6])
{
	/* QEMU initializes RAL/RAH from the configured MAC. */
	uint32_t ral = mmio_r(REG_RAL);
	uint32_t rah = mmio_r(REG_RAH);
	mac[0] = ral & 0xff; mac[1] = (ral >> 8) & 0xff;
	mac[2] = (ral >> 16) & 0xff; mac[3] = (ral >> 24) & 0xff;
	mac[4] = rah & 0xff; mac[5] = (rah >> 8) & 0xff;
}

static void
tx_reclaim_done(void)
{
	while (tx_reclaim != tx_tail && (tx_ring[tx_reclaim].status & TXD_DD)) {
		tx_ring[tx_reclaim].cmd = 0;
		tx_reclaim = (tx_reclaim + 1) % TX_DESCS;
	}
}

static int
e1000_transmit(netdev_t *nd, const void *frame, uint16_t len)
{
	(void)nd;
	if (len > TX_BUF)
		return -1;
	tx_reclaim_done();
	uint16_t next = (tx_tail + 1) % TX_DESCS;
	if (next == tx_reclaim)        /* ring full: one slot reserved so TDT != TDH-as-empty */
		return -1;
	for (uint16_t i = 0; i < len; i++)
		tx_buf[tx_tail][i] = ((const uint8_t *)frame)[i];
	tx_ring[tx_tail].addr = tx_buf_phys[tx_tail];
	tx_ring[tx_tail].length = len;
	tx_ring[tx_tail].cso = 0;
	tx_ring[tx_tail].cmd = TXD_EOP | TXD_IFCS | TXD_RS;
	tx_ring[tx_tail].status = 0;
	tx_tail = next;
	mmio_w(REG_TDT, tx_tail);
	return 0;
}

static void
e1000_drain_rx(void)
{
	while (rx_ring[rx_head].status & RXD_DD) {
		volatile struct rx_desc *d = &rx_ring[rx_head];
		uint16_t len = d->length;
		netdev_deliver_rx(&e1000_netdev, rx_buf[rx_head], len);
		d->status = 0;
		uint16_t old = rx_head;
		rx_head = (rx_head + 1) % RX_DESCS;
		mmio_w(REG_RDT, old);
	}
}

static void
e1000_irq(int vector)
{
	(void)vector;
	uint32_t icr = mmio_r(REG_ICR);   /* read clears the cause */
	if (!(icr & IMS_RXT0))
		return;
	e1000_drain_rx();
}

static void
map_bar(uint64_t bar_phys)
{
	uint64_t base = bar_phys & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t pml4 = read_cr3() & PTE_ADDR_MASK;
	paging_map_range(pml4, HHDM_BASE + base, base, 16 * PAGE_SIZE,
			 PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
	mmio = (volatile uint8_t *)phys_to_virt(bar_phys);
}

void
e1000_init(void)
{
	pci_dev_t pd = pci_find_by_id(E1000_VENDOR, E1000_DEVICE);
	if (!pd.found)
		return;
	int is_mmio;
	uint64_t bar0 = pci_bar(pd, 0, &is_mmio);
	if (!is_mmio || bar0 == 0)
		return;
	pci_enable_bus_master(pd);
	map_bar(bar0);

	/* Reset. */
	mmio_w(REG_IMC, 0xffffffff);
	mmio_w(REG_CTRL, mmio_r(REG_CTRL) | CTRL_RST);
	for (volatile int i = 0; i < 100000; i++) { }
	mmio_w(REG_IMC, 0xffffffff);
	mmio_w(REG_CTRL, mmio_r(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

	/* Clear the Multicast Table Array so stale filter bits do not survive reset. */
	for (int i = 0; i < 128; i++)
		mmio_w(REG_MTA + i * 4, 0);

	read_mac(e1000_netdev.mac);
	netdev_add_addr(&e1000_netdev, OUR_IP);

	/* RX ring + buffers. */
	rx_ring_phys = pmm_alloc_page();
	rx_ring = (volatile struct rx_desc *)phys_to_virt(rx_ring_phys);
	for (int i = 0; i < RX_DESCS; i++) {
		rx_buf_phys[i] = pmm_alloc_page();
		rx_buf[i] = (uint8_t *)phys_to_virt(rx_buf_phys[i]);
		rx_ring[i].addr = rx_buf_phys[i];
		rx_ring[i].status = 0;
	}
	mmio_w(REG_RDBAL, (uint32_t)rx_ring_phys);
	mmio_w(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
	mmio_w(REG_RDLEN, RX_DESCS * 16);
	mmio_w(REG_RDH, 0);
	mmio_w(REG_RDT, RX_DESCS - 1);
	rx_head = 0;
	mmio_w(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

	/* TX ring + buffers. */
	tx_ring_phys = pmm_alloc_page();
	tx_ring = (volatile struct tx_desc *)phys_to_virt(tx_ring_phys);
	for (int i = 0; i < TX_DESCS; i++) {
		tx_buf_phys[i] = pmm_alloc_page();
		tx_buf[i] = (uint8_t *)phys_to_virt(tx_buf_phys[i]);
		tx_ring[i].addr = 0;
		tx_ring[i].cmd = 0;
		tx_ring[i].status = TXD_DD;   /* mark free */
	}
	mmio_w(REG_TDBAL, (uint32_t)tx_ring_phys);
	mmio_w(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
	mmio_w(REG_TDLEN, TX_DESCS * 16);
	mmio_w(REG_TDH, 0);
	mmio_w(REG_TDT, 0);
	tx_tail = 0;
	tx_reclaim = 0;
	mmio_w(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
	mmio_w(REG_TIPG, 0x0060200A);

	/* Register the netdev and route the IRQ to the BSP. */
	e1000_netdev.name = "e1000";
	e1000_netdev.transmit = e1000_transmit;
	e1000_netdev.rx_handler = 0;
	netdev_register(&e1000_netdev);

	uint8_t gsi = pci_interrupt_line(pd);
	console_write("e1000: irq gsi=");
	console_hex64((uint64_t)gsi);
	console_write("\n");
	irq_route_pci(gsi, VEC_E1000, (uint8_t)lapic_id(), e1000_irq);

	/* Enable the receive interrupt. */
	mmio_w(REG_IMS, IMS_RXT0);
	(void)mmio_r(REG_ICR);   /* clear any pending */

	console_write("e1000: up\n");
}
