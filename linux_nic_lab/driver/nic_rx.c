// SPDX-License-Identifier: GPL-2.0
/*
 * nic_rx.c - RX ring management, buffer refill, and the NAPI RX clean path.
 *
 * RX descriptor lifecycle (see README for the full diagram):
 *
 *   1. edunic_alloc_rx_buffers() allocates an skb, DMA-maps it
 *      DMA_FROM_DEVICE, points a descriptor at it, and hands ownership to
 *      the device by advancing RDT. The buffer is now "empty and owned by
 *      hardware".
 *   2. The device (real silicon, or edunic_hw_deliver_rx_frame() in our
 *      emulated model) DMA-writes a received frame into the buffer, fills
 *      in length/status, sets the Done (DD) bit, and raises an RX
 *      interrupt.
 *   3. edunic_clean_rx_irq(), called from NAPI poll, walks forward from
 *      next_to_clean harvesting every DD descriptor: unmap the DMA buffer,
 *      build an skb, hand it to the stack, advance next_to_clean.
 *   4. Freshly-consumed slots are refilled with new buffers and handed back
 *      to hardware, closing the loop.
 */

#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include "nic.h"

int edunic_setup_rx_resources(struct edunic_adapter *adapter)
{
	struct edunic_rx_ring *rx_ring = &adapter->rx_ring;
	struct device *dev = &adapter->pdev->dev;
	size_t desc_size = EDUNIC_RING_SIZE * sizeof(struct edunic_rx_desc);

	rx_ring->count = EDUNIC_RING_SIZE;
	rx_ring->next_to_use = 0;
	rx_ring->next_to_clean = 0;

	rx_ring->desc = dma_alloc_coherent(dev, desc_size, &rx_ring->dma,
					    GFP_KERNEL);
	if (!rx_ring->desc)
		return -ENOMEM;

	rx_ring->buf = kcalloc(rx_ring->count, sizeof(*rx_ring->buf),
				GFP_KERNEL);
	if (!rx_ring->buf) {
		dma_free_coherent(dev, desc_size, rx_ring->desc, rx_ring->dma);
		rx_ring->desc = NULL;
		return -ENOMEM;
	}

	return 0;
}

void edunic_free_rx_resources(struct edunic_adapter *adapter)
{
	struct edunic_rx_ring *rx_ring = &adapter->rx_ring;
	struct device *dev = &adapter->pdev->dev;
	size_t desc_size = rx_ring->count * sizeof(struct edunic_rx_desc);
	u16 i;

	if (!rx_ring->buf)
		goto free_desc;

	/* Free every buffer still posted to hardware (i.e. never delivered
	 * as a received frame) -- these were mapped DMA_FROM_DEVICE and
	 * must be unmapped before the underlying pages are freed.
	 */
	for (i = 0; i < rx_ring->count; i++) {
		struct edunic_rx_buf *buf = &rx_ring->buf[i];

		if (buf->skb) {
			dma_unmap_single(dev, buf->dma, buf->len,
					  DMA_FROM_DEVICE);
			dev_kfree_skb(buf->skb);
			buf->skb = NULL;
		}
	}

	kfree(rx_ring->buf);
	rx_ring->buf = NULL;

free_desc:
	if (rx_ring->desc) {
		dma_free_coherent(dev, desc_size, rx_ring->desc, rx_ring->dma);
		rx_ring->desc = NULL;
	}
}

/**
 * edunic_alloc_rx_buffers - allocate and post up to @cleaned_count fresh
 * RX buffers starting at next_to_use, then ring the RDT doorbell once for
 * the whole batch.
 *
 * Batching the doorbell write (rather than writing RDT once per buffer) is
 * a standard real-driver optimization: it turns N MMIO writes into 1.
 */
