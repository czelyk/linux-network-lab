/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nic.h - shared definitions for the "edunic" educational PCIe NIC driver.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS
 * ---------------------------------------------------------------------------
 * edunic is a from-scratch Linux PCIe Ethernet driver written purely for
 * learning kernel networking internals. It is NOT a driver for any real,
 * purchasable network card. There is no "edunic" silicon.
 *
 * Every kernel-facing API used by this driver (pci_driver, net_device_ops,
 * DMA API, NAPI, IRQ handling, etc.) is 100% real and used the way a real
 * driver would use it. What is emulated is the *hardware itself*: the
 * "chip" behind the MMIO BAR is a small software model implemented in
 * nic_hw.c that reacts to register writes the way a real Ethernet MAC/DMA
 * engine would (moves descriptor ownership, raises interrupts, "transmits"
 * onto a loopback wire). See the big comment at the top of nic_hw.c for the
 * full explanation of the emulation model and how to exercise it.
 *
 * The PCI Vendor/Device ID pair below is a placeholder that does not belong
 * to any real, allocated PCI-SIG vendor. It will never match a physical PCI
 * device on real hardware. To actually bind this driver to something, you
 * need a matching virtual PCI device (e.g. a QEMU pci-testdev-style model)
 * or you drive the code paths directly via the emulation harness described
 * in the README. This is intentional and documented, per the project goal
 * of "educational correctness and architectural realism" rather than
 * pretending to support real silicon.
 */

#ifndef _EDUNIC_H_
#define _EDUNIC_H_

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/u64_stats_sync.h>

#define EDUNIC_DRV_NAME		"edunic"
#define EDUNIC_DRV_DESC		"Educational PCIe NIC driver (software-emulated hardware)"

/* ===========================================================================
 * PCI IDENTIFICATION -- PLACEHOLDER, NOT A REAL VENDOR/DEVICE
 * ===========================================================================
 * 0xFEED / 0xC0DE are not PCI-SIG assigned IDs. They are chosen deliberately
 * so this table can never accidentally claim a real device on a real
 * machine.
 */
#define EDUNIC_VENDOR_ID	0xFEED	/* placeholder -- NOT a real PCI-SIG vendor ID */
#define EDUNIC_DEVICE_ID	0xC0DE	/* placeholder -- "edunic rev A" */

/* ===========================================================================
 * REGISTER MAP (BAR0, MMIO) -- HARDWARE-SPECIFIC PLACEHOLDER
 * ===========================================================================
 * This layout is *invented for this project*. It is loosely modeled on the
 * shape of real legacy-descriptor NICs (e.g. Intel e1000-style ring control
 * registers: base/length/head/tail per ring, ICR/IMS interrupt registers)
 * because that shape is genuinely representative of how a huge fraction of
 * real Ethernet MACs work. But the exact offsets and bit numbers below are
 * NOT from any real datasheet. Every register access goes through readl()/
 * writel() via the accessors in nic_hw.c -- never through a raw volatile
 * pointer dereference. See the comment block above edunic_reg_read() in
 * nic_hw.c for why.
 */
#define EDUNIC_BAR		0		/* BAR0 holds the register file */
#define EDUNIC_BAR_LEN		0x2000		/* 8 KiB of MMIO register space */

/* --- Global control / status -------------------------------------------- */
#define EDUNIC_REG_CTRL		0x0000	/* Device control */
#define   EDUNIC_CTRL_RST	  BIT(0)	/* Software reset (self-clearing) */
#define   EDUNIC_CTRL_TXEN	  BIT(1)	/* Enable TX DMA engine */
#define   EDUNIC_CTRL_RXEN	  BIT(2)	/* Enable RX DMA engine */

#define EDUNIC_REG_STATUS	0x0004	/* Device status (read-only) */
#define   EDUNIC_STATUS_LINKUP	  BIT(0)	/* Link is up */
#define   EDUNIC_STATUS_RST_DONE  BIT(1)	/* Reset has completed */

/* --- MAC station address (loaded into the device at init) --------------- */
#define EDUNIC_REG_MAC_LOW	0x0010	/* bytes 0..3 of the MAC address */
#define EDUNIC_REG_MAC_HIGH	0x0014	/* bytes 4..5 of the MAC address */

