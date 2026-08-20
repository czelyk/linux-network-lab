// SPDX-License-Identifier: GPL-2.0
/*
 * nic_pci.c - PCI device lifecycle: probe() and remove().
 *
 * This is the file that turns "the PCI core found a device whose Vendor/
 * Device ID matches our table" into "there is a working net_device". Read
 * it top to bottom for probe(): every step is commented with what it does
 * and, critically, what has to be undone if a *later* step fails -- that
 * unwind discipline is one of the most interview-relevant parts of writing
 * a Linux driver correctly.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include "nic.h"

/*
 * PCI device ID table. This is what the kernel's PCI core matches new
 * devices against (both at coldplug/enumeration time and on hotplug) to
 * decide whether to call our probe(). See the big comment at the top of
 * nic.h: EDUNIC_VENDOR_ID/EDUNIC_DEVICE_ID are placeholders that cannot
 * match any real PCI device.
 */
static const struct pci_device_id edunic_pci_tbl[] = {
	{ PCI_DEVICE(EDUNIC_VENDOR_ID, EDUNIC_DEVICE_ID) },
	{ 0, } /* terminating entry required by the PCI core */
};
MODULE_DEVICE_TABLE(pci, edunic_pci_tbl);

/**
 * edunic_probe - called by the PCI core when a device matching
 * edunic_pci_tbl is found.
 *
 * Runtime position in the overall driver lifecycle (see README "Code
 * Reading Order" / guided walkthrough): module_init() has already called
 * pci_register_driver(); this is the per-device callback that fires once
 * per matching device found on the bus.
 */
static int edunic_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct net_device *netdev;
	struct edunic_adapter *adapter;
	void __iomem *hw_addr;
	int ret;

	/*
	 * pci_enable_device(): actually turns the device on at the PCI
	 * level -- enables its memory/IO decoding (so BAR accesses and DMA
	 * will actually reach it) and, on some platforms, assigns/validates
	 * IRQ routing. Nothing below this point is meaningful until this
	 * succeeds.
	 */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		return ret;
	}

	/*
	 * pci_set_master(): sets the PCI command register's Bus Master
	 * Enable bit. Without this, the device is not permitted to
	 * initiate DMA transfers at all -- every dma_alloc_coherent()/
	 * dma_map_single() transfer this driver relies on requires the
	 * device to be able to act as a bus master.
	 */
	pci_set_master(pdev);

	/*
	 * pci_request_regions(): reserves this device's PCI BARs in the
	 * kernel's resource tree so no other driver can claim them
	 * concurrently. Must happen before we ioremap any BAR.
	 */
	ret = pci_request_regions(pdev, EDUNIC_DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "pci_request_regions failed: %d\n", ret);
		goto err_disable_device;
	}

	/*
	 * pci_iomap(): maps BAR0 (a memory-mapped-I/O region) into the
	 * kernel's virtual address space and returns a __iomem-annotated
	 * pointer. This is the classic, explicit API; the modern managed
	 * equivalent is pcim_enable_device() + pcim_iomap_regions(), which
	 * ties the mapping's lifetime to the struct device and auto-cleans
	 * it up on driver detach. We use the explicit pci_* calls here
	 * deliberately, because seeing (and correctly unwinding) each
	 * resource acquisition by hand is the whole point of this file for
	 * a learning project -- the managed variant would hide exactly the
	 * unwind logic this file exists to demonstrate.
	 */
	hw_addr = pci_iomap(pdev, EDUNIC_BAR, EDUNIC_BAR_LEN);
	if (!hw_addr) {
		dev_err(&pdev->dev, "pci_iomap failed for BAR%d\n", EDUNIC_BAR);
		ret = -EIO;
		goto err_release_regions;
	}

	/*
	 * DMA mask setup: tells the kernel (and, on platforms with one, the
	 * IOMMU layer) the range of bus addresses this device is capable of
	 * generating/accepting, for both streaming (dma_map_single) and
	 * coherent (dma_alloc_coherent) mappings. We ask for 64-bit first
	 * since that's what a modern device would support and it avoids
	 * bounce-buffering on hosts with >4GB of RAM; a real driver
	 * targeting hardware that is genuinely 32-bit-DMA-only would only
	 * ever request DMA_BIT_MASK(32).
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "no usable DMA addressing mode\n");
			goto err_iounmap;
		}
	}

	/*
	 * alloc_etherdev(): allocates a struct net_device sized to also
	 * hold sizeof(struct edunic_adapter) of driver-private data
	 * immediately after it (retrieved later with netdev_priv()), and
	 * pre-fills Ethernet-specific defaults (netdev->type = ARPHRD_ETHER,
	 * netdev->hard_header_len = ETH_HLEN, broadcast address, ...).
	 */
	netdev = alloc_etherdev(sizeof(struct edunic_adapter));
	if (!netdev) {
		ret = -ENOMEM;
		goto err_iounmap;
	}

	/* Ties this net_device to our struct device for sysfs, power
	 * management, and so /sys/class/net/<iface>/device points at the
	 * PCI device.
	 */
	SET_NETDEV_DEV(netdev, &pdev->dev);

	adapter = netdev_priv(netdev);
	adapter->netdev = netdev;
	adapter->pdev = pdev;
	adapter->hw_addr = hw_addr;

	/* pci_get_drvdata()/pci_set_drvdata() is how remove() (and any
	 * power-management callback) gets back from struct pci_dev to our
	 * net_device.
	 */
	pci_set_drvdata(pdev, netdev);

	netdev->netdev_ops = &edunic_netdev_ops;
	netdev->ethtool_ops = &edunic_ethtool_ops;
	netdev->min_mtu = EDUNIC_MIN_MTU;
	netdev->max_mtu = EDUNIC_MAX_MTU;

	/* No link until the emulated PHY's autonegotiation timer fires --
	 * see edunic_hw_start()/edunic_link_timer_fn() in nic_hw.c. Setting
	 * this explicitly (rather than relying on the default) documents
	 * the intent and matches what real drivers do before link is
	 * confirmed.
	 */
	netif_carrier_off(netdev);

	edunic_hw_reset(adapter);

	/* No EEPROM/OTP fuse to read a burned-in address from -- generate a
	 * locally-administered random MAC (sets the U/L bit so it can never
	 * collide with a real vendor-assigned address) and program it into
	 * the device so hardware and netdev->dev_addr agree.
	 */
	eth_hw_addr_random(netdev);
	edunic_hw_set_mac(adapter, netdev->dev_addr);

	ret = edunic_setup_tx_resources(adapter);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate TX resources: %d\n",
			ret);
		goto err_free_netdev;
	}

	ret = edunic_setup_rx_resources(adapter);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate RX resources: %d\n",
			ret);
		goto err_free_tx;
	}

	/* Program ring base/length into the device, then post the initial
	 * set of RX buffers (count-1, not count -- see the ring-full/empty
	 * comment on edunic_tx_unused() in nic_tx.c; the same convention
	 * applies to the RX ring's head==tail "empty" sentinel used by
	 * edunic_hw_deliver_rx_frame() in nic_rx.c).
	 */
	edunic_hw_init_rings(adapter);
	if (!edunic_alloc_rx_buffers(adapter, adapter->rx_ring.count - 1)) {
		dev_err(&pdev->dev, "failed to allocate any RX buffers\n");
		ret = -ENOMEM;
		goto err_free_rx;
	}

	ret = edunic_hw_emulation_init(adapter);
	if (ret) {
		dev_err(&pdev->dev, "failed to init hw emulation: %d\n", ret);
		goto err_free_rx;
	}

	/* Bind NAPI to this net_device for its whole lifetime; enabled/
	 * disabled per open()/stop(), deleted here in remove().
	 */
	edunic_napi_add(adapter);

	/*
	 * register_netdev(): publishes the interface to the rest of the
	 * kernel and userspace -- from this call returning successfully,
	 * `ip link show` will list it, udev will see it, and (once an admin
	 * runs `ip link set up`) ndo_open() can be called. This is
	 * deliberately one of the very last things probe() does: everything
	 * the interface could need (rings, NAPI, hw reset done, MAC set) is
	 * ready before it becomes visible.
	 */
	ret = register_netdev(netdev);
	if (ret) {
		dev_err(&pdev->dev, "register_netdev failed: %d\n", ret);
		goto err_napi_del;
	}

	netdev_info(netdev, "%s, MAC %pM, BAR%d at 0x%llx mapped to %p\n",
		    EDUNIC_DRV_DESC, netdev->dev_addr, EDUNIC_BAR,
		    (unsigned long long)pci_resource_start(pdev, EDUNIC_BAR),
		    hw_addr);

	return 0;

	/*
	 * Error unwind, in exact reverse order of acquisition. Every label
	 * below undoes precisely the steps above it and nothing more, then
	 * falls through into undoing the step before that -- the standard
	 * Linux kernel "staircase" cleanup pattern.
	 */
