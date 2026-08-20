// SPDX-License-Identifier: GPL-2.0
/*
 * nic_hw.c - register access + the software-emulated "chip" behind BAR0.
 *
 * This file has two very different halves, and it is important to keep the
 * distinction clear while reading it:
 *
 *   1. REGISTER ACCESSORS (edunic_reg_read/write, edunic_hw_reset/init/
 *      start/stop, MAC get/set): these are exactly what a real driver would
 *      contain. They talk to adapter->hw_addr, the ioremap'd BAR0 handed to
 *      us by pci_iomap() in nic_pci.c, using readl()/writel(). This code is
 *      only ever *exercised* when this driver is actually bound to a device
 *      (real or virtual) that implements the register contract documented
 *      in nic.h. There is no such real device -- see the top of nic.h.
 *
 *   2. THE EMULATED DMA ENGINE (edunic_hw_tx_worker, edunic_hw_kick_tx,
 *      the link-up hrtimer): this models what the *chip itself* would do
 *      internally in reaction to the register writes above -- fetch TX
 *      descriptors, DMA the payload, mark completion, assert an interrupt.
 *      A real chip does this in silicon; here it is done by a kernel
 *      workqueue operating on the very same dma_alloc_coherent() ring
 *      memory a real DMA engine would bus-master into. This is the
 *      "software-emulated educational NIC model" the project intentionally
 *      substitutes for real silicon.
 *
 * ---------------------------------------------------------------------------
 * WHY readl()/writel() AND NOT A RAW `volatile u32 *` DEREFERENCE?
 * ---------------------------------------------------------------------------
 * It is tempting to write:
 *
 *     u32 val = *(volatile u32 *)(hw_addr + reg);
 *
 * and assume `volatile` is enough. It is not, for several independent
 * reasons that readl()/writel() (and the ioread/iowrite family) handle for
 * you:
 *
 *   - Compiler barrier vs CPU barrier: `volatile` stops the *compiler* from
 *     reordering or eliding the access, but says nothing about whether the
 *     *CPU* can reorder it against other loads/stores, or whether posted
 *     writes on the interconnect have actually reached the device. readl()/
 *     writel() include the architecture-appropriate barriers (e.g. on many
 *     architectures a write is followed by a read-back-style barrier, and
 *     on arm64 accesses go through Device-nGnRE memory with ordering rules
 *     the compiler alone cannot express).
 *
 *   - Endianness: real PCIe register files are conventionally little-endian
 *     on the bus. readl()/writel() do the swap for you on big-endian hosts;
 *     a raw pointer dereference does not.
 *
 *   - Portability across architectures: MMIO semantics (whether accesses
 *     may be combined/reordered, whether they need side-effect-preserving
 *     "strongly ordered" access) differ across x86/arm/arm64/powerpc. The
 *     io.h accessors are the portable abstraction; raw pointers are not.
 *
 *   - Tooling: sparse's `__iomem` address-space annotation lets `make C=1`
 *     catch accidental use of an MMIO pointer as normal memory (or vice
 *     versa) at build time. A `volatile u32 *` has no such annotation and
 *     defeats this class of static checking entirely.
 *
 *   - Debuggability: readl()/writel() are trace points many kernels build
 *     (CONFIG_TRACE_MMIO_ACCESS-style facilities) hook into; raw pointer
 *     dereferences are invisible to that infrastructure.
 *
 * In short: `volatile` only promises "don't optimize this access away or
 * reorder it at the C abstract-machine level." MMIO correctness needs
 * architecture-aware ordering and endianness handling, which is exactly
 * what readl()/writel() provide and a raw pointer does not.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/etherdevice.h>
#include "nic.h"

/* ===========================================================================
 * Register accessors
 * ===========================================================================
 */

/**
 * edunic_reg_read - read one 32-bit MMIO register from BAR0.
 *
 * readl() guarantees: the access is not reordered by the compiler, it is
 * not combined/cached, and (on architectures where it matters) prior
 * writel()s issued by this CPU are ordered before this read completes at
 * the bus level -- which is exactly the guarantee the driver leans on when
 * it writes a doorbell register and then wants to observe device state
 * that write is supposed to have kicked off.
 */
u32 edunic_reg_read(struct edunic_adapter *adapter, u32 reg)
{
	return readl(adapter->hw_addr + reg);
}

/**
 * edunic_reg_write - write one 32-bit MMIO register in BAR0.
 *
 * writel() on most architectures is a *posted* write: it may complete on
 * the interconnect asynchronously with respect to the CPU instruction
 * stream. Drivers that need the write to be visible to the device before
 * proceeding follow it with a readl() of any register (a "posted write
 * flush"), which is exactly what edunic_hw_start()/edunic_hw_stop() do
 * after touching EDUNIC_REG_CTRL.
 */