int edunic_alloc_rx_buffers(struct edunic_adapter *adapter, u16 cleaned_count)
{
	struct edunic_rx_ring *rx_ring = &adapter->rx_ring;
	struct device *dev = &adapter->pdev->dev;
	u16 i = rx_ring->next_to_use;
	u16 posted = 0;

	while (cleaned_count--) {
		struct edunic_rx_desc *desc = &rx_ring->desc[i];
		struct edunic_rx_buf *buf = &rx_ring->buf[i];
		struct sk_buff *skb;
		dma_addr_t dma;

		skb = netdev_alloc_skb_ip_align(adapter->netdev,
						 EDUNIC_RX_BUF_SIZE);
		if (!skb)
			break;

		/* Streaming DMA mapping, DMA_FROM_DEVICE this time: the
		 * device will write into this buffer, the CPU must not read
		 * it as valid until ownership comes back (DD bit observed +
		 * dma_unmap_single()/dma_sync_single_for_cpu(), which is what
		 * flushes any cache lines the device's write needs the CPU
		 * to see on non-cache-coherent-DMA architectures).
		 */
		dma = dma_map_single(dev, skb->data, EDUNIC_RX_BUF_SIZE,
				      DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, dma)) {
			dev_kfree_skb(skb);
			break;
		}

		buf->skb = skb;
		buf->dma = dma;
		buf->len = EDUNIC_RX_BUF_SIZE;

		/* Hardware descriptor: DMA address + ownership only. length
		 * is left 0 / status left 0 (not-done) -- the device fills
		 * both in when it writes a frame.
		 */
		desc->buffer_addr = cpu_to_le64(dma);
		desc->length = 0;
		desc->status = 0;
		desc->errors = 0;

		i = (i + 1) & EDUNIC_RING_MASK;
		posted++;
	}

	if (posted) {
		/* Ensure the descriptor writes above are visible before the
		 * device (or our emulated one) is told, via RDT, that it may
		 * start writing into these slots.
		 */
		dma_wmb();
		rx_ring->next_to_use = i;
		edunic_reg_write(adapter, EDUNIC_REG_RDT, i);
	}

	return posted;
}

/**
 * edunic_clean_rx_irq - harvest completed RX descriptors and hand frames
 * to the stack. Called from NAPI poll context with a work budget.
 *
 * Returns the number of packets processed, which the caller compares
 * against budget to decide whether NAPI should keep polling or complete.
 */
int edunic_clean_rx_irq(struct edunic_adapter *adapter, int budget)
{
	struct edunic_rx_ring *rx_ring = &adapter->rx_ring;
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->pdev->dev;
	unsigned int bytes = 0;
	u16 cleaned_count = 0;
	int work_done = 0;
	u16 i = rx_ring->next_to_clean;

	while (work_done < budget) {
		struct edunic_rx_desc *desc = &rx_ring->desc[i];
		struct edunic_rx_buf *buf = &rx_ring->buf[i];
		struct sk_buff *skb;
		u16 len;

		if (!(desc->status & EDUNIC_RXD_STAT_DD))
			break;
		/* Pairs with the dma_wmb() the producer (device / emulated
		 * hw) issues before setting DD -- see edunic_hw_deliver_rx_
		 * frame(). Guarantees length/status/errors read below are
		 * not stale relative to the DD bit we just observed.
		 */
		dma_rmb();

		len = le16_to_cpu(desc->length);

		/* Buffer ownership: device -> CPU. dma_unmap_single() tears
		 * down the streaming mapping and performs the cache
		 * invalidation a dma_sync_single_for_cpu() would, so the
		 * data the "device" wrote is now safe for the CPU to read.
		 */
		dma_unmap_single(dev, buf->dma, buf->len, DMA_FROM_DEVICE);

		skb = buf->skb;
		buf->skb = NULL;
		buf->dma = 0;

		if (desc->errors & EDUNIC_RXD_ERR_RXE) {
			dev_kfree_skb_any(skb);
			u64_stats_update_begin(&adapter->stats.syncp);
			adapter->stats.rx_errors++;
			u64_stats_update_end(&adapter->stats.syncp);
			goto next_desc;
		}

		skb_put(skb, len);

		/*
		 * eth_type_trans() does three things: it sets skb->dev,
		 * determines skb->pkt_type (PACKET_HOST / PACKET_BROADCAST /
		 * PACKET_MULTICAST / PACKET_OTHERHOST) by inspecting the
		 * Ethernet destination address against this device's own
		 * address, and pulls the Ethernet header off the front of
		 * the skb (skb->data now points at the L3 header, skb->len
		 * no longer includes the 14-byte Ethernet header), returning
		 * the EtherType to store in skb->protocol. This is the exact
		 * point where a raw received frame becomes something the
		 * rest of the network stack (ip_rcv, arp_rcv, ...) knows how
		 * to dispatch.
		 */
		skb->protocol = eth_type_trans(skb, netdev);

		/*
		 * Checksum metadata. Our emulated "hardware" verified the
		 * checksum (see EDUNIC_RXD_STAT_CSUM_VALID / nic_hw.c) by
		 * virtue of looping back a frame the stack itself computed a
		 * correct checksum for, so we can honestly report
		 * CHECKSUM_UNNECESSARY: "the stack does not need to verify
		 * this checksum itself." A real NIC would set this bit only
		 * after actually validating the IP/TCP/UDP checksum in
		 * hardware. If the bit is clear, CHECKSUM_NONE tells the
		 * stack "nothing was validated, check it yourself" -- the
		 * always-safe default.
		 */
		if (desc->status & EDUNIC_RXD_STAT_CSUM_VALID)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb->ip_summed = CHECKSUM_NONE;

		bytes += len;

		u64_stats_update_begin(&adapter->stats.syncp);
		adapter->stats.rx_packets++;
		adapter->stats.rx_bytes += len;
		u64_stats_update_end(&adapter->stats.syncp);

		/*
		 * napi_gro_receive() hands the frame into the Generic
		 * Receive Offload path: GRO holds onto structurally similar
		 * packets briefly to coalesce them (e.g. a run of TCP
		 * segments from the same flow) into fewer, larger skbs
		 * before handing them up to netif_receive_skb(), reducing
		 * per-packet stack traversal cost. This is the standard NAPI
		 * RX submission call; using netif_rx() instead would skip
		 * GRO entirely.
		 */
		napi_gro_receive(&adapter->napi, skb);

next_desc:
		desc->status = 0;
		desc->errors = 0;
		i = (i + 1) & EDUNIC_RING_MASK;
		work_done++;
		cleaned_count++;
	}

	rx_ring->next_to_clean = i;

	if (cleaned_count)
		edunic_alloc_rx_buffers(adapter, cleaned_count);

	return work_done;
}