err_napi_del:
	edunic_napi_del(adapter);
	edunic_hw_emulation_uninit(adapter);
err_free_rx:
	edunic_free_rx_resources(adapter);
err_free_tx:
	edunic_free_tx_resources(adapter);
err_free_netdev:
	free_netdev(netdev);
err_iounmap:
	pci_iounmap(pdev, hw_addr);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);
	return ret;
}

/**
 * edunic_remove - called when the device is unbound: module unload,
 * hot-unplug, or explicit unbind via sysfs.
 *
 * This is the mirror image of edunic_probe(), unwinding every resource in
 * the same reverse order the error path above uses -- which is exactly why
 * writing the error-unwind labels carefully in probe() pays off twice.
 */
static void edunic_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct edunic_adapter *adapter = netdev_priv(netdev);
	void __iomem *hw_addr = adapter->hw_addr;

	/*
	 * unregister_netdev(): removes the interface from userspace's view
	 * and, critically, if the interface is currently administratively
	 * up, this calls dev_close() internally which invokes our
	 * ndo_stop() -- so TX/RX hardware activity and interrupts are
	 * already stopped and NAPI already disabled by the time this
	 * returns. If the interface was never brought up, ndo_stop() was
	 * never needed because the device was never started in the first
	 * place (edunic_hw_start() only runs from ndo_open()).
	 */
	unregister_netdev(netdev);

	edunic_napi_del(adapter);
	edunic_hw_emulation_uninit(adapter);

	/* Free the RX/TX rings: this unmaps every outstanding streaming DMA
	 * mapping (dma_unmap_single() for any buffer still posted to
	 * hardware or still owned by an unsent/uncompleted skb) and frees
	 * both the packet buffers themselves and the coherent descriptor
	 * arrays.
	 */
	edunic_free_rx_resources(adapter);
	edunic_free_tx_resources(adapter);

	pci_iounmap(pdev, hw_addr);
	pci_release_regions(pdev);

	/* free_netdev() must come before pci_disable_device(): netdev_ops
	 * callbacks and sysfs attributes derived from the netdev must not
	 * be reachable after the underlying PCI device is torn down.
	 */
	free_netdev(netdev);

	pci_disable_device(pdev);
}

struct pci_driver edunic_pci_driver = {
	.name		= EDUNIC_DRV_NAME,
	.id_table	= edunic_pci_tbl,
	.probe		= edunic_probe,
	.remove		= edunic_remove,
};