void edunic_reg_write(struct edunic_adapter *adapter, u32 reg, u32 val)
{
	writel(val, adapter->hw_addr + reg);
}

/**
 * edunic_hw_reset - pulse the device's software reset and wait for it to
 * self-clear / for the device to report reset-done.
 *
 * This is the standard pattern for level-triggered "reset done" status
 * bits: write the reset bit, then poll status with a bounded timeout
 * rather than an unbounded busy loop, so a wedged or absent device
 * produces a clean -ETIMEDOUT instead of hanging the calling context
 * forever. readl_poll_timeout() sleeps between polls (it is not atomic
 * context here -- we are in probe()), which is friendlier than a tight
 * spin.
 */
void edunic_hw_reset(struct edunic_adapter *adapter)
{
	u32 status;
	int ret;

	adapter->link_up = false;
	edunic_reg_write(adapter, EDUNIC_REG_CTRL, EDUNIC_CTRL_RST);

	ret = readl_poll_timeout(adapter->hw_addr + EDUNIC_REG_STATUS, status,
				  status & EDUNIC_STATUS_RST_DONE,
				  1000, 50000);
	if (ret)
		netdev_warn(adapter->netdev,
			    "reset did not complete within timeout (no device attached?)\n");
}

/**
 * edunic_hw_init_rings - program the ring base/length registers.
 *
 * This is the point where the device is told where the descriptor rings
 * live in DMA-addressable memory. TDBAL/TDBAH/RDBAL/RDBAH take the 64-bit
 * dma_addr_t returned by dma_alloc_coherent() split across two 32-bit
 * registers, which is exactly how real 32-bit-register-file NICs expose a
 * 64-bit base address. TDH/RDH (head) and TDT/RDT (tail) both start at 0:
 * an empty ring, nothing owned by hardware yet.
 */
void edunic_hw_init_rings(struct edunic_adapter *adapter)
{
	dma_addr_t tx_dma = adapter->tx_ring.dma;
	dma_addr_t rx_dma = adapter->rx_ring.dma;

	edunic_reg_write(adapter, EDUNIC_REG_TDBAL, lower_32_bits(tx_dma));
	edunic_reg_write(adapter, EDUNIC_REG_TDBAH, upper_32_bits(tx_dma));
	edunic_reg_write(adapter, EDUNIC_REG_TDLEN, adapter->tx_ring.count);
	edunic_reg_write(adapter, EDUNIC_REG_TDH, 0);
	edunic_reg_write(adapter, EDUNIC_REG_TDT, 0);

	edunic_reg_write(adapter, EDUNIC_REG_RDBAL, lower_32_bits(rx_dma));
	edunic_reg_write(adapter, EDUNIC_REG_RDBAH, upper_32_bits(rx_dma));
	edunic_reg_write(adapter, EDUNIC_REG_RDLEN, adapter->rx_ring.count);
	edunic_reg_write(adapter, EDUNIC_REG_RDH, 0);
	edunic_reg_write(adapter, EDUNIC_REG_RDT, 0);
}

/**
 * edunic_hw_start - enable TX/RX DMA engines and unmask interrupts.
 *
 * Order matters here in a real driver: NAPI must already be enabled and
 * the IRQ already requested (done by the caller, edunic_open(), before
 * this is called) so that the moment the device is allowed to raise an
 * interrupt, someone is listening for it. Only once that's true do we
 * flip TXEN/RXEN and unmask causes.
 */
void edunic_hw_start(struct edunic_adapter *adapter)
{
	u32 ctrl;

	edunic_reg_write(adapter, EDUNIC_REG_IMS, EDUNIC_INT_ALL);

	ctrl = edunic_reg_read(adapter, EDUNIC_REG_CTRL);
	ctrl |= EDUNIC_CTRL_TXEN | EDUNIC_CTRL_RXEN;
	edunic_reg_write(adapter, EDUNIC_REG_CTRL, ctrl);

	/* Flush the posted write before returning, so the caller can rely on
	 * the device having observed TXEN/RXEN.
	 */
	edunic_reg_read(adapter, EDUNIC_REG_STATUS);

	/* Kick off the emulated PHY autonegotiation delay; see
	 * edunic_link_timer_fn() above.
	 */
	hrtimer_start(&adapter->link_timer, ms_to_ktime(1000),
		      HRTIMER_MODE_REL);
}

/**
 * edunic_hw_stop - disable TX/RX DMA engines and mask all interrupts.
 *
 * Always mask interrupts and stop DMA *before* the caller tears down NAPI
 * and frees the IRQ (see edunic_stop() in nic_main.c) -- otherwise a
 * still-armed device could raise an interrupt for a handler that no
 * longer exists.
 */
