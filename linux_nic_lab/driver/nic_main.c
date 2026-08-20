// SPDX-License-Identifier: GPL-2.0
/*
 * nic_main.c - module init/exit, net_device_ops, ndo_open/ndo_stop, and the
 * handful of other net_device callbacks that don't have a whole file of
 * their own (change_mtu, get_stats64, set_mac_address) plus a minimal
 * ethtool_ops.
 *
 * This is deliberately the *last* file in the "Code Reading Order" section
 * of the README even though it's small: module_init() is where execution
 * begins, but understanding what it kicks off (pci_register_driver() ->
 * eventually edunic_probe() in nic_pci.c) requires everything else first.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/u64_stats_sync.h>
#include "nic.h"

/**
 * edunic_open - net_device_ops .ndo_open, invoked when the interface is
 * administratively brought up (`ip link set <iface> up`, or automatically
 * if NetworkManager/systemd-networkd does so).
 *
 * Rings, NAPI *registration*, and the IRQ vector's non-request setup were
 * already done once in probe() and are *not* redone here (see nic_pci.c);
 * ndo_open()'s job is to make the already-built machinery live: enable
 * NAPI processing, request the actual IRQ, then tell the device it may
 * start moving packets and raising interrupts -- in that order, because
 * each step assumes the previous one already holds.
 */
int edunic_open(struct net_device *netdev)
{
	struct edunic_adapter *adapter = netdev_priv(netdev);
	int ret;

	edunic_napi_enable(adapter);

	ret = edunic_setup_interrupts(adapter);
	if (ret) {
		edunic_napi_disable(adapter);
		return ret;
	}

	/* Only now is it safe to let the device start DMA and raise
	 * interrupts: NAPI can process them (enabled above) and an ISR is
	 * registered to schedule NAPI (request_irq above).
	 */
	edunic_hw_start(adapter);

	netif_start_queue(netdev);

	return 0;
}

/**
 * edunic_stop - net_device_ops .ndo_stop, invoked on `ip link set down`
 * or automatically by unregister_netdev() if the interface is still up at
 * module removal time.
 *
 * Mirror image of edunic_open(), in reverse: stop new traffic first, then
 * stop the hardware, then tear down the things that only make sense while
 * the hardware might raise an interrupt.
 */
int edunic_stop(struct net_device *netdev)
{
	struct edunic_adapter *adapter = netdev_priv(netdev);

	/* Prevent the stack from handing us any more packets to transmit. */
	netif_stop_queue(netdev);

	/* Stop TX/RX DMA activity and mask interrupts at the device *before*
	 * we free the IRQ / disable NAPI -- otherwise a still-armed device
	 * could raise an interrupt in the gap.
	 */
	edunic_hw_stop(adapter);

	/* free_irq() blocks until any currently-executing edunic_intr() call
	 * returns (synchronize_irq() semantics), so once this returns we are
	 * guaranteed no new napi_schedule() can be triggered by our own ISR.
	 */
	edunic_free_interrupts(adapter);

	/* napi_disable() blocks until any in-progress edunic_poll() call
	 * completes and prevents future scheduling. Safe to call now that
	 * nothing can trigger a new schedule.
	 */
	edunic_napi_disable(adapter);

	netif_carrier_off(netdev);

	return 0;
}

/**
 * edunic_change_mtu - net_device_ops .ndo_change_mtu
 *
 * The core network stack (dev_set_mtu()) already validates @new_mtu
 * against netdev->min_mtu/max_mtu before this is ever called, so by the
 * time we get here the value is known-legal. Because this driver's RX
 * buffers (EDUNIC_RX_BUF_SIZE, see nic.h) are sized to cover the entire
 * legal MTU range unconditionally, no ring reallocation is needed. A
 * driver whose buffer size scales with MTU (e.g. to avoid wasting memory
 * on small-MTU interfaces, or to support jumbo frames above what a single
 * fixed buffer can hold) would instead need to, here: stop the queue,
 * quiesce NAPI, tear down and reallocate the RX ring with new buffer
 * sizes, reprogram the relevant hardware register(s), and only then
 * restart -- i.e. a scaled-down version of ndo_stop()+ndo_open() around
 * just the RX ring.
 */
static int edunic_change_mtu(struct net_device *netdev, int new_mtu)
{
	netdev->mtu = new_mtu;
	netdev_dbg(netdev, "MTU changed to %d\n", new_mtu);
	return 0;
}