/* --- TX ring control ------------------------------------------------------
 * TDBAL/TDBAH: 64-bit DMA (bus) address of the TX descriptor ring, split
 *              into low/high 32-bit halves the way real ring-based NICs
 *              program a 64-bit base address through two 32-bit MMIO
 *              registers.
 * TDLEN:       number of descriptors in the ring.
 * TDH:         hardware-owned "head" -- next descriptor the device will
 *              fetch/consume. Software must never write TDH.
 * TDT:         software-owned "tail" -- the doorbell. Software advances
 *              TDT to hand new descriptors to the device.
 */
#define EDUNIC_REG_TDBAL	0x0100
#define EDUNIC_REG_TDBAH	0x0104
#define EDUNIC_REG_TDLEN	0x0108
#define EDUNIC_REG_TDH		0x010C
#define EDUNIC_REG_TDT		0x0110

/* --- RX ring control (mirrors TX) ---------------------------------------- */
#define EDUNIC_REG_RDBAL	0x0200
#define EDUNIC_REG_RDBAH	0x0204
#define EDUNIC_REG_RDLEN	0x0208
#define EDUNIC_REG_RDH		0x020C
#define EDUNIC_REG_RDT		0x0210

/* --- Interrupt handling ---------------------------------------------------
 * ICR: Interrupt Cause Read. Reading it returns the set of pending causes
 *      and (on this model) clears them, exactly like real NICs such as
 *      e1000 do -- "read to clear" is a deliberate hardware idiom that
 *      avoids a separate ack write on the hot path.
 * ICS: Interrupt Cause Set -- software/emulation-only knob used by the
 *      emulated "chip" in nic_hw.c to raise a cause; a real NIC would not
 *      expose this to the driver, it's here purely so the software model
 *      has a place to inject RX/TX/LSC events.
 * IMS: Interrupt Mask Set -- OR bits in to unmask (enable) causes.
 * IMC: Interrupt Mask Clear -- OR bits in to mask (disable) causes.
 */
#define EDUNIC_REG_ICR		0x0300
#define EDUNIC_REG_ICS		0x0304
#define EDUNIC_REG_IMS		0x0308
#define EDUNIC_REG_IMC		0x030C

#define   EDUNIC_INT_TX		  BIT(0)	/* TX descriptor(s) completed */
#define   EDUNIC_INT_RX		  BIT(1)	/* RX descriptor(s) ready */
#define   EDUNIC_INT_LSC	  BIT(2)	/* Link status change */
#define   EDUNIC_INT_ALL	  (EDUNIC_INT_TX | EDUNIC_INT_RX | EDUNIC_INT_LSC)

/* ===========================================================================
 * HARDWARE DESCRIPTORS
 * ===========================================================================
 * These structures are laid out exactly as "the wire format the device DMA
 * engine understands". A real NIC's DMA engine walks these bytes directly,
 * so they may contain *only* things silicon can understand: a DMA (bus)
 * address, a length, and command/status/ownership bits. They must NEVER
 * contain a kernel pointer (struct sk_buff *, void *, etc.) -- the device
 * has no concept of kernel virtual addresses.
 *
 * All multi-byte fields are little-endian (__le*) because that is what the
 * overwhelming majority of real PCIe NICs use on the wire for their control
 * structures, and using __leNN types (instead of plain u32/u16) documents
 * that endianness conversion (cpu_to_le32/le32_to_cpu) is required, which
 * sparse and the kernel's endianness checkers will enforce.
 *
 * Both descriptors are 16 bytes, a very common real-world descriptor size
 * (e.g. legacy e1000 descriptors are also 16 bytes).
 */
struct edunic_tx_desc {
	__le64	buffer_addr;	/* DMA address of the packet buffer */
	__le16	length;		/* length of data in the buffer */
	u8	cso;		/* checksum offset placeholder (unused by model) */
	u8	cmd;		/* command bits, see EDUNIC_TXD_CMD_* */
	u8	status;		/* status bits, see EDUNIC_TXD_STAT_* */
	u8	css;		/* checksum start placeholder (unused by model) */
	__le16	special;	/* VLAN tag placeholder (unused by model) */
} __packed;

#define EDUNIC_TXD_CMD_EOP	BIT(0)	/* End Of Packet */
#define EDUNIC_TXD_CMD_RS	BIT(1)	/* Report Status when done */
#define EDUNIC_TXD_STAT_DD	BIT(0)	/* Descriptor Done -- device -> driver ownership */

