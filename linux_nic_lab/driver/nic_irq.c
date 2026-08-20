// SPDX-License-Identifier: GPL-2.0
/*
 * nic_irq.c - interrupt request/free, the hard-IRQ handler, and NAPI.
 *
 * IRQ <-> NAPI relationship, in order:
 *
 *   1. The device raises an interrupt (a real one, via MSI/MSI-X or legacy
 *      INTx; in our emulated model, a direct call into edunic_intr() from
 *      nic_hw.c standing in for the interrupt controller).
 *   2. edunic_intr() runs in hard-IRQ context. It must be fast: it reads
 *      ICR (which atomically clears the causes it read -- "read to
 *      clear"), masks further RX/TX interrupts at the device, and calls
 *      napi_schedule() to defer the actual work to softirq context. It
 *      does *not* touch descriptors or build skbs itself.
 *   3. The NAPI core runs edunic_poll() in softirq context (NET_RX_SOFTIRQ)
 *      some time later, with a work budget. edunic_poll() drains TX
 *      completions and RX frames.
 *   4. If all available work was drained within budget, edunic_poll() calls
 *      napi_complete_done() and re-enables (unmasks) RX/TX interrupts. If
 *      the budget was exhausted with more work remaining, it returns
 *      without completing, and NAPI will call it again on the next softirq
 *      pass without needing another hardware interrupt at all -- this is
 *      exactly the mechanism that lets NAPI shed interrupt load under
 *      heavy traffic (interrupt mitigation).
 *
 * WHY MASK RX/TX INTERRUPTS WHILE NAPI RUNS?
 * Leaving them unmasked would let the device raise another interrupt for
 * work NAPI is already in the process of draining, which would just
 * trigger another (redundant) softirq schedule attempt for no benefit --
 * pure overhead under load. Masking during poll and unmasking only once
 * poll reports "no more work right now" is the standard NAPI-driver
 * contract.
 */

#include <linux/interrupt.h>
#include <linux/pci.h>
#include "nic.h"

/**
 * edunic_intr - hard-IRQ handler.
 *
 * Registered with request_irq() against whatever vector
 * pci_alloc_irq_vectors() gave us (MSI if available, legacy INTx
 * otherwise). Must be safe to call with interrupts disabled on this CPU
 * and must not sleep.
 */
irqreturn_t edunic_intr(int irq, void *data)
{
	struct net_device *netdev = data;
	struct edunic_adapter *adapter = netdev_priv(netdev);
	u32 icr;

	/*
	 * Interrupt Cause Read: on this model (mirroring real hardware such
	 * as e1000), reading ICR atomically returns the pending cause bits
	 * *and clears them*. This "read to clear" idiom means the read
	 * itself is the acknowledgment -- there is no separate "write 1 to
	 * ack" step needed for causes we are about to handle.
	 */
	icr = edunic_reg_read(adapter, EDUNIC_REG_ICR);
	if (!icr)
		return IRQ_NONE; /* not from us -- important if IRQF_SHARED */

	if (icr & EDUNIC_INT_LSC) {
		if (adapter->link_up) {
			netif_carrier_on(netdev);
			netdev_info(netdev, "link up\n");
		} else {
			netif_carrier_off(netdev);
			netdev_info(netdev, "link down\n");
		}
	}

	if (icr & (EDUNIC_INT_RX | EDUNIC_INT_TX)) {
		/*
		 * napi_schedule_prep() atomically checks "is NAPI already
		 * scheduled/running?" and, if not, marks it scheduled. This
		 * guards against double-scheduling if a second interrupt
		 * fires before the first poll has run (impossible here once
		 * we mask below, but this is the correct idiom generally,
		 * e.g. also races against napi_disable()).
		 */
		if (napi_schedule_prep(&adapter->napi)) {
			/* Mask RX/TX causes at the device until poll
			 * re-enables them -- see file header comment.
			 */
			edunic_reg_write(adapter, EDUNIC_REG_IMC,
					  EDUNIC_INT_RX | EDUNIC_INT_TX);
			__napi_schedule(&adapter->napi);
		}
	}

	return IRQ_HANDLED;
}

/**
 * edunic_poll - NAPI poll callback.
 *
 * Runs in softirq context. @budget bounds how many RX packets we may
 * process in this call so a single very busy device cannot monopolize the
 * CPU and starve other NAPI clients -- the core fairness mechanism NAPI
 * provides.
 */