/**
 * edunic_set_mac_address - net_device_ops .ndo_set_mac_address
 *
 * eth_mac_addr() validates the new address and updates netdev->dev_addr;
 * we additionally push it into the device's MAC_LOW/MAC_HIGH registers so
 * hardware and the netdev agree, the same way probe() does for the
 * initial random address.
 */
static int edunic_set_mac_address(struct net_device *netdev, void *p)
{
	struct edunic_adapter *adapter = netdev_priv(netdev);
	int ret;

	ret = eth_mac_addr(netdev, p);
	if (ret)
		return ret;

	edunic_hw_set_mac(adapter, netdev->dev_addr);
	return 0;
}

/**
 * edunic_get_stats64 - net_device_ops .ndo_get_stats64
 *
 * Reads the per-adapter counters maintained by nic_tx.c/nic_rx.c using the
 * u64_stats_sync read-side loop, so a torn read racing a concurrent
 * u64_stats_update_begin/end pair on another CPU is retried rather than
 * observed half-updated (relevant on 32-bit hosts where a u64 store isn't
 * atomic; harmless-but-consistent on 64-bit).
 */
static void edunic_get_stats64(struct net_device *netdev,
				struct rtnl_link_stats64 *stats)
{
	struct edunic_adapter *adapter = netdev_priv(netdev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&adapter->stats.syncp);
		stats->tx_packets = adapter->stats.tx_packets;
		stats->tx_bytes = adapter->stats.tx_bytes;
		stats->tx_errors = adapter->stats.tx_errors;
		stats->rx_packets = adapter->stats.rx_packets;
		stats->rx_bytes = adapter->stats.rx_bytes;
		stats->rx_errors = adapter->stats.rx_errors;
		stats->rx_dropped = adapter->stats.rx_dropped;
	} while (u64_stats_fetch_retry(&adapter->stats.syncp, start));
}

const struct net_device_ops edunic_netdev_ops = {
	.ndo_open		= edunic_open,
	.ndo_stop		= edunic_stop,
	.ndo_start_xmit		= edunic_start_xmit,
	.ndo_change_mtu		= edunic_change_mtu,
	.ndo_set_mac_address	= edunic_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_stats64	= edunic_get_stats64,
};

/* --- minimal ethtool support -----------------------------------------
 * Enough to make `ethtool <iface>` and `ethtool -i <iface>` (see README
 * testing instructions) produce meaningful output. Ring/coalesce/offload
 * parameter get/set ops are intentionally omitted -- this driver has a
 * fixed ring size and no offloads to tune.
 */
static void edunic_get_drvinfo(struct net_device *netdev,
				struct ethtool_drvinfo *info)
{
	struct edunic_adapter *adapter = netdev_priv(netdev);

	strscpy(info->driver, EDUNIC_DRV_NAME, sizeof(info->driver));
	strscpy(info->bus_info, pci_name(adapter->pdev), sizeof(info->bus_info));
}

const struct ethtool_ops edunic_ethtool_ops = {
	.get_drvinfo	= edunic_get_drvinfo,
	.get_link	= ethtool_op_get_link,
};

/**
 * edunic_init_module / edunic_exit_module - kernel module entry/exit
 * points.
 *
 * pci_register_driver() does *not* itself find or bind any device; it
 * registers edunic_pci_driver (id table + probe/remove) with the PCI core,
 * which then walks its existing device list looking for matches (calling
 * edunic_probe() synchronously for each match found right here, before
 * pci_register_driver() returns) and remembers the table for any device
 * that appears later via hotplug.
 */
static int __init edunic_init_module(void)
{
	pr_info("%s: %s\n", EDUNIC_DRV_NAME, EDUNIC_DRV_DESC);
	return pci_register_driver(&edunic_pci_driver);
}

/**
 * pci_unregister_driver() walks every device currently bound to this
 * driver and calls edunic_remove() on each before returning, then removes
 * the driver from the PCI core's registry so it can no longer match new
 * devices. This is why module_exit() needs no explicit per-device cleanup
 * loop of its own -- it's handled inside this one call.
 */
static void __exit edunic_exit_module(void)
{
	pci_unregister_driver(&edunic_pci_driver);
}

module_init(edunic_init_module);
module_exit(edunic_exit_module);

MODULE_AUTHOR("linux_nic_lab");
MODULE_DESCRIPTION(EDUNIC_DRV_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