struct edunic_rx_desc {
	__le64	buffer_addr;	/* DMA address of the receive buffer */
	__le16	length;		/* length of data written by the device */
	__le16	checksum;	/* placeholder hw checksum result */
	u8	status;		/* status bits, see EDUNIC_RXD_STAT_* */
	u8	errors;		/* error bits, see EDUNIC_RXD_ERR_* */
	__le16	special;	/* VLAN tag placeholder (unused by model) */
} __packed;

#define EDUNIC_RXD_STAT_DD	BIT(0)	/* Descriptor Done -- device -> driver ownership */
#define EDUNIC_RXD_STAT_EOP	BIT(1)	/* End Of Packet (single-buffer packets: always set) */
#define EDUNIC_RXD_STAT_CSUM_VALID BIT(2) /* HW checksum offload: L3/L4 checksum verified good */
#define EDUNIC_RXD_ERR_RXE	BIT(0)	/* Generic RX error */

/* Ring geometry. Must be a power of two so index wraparound is a cheap
 * bitmask instead of a modulo. 256 mirrors common real-world default ring
 * sizes (e.g. ethtool -g on many NICs defaults RX/TX to 256-512).
 */
#define EDUNIC_RING_SIZE	256
#define EDUNIC_RING_MASK	(EDUNIC_RING_SIZE - 1)

/* Fixed per-buffer size for the RX ring. 2KiB comfortably holds a standard
 * 1500-byte-MTU Ethernet frame (1518 bytes incl. header, no FCS) plus
 * NET_IP_ALIGN slack. Real jumbo-frame-capable NICs use larger buffers or
 * scatter-gather across multiple descriptors -- see README for discussion.
 */
#define EDUNIC_RX_BUF_SIZE	2048

/*
 * This model uses one fixed-size (EDUNIC_RX_BUF_SIZE) buffer per RX
 * descriptor and never chains descriptors for a single packet (no RX
 * scatter-gather). That caps the largest frame it can receive at just
 * under EDUNIC_RX_BUF_SIZE. Real jumbo-frame-capable NICs either use
 * larger single buffers (wasteful for small packets) or, more commonly,
 * chain multiple descriptors per packet -- see the "MTU / Jumbo Frames"
 * section of the README for why that is deliberately not implemented
 * here: it would roughly double the bookkeeping in nic_rx.c for a detail
 * that is orthogonal to what this project is trying to teach. We stick to
 * the standard 1500-byte Ethernet MTU, which comfortably fits.
 */
#define EDUNIC_MAX_MTU		ETH_DATA_LEN	/* 1500, see comment above */
#define EDUNIC_MIN_MTU		ETH_MIN_MTU

/* ===========================================================================
 * SOFTWARE-SIDE METADATA (never touched by "hardware")
 * ===========================================================================
 * These live in normal kernel memory, parallel to (but separate from) the
 * hardware descriptor arrays, indexed by the same ring index. This is the
 * standard split used by essentially every real NIC driver: the hardware
 * descriptor is the wire contract with silicon, the software metadata is
 * the driver's own bookkeeping (which skb owns this slot, what DMA address
 * was handed to the device so it can be unmapped later, etc).
 */
struct edunic_tx_buf {
	struct sk_buff	*skb;
	dma_addr_t	dma;
	size_t		len;
};

struct edunic_rx_buf {
	struct sk_buff	*skb;
	dma_addr_t	dma;
	size_t		len;
};

/* A ring bundles the coherent (DMA-able) descriptor array with the parallel
 * software metadata array and the head/tail bookkeeping the driver needs.
 */
struct edunic_tx_ring {
	struct edunic_tx_desc	*desc;		/* CPU virtual addr, coherent DMA memory */
	dma_addr_t		dma;		/* DMA (bus) address of desc[] */
	struct edunic_tx_buf	*buf;		/* parallel software metadata, kzalloc'd */
	u16			count;		/* number of descriptors (EDUNIC_RING_SIZE) */
	u16			next_to_use;	/* next slot the driver will fill */
	u16			next_to_clean;	/* next slot to reclaim after completion */
};

struct edunic_rx_ring {
	struct edunic_rx_desc	*desc;
	dma_addr_t		dma;
	struct edunic_rx_buf	*buf;
	u16			count;
	u16			next_to_use;	/* next slot to refill and hand to hw */
	u16			next_to_clean;	/* next slot to check for DD/harvest */
};