/**
 * edunic_hw_deliver_rx_frame - emulated-hardware helper: write @len bytes
 * of @frame_data into the next hardware-owned RX buffer and mark it done.
 *
 * This stands in for a real DMA engine's bus-master write into the buffer
 * a driver posted. Called only from nic_hw.c's emulated TX-loopback path
 * (see the file header comment there for why interrupt delivery in this
 * educational model is a direct function call rather than a real IRQ line).
 *
 * A genuine DMA write from a device requires no dma_sync_*() call on the
 * device side -- that API exists for the *driver* side of a streaming
 * mapping. The correct, always-present sync point is the
 * dma_unmap_single() already performed in edunic_clean_rx_irq() above when
 * the driver later takes ownership back; that is what would invalidate any
 * stale CPU cache lines on a real non-cache-coherent-DMA platform. We plain
 * memcpy() here because this "device" is, mechanically, ordinary CPU code
 * sharing coherent kernel memory.
 */
void edunic_hw_deliver_rx_frame(struct edunic_adapter *adapter,
				 const void *frame_data, size_t len)
{
	struct edunic_rx_ring *rx_ring = &adapter->rx_ring;
	struct edunic_rx_desc *desc;
	struct edunic_rx_buf *buf;
	u32 head, tail;

	head = edunic_reg_read(adapter, EDUNIC_REG_RDH);
	tail = edunic_reg_read(adapter, EDUNIC_REG_RDT);

	if (head == tail) {
		/* No driver-posted buffer available -- equivalent to a real
		 * NIC's "RX no buffer" drop condition.
		 */
		u64_stats_update_begin(&adapter->stats.syncp);
		adapter->stats.rx_dropped++;
		u64_stats_update_end(&adapter->stats.syncp);
		return;
	}

	desc = &rx_ring->desc[head];
	buf = &rx_ring->buf[head];

	if (len > buf->len)
		len = buf->len; /* defensive clamp; should not happen with a
				  * fixed MTU-sized loopback frame */

	memcpy(buf->skb->data, frame_data, len);

	desc->length = cpu_to_le16(len);
	desc->errors = 0;

	/* Publish length before the DD ownership bit, mirroring the
	 * TX-side dma_wmb()/doorbell ordering.
	 */
	dma_wmb();
	desc->status = EDUNIC_RXD_STAT_DD | EDUNIC_RXD_STAT_EOP |
		       EDUNIC_RXD_STAT_CSUM_VALID;

	head = (head + 1) & EDUNIC_RING_MASK;
	edunic_reg_write(adapter, EDUNIC_REG_RDH, head);
	edunic_reg_write(adapter, EDUNIC_REG_ICS, EDUNIC_INT_RX);
}
