// SPDX-License-Identifier: GPL-2.0
/*
 * nic_tx.c - TX ring management and ndo_start_xmit().
 *
 * TX descriptor lifecycle (see README for the full diagram):
 *
 *   1. edunic_start_xmit() DMA-maps skb->data, fills the descriptor at
 *      next_to_use, and hands ownership to the device by ringing the TDT
 *      doorbell.
 *   2. The device (real silicon, or our emulated one in nic_hw.c) DMA-reads
 *      the buffer, transmits it, and marks the descriptor Done (DD),
 *      raising a TX-complete interrupt.
 *   3. edunic_clean_tx_irq(), called from NAPI poll, walks forward from
 *      next_to_clean reclaiming every DD descriptor: unmap the DMA buffer,
 *      free the skb, advance next_to_clean.
 *   4. If the ring had been stopped because it was full, and cleaning freed
 *      enough space, the queue is woken so the stack can send more.
 */

#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include "nic.h"

/* Minimum free descriptors we insist on having before accepting another
 * packet. Each skb here uses exactly one descriptor (no scatter-gather),
 * so 1 would technically suffice, but real drivers keep a small cushion
 * (here: enough for one more max-size packet's worth of headroom logic)
 * -- kept at 1 for this single-descriptor-per-packet model, named so the
 * intent at call sites is clear.
 */
#define EDUNIC_TX_DESC_PER_PKT	1

static inline u16 edunic_tx_unused(const struct edunic_tx_ring *tx_ring)
{
	/* Free slots = distance from next_to_use to next_to_clean going
	 * forward around the ring, minus one: a full ring (next_to_use ==
	 * next_to_clean - 1, i.e. one slot short of wrapping onto
	 * next_to_clean) is kept as the "ring full" state rather than
	 * letting next_to_use catch up to next_to_clean exactly, which
	 * would be indistinguishable from "ring empty".
	 */
	u16 count = tx_ring->count;

	return (tx_ring->next_to_clean - tx_ring->next_to_use - 1 + count) % count;
}

/**
 * edunic_maybe_stop_tx - stop the queue if there isn't room for another
 * packet, using the classic check/stop/recheck pattern to avoid a race
 * against a concurrent completion on another CPU.
 *
 * Without the recheck, this sequence is possible:
 *   xmit (CPU0): sees ring full
 *   clean (CPU1): reclaims descriptors, sees queue not yet stopped -> does
 *                 not wake it
 *   xmit (CPU0): calls netif_stop_queue()
 * ...and the queue is now stopped with nobody left to wake it. Rechecking
 * after stop, and waking immediately if space showed up in the meantime,
 * closes that window -- this is the same pattern used by e1000/igb.
 */
static int edunic_maybe_stop_tx(struct net_device *netdev,
				 struct edunic_tx_ring *tx_ring, u16 needed)
{
	if (likely(edunic_tx_unused(tx_ring) >= needed))
		return 0;

	netif_stop_queue(netdev);
	/* Pairs with the barrier implied by dma_wmb()/the descriptor status
	 * write in edunic_clean_tx_irq(); ensures we don't observe a stale
	 * next_to_clean after stopping the queue.
	 */
	smp_mb();

	if (likely(edunic_tx_unused(tx_ring) < needed))
		return -EBUSY;

	/* Someone freed up space between our first check and stopping the
	 * queue -- undo the stop.
	 */
	netif_wake_queue(netdev);
	return 0;
}

int edunic_setup_tx_resources(struct edunic_adapter *adapter)
{
	struct edunic_tx_ring *tx_ring = &adapter->tx_ring;
	struct device *dev = &adapter->pdev->dev;
	size_t desc_size = EDUNIC_RING_SIZE * sizeof(struct edunic_tx_desc);

	tx_ring->count = EDUNIC_RING_SIZE;
	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;

	/*
	 * dma_alloc_coherent() gives us memory that is simultaneously:
	 *   - CPU-addressable via the returned kernel virtual pointer, and
	 *   - DMA-addressable via *dma without any explicit sync calls,
	 *     because "coherent" memory is kept coherent between CPU caches
	 *     and the device by the platform (either genuinely uncached, or
	 *     hardware-cache-coherent DMA).
	 * This is the right allocator for *control structures* like a
	 * descriptor ring that both sides read/write repeatedly and where
	 * sync-on-every-access overhead would be wasteful. It is the wrong
	 * allocator for packet payloads, which is why buffer data uses
	 * streaming DMA (dma_map_single()) instead -- see edunic_start_xmit()
	 * and nic_rx.c.
	 */
	tx_ring->desc = dma_alloc_coherent(dev, desc_size, &tx_ring->dma,
					    GFP_KERNEL);
	if (!tx_ring->desc)
		return -ENOMEM;

	tx_ring->buf = kcalloc(tx_ring->count, sizeof(*tx_ring->buf),
				GFP_KERNEL);
	if (!tx_ring->buf) {
		dma_free_coherent(dev, desc_size, tx_ring->desc, tx_ring->dma);
		tx_ring->desc = NULL;
		return -ENOMEM;
	}

	return 0;
}