static int edunic_poll(struct napi_struct *napi, int budget)
{
	struct edunic_adapter *adapter =
		container_of(napi, struct edunic_adapter, napi);
	int work_done;

	/* TX completion reclaim is not budgeted: the TX ring is bounded in
	 * size, so draining everything currently marked done is bounded
	 * work, and doing so promptly keeps the TX queue from stalling.
	 * This is the same choice most real drivers (e1000e, igb, ...) make.
	 */
	edunic_clean_tx_irq(adapter);

	work_done = edunic_clean_rx_irq(adapter, budget);

	/* If we used the entire budget, there may be more RX work waiting;
	 * return without completing so NAPI polls us again on its next pass
	 * instead of going back to interrupt-driven mode.
	 */
	if (work_done >= budget)
		return budget;

	/*
	 * napi_complete_done() tells the NAPI core "I'm done, nothing left
	 * right now" and transitions this NAPI context back out of
	 * scheduled state. Only after that should interrupts be
	 * re-enabled -- doing it in the other order would open a window
	 * where a new interrupt is masked-pending while NAPI still
	 * considers itself scheduled with nothing left to poll.
	 */
	if (napi_complete_done(napi, work_done))
		edunic_reg_write(adapter, EDUNIC_REG_IMS,
				  EDUNIC_INT_RX | EDUNIC_INT_TX);

	return work_done;
}

/**
 * edunic_setup_interrupts - allocate an interrupt vector and hook our ISR
 * up to it.
 *
 * pci_alloc_irq_vectors() with PCI_IRQ_MSI | PCI_IRQ_INTX asks for exactly
 * one vector, preferring MSI and falling back to legacy line-based INTx if
 * the platform or device can't do MSI. This is the modern replacement for
 * manually calling pci_enable_msi(); it also transparently covers MSI-X
 * for drivers that request more than one vector (this driver only needs
 * one, since RX and TX share a single NAPI context here -- see the README
 * for how a real multi-queue NIC would instead request one vector per
 * queue with PCI_IRQ_MSIX).
 */
int edunic_setup_interrupts(struct edunic_adapter *adapter)
{
	struct pci_dev *pdev = adapter->pdev;
	unsigned long irq_flags;
	int ret;

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (ret < 0) {
		netdev_err(adapter->netdev,
			   "failed to allocate an interrupt vector: %d\n",
			   ret);
		return ret;
	}

	adapter->msi_enabled = pdev->msi_enabled;
	adapter->irq = pci_irq_vector(pdev, 0);

	/* Legacy INTx lines are frequently shared between devices on the
	 * same routing pin; MSI vectors are private to this device and
	 * never need IRQF_SHARED.
	 */
	irq_flags = adapter->msi_enabled ? 0 : IRQF_SHARED;

	ret = request_irq(adapter->irq, edunic_intr, irq_flags,
			   adapter->netdev->name, adapter->netdev);
	if (ret) {
		netdev_err(adapter->netdev, "request_irq failed: %d\n", ret);
		pci_free_irq_vectors(pdev);
		return ret;
	}

	netdev_info(adapter->netdev, "using %s interrupts, irq %d\n",
		    adapter->msi_enabled ? "MSI" : "legacy INTx", adapter->irq);

	return 0;
}

void edunic_free_interrupts(struct edunic_adapter *adapter)
{
	free_irq(adapter->irq, adapter->netdev);
	pci_free_irq_vectors(adapter->pdev);
}

/**
 * edunic_napi_add / edunic_napi_del - bind/unbind the NAPI context to the
 * net_device. Called once each, from probe()/remove(), *not* from
 * open()/stop() -- netif_napi_add() associates the struct napi_struct with
 * the net_device for its entire lifetime, independent of how many times
 * the interface is brought up or down.
 */
void edunic_napi_add(struct edunic_adapter *adapter)
{
	netif_napi_add(adapter->netdev, &adapter->napi, edunic_poll);
}

void edunic_napi_del(struct edunic_adapter *adapter)
{
	netif_napi_del(&adapter->napi);
}

/**
 * edunic_napi_enable / edunic_napi_disable - called from ndo_open()/
 * ndo_stop() respectively.
 *
 * napi_enable() must happen before the device can be told it may raise RX/
 * TX interrupts (edunic_hw_start() in edunic_open()), or a napi_schedule()
 * could race a NAPI context that isn't allowed to run yet.
 *
 * napi_disable() blocks until any in-progress edunic_poll() call finishes
 * and prevents new ones from being scheduled -- required before
 * edunic_stop() tears down the rings poll touches.
 */
void edunic_napi_enable(struct edunic_adapter *adapter)
{
	napi_enable(&adapter->napi);
}

void edunic_napi_disable(struct edunic_adapter *adapter)
{
	napi_disable(&adapter->napi);
}