void edunic_hw_stop(struct edunic_adapter *adapter)
{
	u32 ctrl;

	edunic_reg_write(adapter, EDUNIC_REG_IMC, EDUNIC_INT_ALL);

	ctrl = edunic_reg_read(adapter, EDUNIC_REG_CTRL);
	ctrl &= ~(EDUNIC_CTRL_TXEN | EDUNIC_CTRL_RXEN);
	edunic_reg_write(adapter, EDUNIC_REG_CTRL, ctrl);

	edunic_reg_read(adapter, EDUNIC_REG_STATUS); /* posted-write flush */
}

/**
 * edunic_hw_get_mac / edunic_hw_set_mac - station address handling.
 *
 * A real NIC typically loads a burned-in MAC from an EEPROM/OTP fuse and
 * exposes it read-only (or read/write for override) via MAC_LOW/MAC_HIGH.
 * We have no EEPROM, so nic_pci.c seeds one locally-administered random
 * address with eth_hw_addr_random() and programs it into the device with
 * edunic_hw_set_mac() so the two stay in sync, mirroring what a driver
 * does after accepting an administrator override via `ip link set address`.
 */
void edunic_hw_get_mac(struct edunic_adapter *adapter, u8 *mac)
{
	u32 low = edunic_reg_read(adapter, EDUNIC_REG_MAC_LOW);
	u32 high = edunic_reg_read(adapter, EDUNIC_REG_MAC_HIGH);

	mac[0] = low & 0xff;
	mac[1] = (low >> 8) & 0xff;
	mac[2] = (low >> 16) & 0xff;
	mac[3] = (low >> 24) & 0xff;
	mac[4] = high & 0xff;
	mac[5] = (high >> 8) & 0xff;
}

void edunic_hw_set_mac(struct edunic_adapter *adapter, const u8 *mac)
{
	u32 low = mac[0] | (mac[1] << 8) | (mac[2] << 16) | (mac[3] << 24);
	u32 high = mac[4] | (mac[5] << 8);

	edunic_reg_write(adapter, EDUNIC_REG_MAC_LOW, low);
	edunic_reg_write(adapter, EDUNIC_REG_MAC_HIGH, high);
}

/* ===========================================================================
 * Emulated DMA engine / "chip" behavior
 * ===========================================================================
 * Everything below stands in for silicon. It is deliberately written to
 * only ever touch memory a real DMA engine would legitimately touch (the
 * coherent descriptor rings, and streaming-DMA packet buffers via the
 * proper dma_sync_single_for_*() calls) and to only ever signal the driver
 * the way real hardware signals a driver: by updating status registers and
 * causing an interrupt. See the note in edunic_hw_deliver_rx_frame() and
 * below for the one place this necessarily departs from real hardware:
 * there is no physical interrupt line to assert, so we call the driver's
 * own ISR (edunic_intr(), in nic_irq.c) directly, the same way an
 * interrupt controller calling into do_IRQ() would.
 */

/**
 * edunic_hw_tx_worker - the emulated DMA engine's reaction to a TX doorbell.
 *
 * Runs in process context on adapter->hw_wq. Walks every descriptor between
 * the device's current head (TDH) and the tail the driver last rang
 * (TDT), "transmits" each one (captures its payload for RX loopback, see
 * below), marks it done, advances TDH, and raises a TX-complete interrupt
 * -- exactly what real NIC silicon does, just in C instead of gates.
 */