/* Simple per-adapter stats, updated with u64_stats_sync so 32-bit reads from
 * ndo_get_stats64() stay consistent with concurrent 64-bit updates on other
 * CPUs, exactly as real drivers do.
 */
struct edunic_stats {
	struct u64_stats_sync	syncp;
	u64			tx_packets;
	u64			tx_bytes;
	u64			tx_errors;
	u64			rx_packets;
	u64			rx_bytes;
	u64			rx_errors;
	u64			rx_dropped;
};

/* ===========================================================================
 * PER-DEVICE PRIVATE DATA
 * ===========================================================================
 * This is the netdev_priv() blob: everything the driver needs to remember
 * about one PCI function / one net_device instance.
 */
struct edunic_adapter {
	struct net_device	*netdev;
	struct pci_dev		*pdev;

	void __iomem		*hw_addr;	/* ioremap'd BAR0 */

	struct edunic_tx_ring	tx_ring;
	struct edunic_rx_ring	rx_ring;

	struct napi_struct	napi;

	int			irq;
	bool			msi_enabled;

	struct edunic_stats	stats;

	/* --- software hardware-emulation state, see nic_hw.c --- */
	struct workqueue_struct	*hw_wq;
	struct work_struct	hw_tx_work;	/* emulated DMA engine: TX completion */
	struct work_struct	hw_rx_work;	/* emulated DMA engine: frame delivery */
	struct hrtimer		link_timer;	/* emulated PHY: link-up event */
	bool			link_up;
};

/* ===========================================================================
 * Cross-file function prototypes
 * ===========================================================================
 */

/* nic_hw.c -- register accessors and the emulated hardware model */
u32  edunic_reg_read(struct edunic_adapter *adapter, u32 reg);
void edunic_reg_write(struct edunic_adapter *adapter, u32 reg, u32 val);
void edunic_hw_reset(struct edunic_adapter *adapter);
void edunic_hw_init_rings(struct edunic_adapter *adapter);
void edunic_hw_start(struct edunic_adapter *adapter);
void edunic_hw_stop(struct edunic_adapter *adapter);
void edunic_hw_get_mac(struct edunic_adapter *adapter, u8 *mac);
void edunic_hw_set_mac(struct edunic_adapter *adapter, const u8 *mac);
int  edunic_hw_emulation_init(struct edunic_adapter *adapter);
void edunic_hw_emulation_uninit(struct edunic_adapter *adapter);
void edunic_hw_kick_tx(struct edunic_adapter *adapter);

/* nic_tx.c */
int  edunic_setup_tx_resources(struct edunic_adapter *adapter);
void edunic_free_tx_resources(struct edunic_adapter *adapter);
netdev_tx_t edunic_start_xmit(struct sk_buff *skb, struct net_device *netdev);
int  edunic_clean_tx_irq(struct edunic_adapter *adapter);

/* nic_rx.c */
int  edunic_setup_rx_resources(struct edunic_adapter *adapter);
void edunic_free_rx_resources(struct edunic_adapter *adapter);
int  edunic_alloc_rx_buffers(struct edunic_adapter *adapter, u16 cleaned_count);
int  edunic_clean_rx_irq(struct edunic_adapter *adapter, int budget);
void edunic_hw_deliver_rx_frame(struct edunic_adapter *adapter,
				 const void *frame_data, size_t len);

/* nic_irq.c */
int  edunic_setup_interrupts(struct edunic_adapter *adapter);
void edunic_free_interrupts(struct edunic_adapter *adapter);
void edunic_napi_add(struct edunic_adapter *adapter);
void edunic_napi_del(struct edunic_adapter *adapter);
void edunic_napi_enable(struct edunic_adapter *adapter);
void edunic_napi_disable(struct edunic_adapter *adapter);
irqreturn_t edunic_intr(int irq, void *data);

/* nic_pci.c */
extern struct pci_driver edunic_pci_driver;

/* nic_main.c */
extern const struct net_device_ops edunic_netdev_ops;
extern const struct ethtool_ops edunic_ethtool_ops;
int  edunic_open(struct net_device *netdev);
int  edunic_stop(struct net_device *netdev);

#endif /* _EDUNIC_H_ */