void edunic_free_tx_resources(struct edunic_adapter *adapter)
{
	struct edunic_tx_ring *tx_ring = &adapter->tx_ring;
	struct device *dev = &adapter->pdev->dev;
	size_t desc_size = tx_ring->count * sizeof(struct edunic_tx_desc);
	u16 i;

	if (!tx_ring->buf)
		goto free_desc;

	/* Tear down any packets still in flight: unmap the streaming DMA
	 * mapping and free the skb, exactly like a normal completion would,
	 * since the device will never complete them now.
	 */
	for (i = 0; i < tx_ring->count; i++) {
		struct edunic_tx_buf *buf = &tx_ring->buf[i];

		if (buf->skb) {
			dma_unmap_single(dev, buf->dma, buf->len,
					  DMA_TO_DEVICE);
			dev_kfree_skb(buf->skb);
			buf->skb = NULL;
		}
	}

	kfree(tx_ring->buf);
	tx_ring->buf = NULL;

free_desc:
	if (tx_ring->desc) {
		dma_free_coherent(dev, desc_size, tx_ring->desc, tx_ring->dma);
		tx_ring->desc = NULL;
	}
}

/**
 * edunic_start_xmit - net_device_ops .ndo_start_xmit
 *
 * Called by the qdisc layer with this queue's __netif_tx_lock held, so we
 * do not need our own locking against concurrent xmit calls on the same
 * queue. Concurrent NAPI cleaning on another CPU is handled by the
 * single-writer-per-field discipline documented on edunic_tx_unused().
 */
netdev_tx_t edunic_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct edunic_adapter *adapter = netdev_priv(netdev);
	struct edunic_tx_ring *tx_ring = &adapter->tx_ring;
	struct device *dev = &adapter->pdev->dev;
	struct edunic_tx_desc *desc;
	struct edunic_tx_buf *buf;
	dma_addr_t dma;
	u16 i;

	if (edunic_maybe_stop_tx(netdev, tx_ring, EDUNIC_TX_DESC_PER_PKT)) {
		/* No room. The queue is now stopped; edunic_clean_tx_irq()
		 * will wake it once space frees up. Returning BUSY here
		 * (rather than dropping) tells the stack to requeue.
		 */
		return NETDEV_TX_BUSY;
	}

	/*
	 * Streaming DMA mapping: unlike the coherent ring memory, this
	 * buffer is normal cacheable kernel memory (skb->data) that is only
	 * handed to the device for the duration of one transfer.
	 * dma_map_single() is responsible for:
	 *   - translating the CPU virtual address into a dma_addr_t the
	 *     device can use on the bus. On a system with an IOMMU, this is
	 *     an IOVA that the IOMMU translates to the real physical page(s)
	 *     on the fly (and can enforce that the device may only access
	 *     precisely this mapped range -- DMA isolation); without an
	 *     IOMMU, it degrades to physical address + offset.
	 *   - flushing/invalidating CPU caches as needed so the device sees
	 *     the bytes the CPU just wrote (this is the operation
	 *     dma_alloc_coherent() memory does not need per-transfer,
	 *     because it is kept coherent continuously).
	 * Ownership of this memory now conceptually belongs to the device
	 * until dma_unmap_single() is called in edunic_clean_tx_irq(); the
	 * CPU must not modify skb->data in that window.
	 */
	dma = dma_map_single(dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma)) {
		dev_kfree_skb_any(skb);
		u64_stats_update_begin(&adapter->stats.syncp);
		adapter->stats.tx_errors++;
		u64_stats_update_end(&adapter->stats.syncp);
		return NETDEV_TX_OK;
	}

	i = tx_ring->next_to_use;
	desc = &tx_ring->desc[i];
	buf = &tx_ring->buf[i];

	buf->skb = skb;
	buf->dma = dma;
	buf->len = skb->len;

	/* Hardware descriptor fields only: DMA address, length, command and
	 * status bits. No kernel pointer is ever written here -- the device
	 * has no notion of a struct sk_buff.
	 */
	desc->buffer_addr = cpu_to_le64(dma);
	desc->length = cpu_to_le16(skb->len);
	desc->cmd = EDUNIC_TXD_CMD_EOP | EDUNIC_TXD_CMD_RS;
	desc->status = 0; /* not-done: owned by the device once posted */

	/*
	 * dma_wmb(): a write memory barrier that orders the descriptor field
	 * writes above it *before* the doorbell register write below it, as
	 * observed by the device. Without this, on architectures with
	 * relaxed memory ordering the CPU or interconnect could let the
	 * doorbell write become visible to the device before the descriptor
	 * contents it depends on are visible, and the device could fetch a
	 * half-written descriptor. This is the write-side counterpart of
	 * the dma_rmb() used on the completion side (see nic_rx.c and
	 * nic_hw.c) when reading a status bit before trusting the fields it
	 * guards.
	 */
	dma_wmb();

	tx_ring->next_to_use = (i + 1) & EDUNIC_RING_MASK;

	/* The doorbell: this MMIO write is what actually hands the packet to
	 * the device. On real hardware this alone starts the DMA engine.
	 */
	edunic_reg_write(adapter, EDUNIC_REG_TDT, tx_ring->next_to_use);
	edunic_hw_kick_tx(adapter); /* emulation-only: see nic_hw.c */

	/* Check again with the packet we just posted accounted for; stop the
	 * queue now if the *next* packet wouldn't fit, rather than waiting
	 * for it to arrive and be rejected.
	 */
	edunic_maybe_stop_tx(netdev, tx_ring, EDUNIC_TX_DESC_PER_PKT);

	return NETDEV_TX_OK;
}