static void edunic_hw_tx_worker(struct work_struct *work)
{
	struct edunic_adapter *adapter =
		container_of(work, struct edunic_adapter, hw_tx_work);
	struct edunic_tx_ring *tx_ring = &adapter->tx_ring;
	u32 head, tail;
	bool sent_any = false;

	head = edunic_reg_read(adapter, EDUNIC_REG_TDH);
	tail = edunic_reg_read(adapter, EDUNIC_REG_TDT);

	while (head != tail) {
		struct edunic_tx_desc *desc = &tx_ring->desc[head];
		struct edunic_tx_buf *buf = &tx_ring->buf[head];
		void *frame_copy;
		u16 len = le16_to_cpu(desc->length);

		if (buf->dma && len) {
			/* Make the DMA buffer's contents CPU-visible the same
			 * way a real device's DMA read would require the
			 * driver to have made them device-visible: we are
			 * standing in for the device side of that contract.
			 */
			dma_sync_single_for_cpu(&adapter->pdev->dev, buf->dma,
						 len, DMA_TO_DEVICE);

			frame_copy = kmalloc(len, GFP_ATOMIC);
			if (frame_copy) {
				/* buf->skb is still valid: completion (which
				 * frees it) has not happened yet.
				 */
				memcpy(frame_copy, buf->skb->data, len);
			}

			dma_sync_single_for_device(&adapter->pdev->dev,
						    buf->dma, len,
						    DMA_TO_DEVICE);

			if (frame_copy) {
				/* Loop the frame back onto the RX ring, like
				 * a cable plugged into the same interface's
				 * link partner would. This is what lets the
				 * README's UDP/TCP walkthrough actually see
				 * its own traffic arrive on RX.
				 */
				edunic_hw_deliver_rx_frame(adapter, frame_copy,
							    len);
				kfree(frame_copy);
			}
		}

		/* Ownership transition: device -> driver. dma_wmb() ensures
		 * the write of desc->status = DD is visible to the CPU only
		 * after everything above it (nothing else here, but in
		 * general any descriptor field updates) is visible first --
		 * mirrors the driver-side dma_wmb() used before ringing a
		 * doorbell, just for the opposite direction of ownership.
		 */
		dma_wmb();
		desc->status = EDUNIC_TXD_STAT_DD;

		head = (head + 1) & EDUNIC_RING_MASK;
		sent_any = true;
	}

	if (sent_any) {
		edunic_reg_write(adapter, EDUNIC_REG_TDH, head);
		edunic_reg_write(adapter, EDUNIC_REG_ICS, EDUNIC_INT_TX);
		edunic_intr(adapter->irq, adapter->netdev);
	}
}

/**
 * edunic_hw_kick_tx - called by nic_tx.c immediately after writing TDT.
 *
 * On real hardware, writing the tail/doorbell register is itself the
 * signal that starts the DMA engine -- there is nothing else to do.
 * Because our "DMA engine" is a workqueue instead of silicon, something
 * has to schedule it; that is all this function does. It is the *only*
 * extra call the TX path needs beyond the doorbell write itself, and it
 * is clearly segregated here rather than hidden inside nic_tx.c's
 * otherwise-realistic xmit path.
 */
void edunic_hw_kick_tx(struct edunic_adapter *adapter)
{
	queue_work(adapter->hw_wq, &adapter->hw_tx_work);
}

/**
 * edunic_link_timer_fn - emulated PHY link-up event.
 *
 * Real PHYs take a non-zero amount of time (autonegotiation) after being
 * enabled before link comes up, and signal completion with a Link Status
 * Change interrupt rather than being up instantaneously. We model that
 * with a one-shot hrtimer armed from edunic_hw_start().
 */
static enum hrtimer_restart edunic_link_timer_fn(struct hrtimer *timer)
{
	struct edunic_adapter *adapter =
		container_of(timer, struct edunic_adapter, link_timer);
	u32 status;

	adapter->link_up = true;
	status = edunic_reg_read(adapter, EDUNIC_REG_STATUS);
	edunic_reg_write(adapter, EDUNIC_REG_STATUS,
			  status | EDUNIC_STATUS_LINKUP);
	edunic_reg_write(adapter, EDUNIC_REG_ICS, EDUNIC_INT_LSC);
	edunic_intr(adapter->irq, adapter->netdev);

	return HRTIMER_NORESTART;
}

/**
 * edunic_hw_emulation_init - set up the workqueue/timer backing the
 * emulated chip. Called once from probe().
 */
int edunic_hw_emulation_init(struct edunic_adapter *adapter)
{
	adapter->hw_wq = alloc_ordered_workqueue("%s-hw", 0,
						  netdev_name(adapter->netdev));
	if (!adapter->hw_wq)
		return -ENOMEM;

	INIT_WORK(&adapter->hw_tx_work, edunic_hw_tx_worker);
	/*
	 * hrtimer_setup() is the modern (post-6.12ish) combined init+arm-
	 * callback API, replacing the older hrtimer_init() +
	 * `timer.function = fn` two-step. It takes the callback up front so
	 * the timer is never left in a state where it's initialized but has
	 * no function assigned.
	 */
	hrtimer_setup(&adapter->link_timer, edunic_link_timer_fn,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	return 0;
}

/**
 * edunic_hw_emulation_uninit - mirror of edunic_hw_emulation_init(),
 * called from remove(). Cancels any in-flight emulated work before the
 * memory it touches (rings, adapter struct) is freed out from under it.
 */
void edunic_hw_emulation_uninit(struct edunic_adapter *adapter)
{
	hrtimer_cancel(&adapter->link_timer);
	if (adapter->hw_wq) {
		cancel_work_sync(&adapter->hw_tx_work);
		destroy_workqueue(adapter->hw_wq);
		adapter->hw_wq = NULL;
	}
}