/**
 * edunic_clean_tx_irq - reclaim completed TX descriptors.
 *
 * Called from NAPI poll context (see nic_irq.c). Returns true if the ring
 * was fully drained of completed work (used by the poll function to decide
 * whether TX contributed to "more work available").
 */
int edunic_clean_tx_irq(struct edunic_adapter *adapter)
{
	struct edunic_tx_ring *tx_ring = &adapter->tx_ring;
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->pdev->dev;
	unsigned int packets = 0;
	unsigned int bytes = 0;
	u16 i = tx_ring->next_to_clean;

	while (i != tx_ring->next_to_use) {
		struct edunic_tx_desc *desc = &tx_ring->desc[i];
		struct edunic_tx_buf *buf = &tx_ring->buf[i];

		/*
		 * dma_rmb(): pairs with the dma_wmb() the emulated device
		 * issues in nic_hw.c before setting DD. Guarantees that if
		 * we observe the DD bit set, we will not read a stale
		 * (pre-completion) view of any other descriptor field the
		 * device updated alongside it (length/status/errors on the
		 * RX side; here there is nothing further to read on TX
		 * completion, but the barrier is the same idiom used
		 * uniformly on every ownership-bit check in this driver).
		 */
		if (!(desc->status & EDUNIC_TXD_STAT_DD))
			break;
		dma_rmb();

		dma_unmap_single(dev, buf->dma, buf->len, DMA_TO_DEVICE);

		bytes += buf->len;
		packets++;

		dev_consume_skb_any(buf->skb);
		buf->skb = NULL;
		buf->dma = 0;
		buf->len = 0;
		desc->status = 0;

		i = (i + 1) & EDUNIC_RING_MASK;
	}

	tx_ring->next_to_clean = i;

	if (packets) {
		u64_stats_update_begin(&adapter->stats.syncp);
		adapter->stats.tx_packets += packets;
		adapter->stats.tx_bytes += bytes;
		u64_stats_update_end(&adapter->stats.syncp);

		/* Wake the queue if it was stopped and there's room now.
		 * netif_queue_stopped() + netif_wake_queue() is the standard
		 * pairing; wake unconditionally checks the underlying
		 * capacity rather than trusting packets>0 alone.
		 */
		if (netif_queue_stopped(netdev) &&
		    edunic_tx_unused(tx_ring) >= EDUNIC_TX_DESC_PER_PKT)
			netif_wake_queue(netdev);
	}

	return packets;
}
