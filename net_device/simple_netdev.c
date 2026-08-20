#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/slab.h>


/* ============================================================
 * RING SETTINGS
 * ============================================================
 */

#define RX_RING_SIZE       64
#define TX_RING_SIZE       64

#define RX_BUFFER_SIZE     2048


/* ============================================================
 * HAYALİ NIC REGISTER MAP
 *
 * Gerçek NIC'te bunlar datasheet'ten gelir.
 * ============================================================
 */

#define REG_CONTROL             0x0000
#define REG_STATUS              0x0004

#define REG_RX_RING_BASE_LOW    0x0100
#define REG_RX_RING_BASE_HIGH   0x0104
#define REG_RX_TAIL             0x0108

#define REG_TX_RING_BASE_LOW    0x0200
#define REG_TX_RING_BASE_HIGH   0x0204
#define REG_TX_TAIL             0x0208

#define REG_IRQ_STATUS          0x0300
#define REG_IRQ_MASK            0x0304


/* IRQ status bits */

#define IRQ_RX                  BIT(0)
#define IRQ_TX                  BIT(1)


/* ============================================================
 * RX DESCRIPTOR
 *
 * Eğitim amaçlı descriptor.
 *
 * Gerçek NIC descriptor'ında:
 *
 * DMA address
 * length
 * status
 * command
 * ownership
 *
 * gibi hardware-defined alanlar bulunur.
 * ============================================================
 */

struct my_rx_desc
{
    void *buf;

    dma_addr_t dma_addr;

    unsigned int len;

    bool owned_by_hw;
    bool done;
};


/* ============================================================
 * TX DESCRIPTOR
 *
 * NOT:
 *
 * Gerçek hardware descriptor içinde struct sk_buff *
 * bulunmaz.
 *
 * Bu eğitim modelinde software metadata ile hardware
 * descriptor'ı tek struct içinde gösteriyoruz.
 * ============================================================
 */

struct my_tx_desc
{
    struct sk_buff *skb;

    dma_addr_t dma_addr;

    unsigned int len;

    bool owned_by_hw;
    bool done;
};


/* ============================================================
 * DRIVER PRIVATE DATA
 * ============================================================
 */

struct my_priv
{
    struct napi_struct napi;

    /*
     * NIC MMIO register area.
     */
    void __iomem *mmio;

    /*
     * Hardware IRQ.
     */
    int irq;


    /* ---------------- RX ---------------- */

    struct my_rx_desc *rx_ring;

    /*
     * NIC'in RX descriptor ring'i bulacağı DMA address.
     */
    dma_addr_t rx_ring_dma;

    unsigned int rx_head;


    /* ---------------- TX ---------------- */

    struct my_tx_desc *tx_ring;

    /*
     * NIC'in TX descriptor ring'i bulacağı DMA address.
     */
    dma_addr_t tx_ring_dma;

    /*
     * Yeni TX packet'ın konulacağı descriptor.
     */
    unsigned int tx_head;

    /*
     * Tamamlanmış descriptor'ların temizleneceği index.
     */
    unsigned int tx_clean;
};


/* ============================================================
 * TX RING FULL?
 * ============================================================
 */

static bool my_tx_ring_full(
    struct my_priv *priv)
{
    unsigned int next;

    next =
        (priv->tx_head + 1)
        % TX_RING_SIZE;

    /*
     * Bir slot boş bırakılan klasik ring-buffer modeli.
     */
    return next == priv->tx_clean;
}


/* ============================================================
 * TX CLEAN
 *
 * NIC TX'i tamamladıktan sonra çağrılır.
 * ============================================================
 */

static void my_tx_clean(
    struct net_device *dev)
{
    struct my_priv *priv;
    struct my_tx_desc *desc;

    priv = netdev_priv(dev);

    while(priv->tx_clean != priv->tx_head)
    {
        desc =
            &priv->tx_ring[priv->tx_clean];

        /*
         * Hardware hâlâ descriptor'ın sahibiyse
         * dokunma.
         */
        if(desc->owned_by_hw)
            break;

        /*
         * NIC completion status'unu henüz
         * vermediyse dur.
         */
        if(!desc->done)
            break;

        /*
         * Hardware'ın descriptor'a yaptığı önceki
         * yazılar CPU tarafından bundan sonra
         * gözlemlenmeli.
         */
        dma_rmb();

        /*
         * Packet'ın streaming DMA mapping'i artık
         * kullanılmıyor.
         */
        dma_unmap_single(
            dev->dev.parent,
            desc->dma_addr,
            desc->len,
            DMA_TO_DEVICE
        );

        /*
         * NIC packet ile işini bitirdi.
         * skb artık serbest bırakılabilir.
         */
        dev_kfree_skb(desc->skb);

        desc->skb = NULL;
        desc->dma_addr = 0;
        desc->len = 0;
        desc->done = false;
        desc->owned_by_hw = false;

        priv->tx_clean =
            (priv->tx_clean + 1)
            % TX_RING_SIZE;
    }

    /*
     * Ring daha önce dolduğu için queue durduysa
     * ve artık yer varsa tekrar başlat.
     */
    if(netif_queue_stopped(dev) &&
       !my_tx_ring_full(priv))
    {
        netif_wake_queue(dev);
    }
}


/* ============================================================
 * NDO_START_XMIT
 *
 * Linux network stack -> driver -> NIC
 * ============================================================
 */

static netdev_tx_t my_start_xmit(
    struct sk_buff *skb,
    struct net_device *dev)
{
    struct my_priv *priv;
    struct my_tx_desc *desc;

    priv = netdev_priv(dev);


    /* --------------------------------------------------------
     * 1. Ring dolu mu?
     * --------------------------------------------------------
     */

    if(my_tx_ring_full(priv))
    {
        netif_stop_queue(dev);

        /*
         * BUSY döndüğümüzde skb'yi tüketmiyoruz.
         */
        return NETDEV_TX_BUSY;
    }


    /* --------------------------------------------------------
     * 2. Sıradaki descriptor
     * --------------------------------------------------------
     */

    desc =
        &priv->tx_ring[priv->tx_head];


    /* --------------------------------------------------------
     * 3. skb data -> streaming DMA
     *
     * TX:
     *
     * RAM -> NIC
     * DMA_TO_DEVICE
     * --------------------------------------------------------
     */

    desc->dma_addr =
        dma_map_single(
            dev->dev.parent,
            skb->data,
            skb->len,
            DMA_TO_DEVICE
        );

    if(dma_mapping_error(
            dev->dev.parent,
            desc->dma_addr))
    {
        pr_err(
            "simple_netdev: TX DMA mapping failed\n"
        );

        dev_kfree_skb(skb);

        return NETDEV_TX_OK;
    }


    /* --------------------------------------------------------
     * 4. Descriptor doldur
     * --------------------------------------------------------
     */

    desc->skb = skb;
    desc->len = skb->len;

    desc->done = false;


    /*
     * Descriptor alanlarını hardware ownership
     * vermeden önce hazırla.
     */
    dma_wmb();


    /* --------------------------------------------------------
     * 5. Ownership -> NIC
     * --------------------------------------------------------
     */

    desc->owned_by_hw = true;


    /* --------------------------------------------------------
     * 6. Ring head ilerlet
     * --------------------------------------------------------
     */

    priv->tx_head =
        (priv->tx_head + 1)
        % TX_RING_SIZE;


    /* --------------------------------------------------------
     * 7. NIC'e haber ver: DOORBELL
     *
     * "TX ring'de yeni descriptor var."
     * --------------------------------------------------------
     */

    writel(
        priv->tx_head,
        priv->mmio + REG_TX_TAIL
    );


    pr_info(
        "simple_netdev: TX packet len=%u\n",
        skb->len
    );

    return NETDEV_TX_OK;
}


/* ============================================================
 * RX NAPI POLL
 * ============================================================
 */

static int my_poll(
    struct napi_struct *napi,
    int budget)
{
    struct my_priv *priv;
    struct net_device *dev;

    struct my_rx_desc *desc;
    struct sk_buff *skb;

    int work_done = 0;


    priv =
        container_of(
            napi,
            struct my_priv,
            napi
        );

    dev = napi->dev;


    while(work_done < budget)
    {
        desc =
            &priv->rx_ring[
                priv->rx_head
            ];


        /* ----------------------------------------------------
         * NIC hâlâ descriptor'ın sahibi mi?
         * ----------------------------------------------------
         */

        if(desc->owned_by_hw)
            break;


        /* ----------------------------------------------------
         * Packet tamamlandı mı?
         * ----------------------------------------------------
         */

        if(!desc->done)
            break;


        /*
         * Device'ın descriptor'a yaptığı yazıları
         * bundan sonra CPU doğru sırada görsün.
         */
        dma_rmb();


        /* ----------------------------------------------------
         * Streaming RX buffer:
         *
         * NIC -> CPU ownership
         * ----------------------------------------------------
         */

        dma_sync_single_for_cpu(
            dev->dev.parent,
            desc->dma_addr,
            desc->len,
            DMA_FROM_DEVICE
        );


        /* ----------------------------------------------------
         * skb oluştur
         * ----------------------------------------------------
         */

        skb =
            netdev_alloc_skb(
                dev,
                desc->len
            );

        if(skb != NULL)
        {
            memcpy(
                skb_put(
                    skb,
                    desc->len
                ),
                desc->buf,
                desc->len
            );


            skb->dev = dev;

            skb->protocol =
                eth_type_trans(
                    skb,
                    dev
                );

            skb->ip_summed =
                CHECKSUM_NONE;


            /*
             * Packet Linux network stack'e teslim edilir.
             */
            netif_receive_skb(skb);
        }


        /* ----------------------------------------------------
         * Descriptor refill
         * ----------------------------------------------------
         */

        desc->len = 0;
        desc->done = false;


        /*
         * RX buffer tekrar NIC kullanımına.
         */
        dma_sync_single_for_device(
            dev->dev.parent,
            desc->dma_addr,
            RX_BUFFER_SIZE,
            DMA_FROM_DEVICE
        );


        /*
         * Descriptor hazırlıkları NIC ownership'den
         * önce tamamlanmalı.
         */
        dma_wmb();


        /* ownership -> NIC */

        desc->owned_by_hw = true;


        /* ----------------------------------------------------
         * RX ring ilerle
         * ----------------------------------------------------
         */

        priv->rx_head =
            (priv->rx_head + 1)
            % RX_RING_SIZE;


        /*
         * Bazı NIC'lerde burada RX tail/doorbell
         * güncellemesi gerekir.
         */

        writel(
            priv->rx_head,
            priv->mmio + REG_RX_TAIL
        );


        work_done++;
    }


    /* --------------------------------------------------------
     * Ring boşaldıysa NAPI tamamla
     * --------------------------------------------------------
     */

    if(work_done < budget)
    {
        if(napi_complete_done(
                napi,
                work_done))
        {
            /*
             * RX polling bitti.
             *
             * RX + TX IRQ yeniden aktif.
             */
            writel(
                IRQ_RX | IRQ_TX,
                priv->mmio + REG_IRQ_MASK
            );
        }
    }


    return work_done;
}


/* ============================================================
 * INTERRUPT HANDLER
 * ============================================================
 */

static irqreturn_t my_irq_handler(
    int irq,
    void *dev_id)
{
    struct net_device *dev;
    struct my_priv *priv;

    u32 status;


    dev = dev_id;

    priv = netdev_priv(dev);


    /* --------------------------------------------------------
     * Interrupt neden geldi?
     * --------------------------------------------------------
     */

    status =
        readl(
            priv->mmio
            + REG_IRQ_STATUS
        );


    if(status == 0)
        return IRQ_NONE;


    /* --------------------------------------------------------
     * RX EVENT
     * --------------------------------------------------------
     */

    if(status & IRQ_RX)
    {
        /*
         * RX IRQ'yu geçici olarak kapat.
         *
         * NAPI polling yaparken her packet için
         * tekrar interrupt istemiyoruz.
         *
         * TX IRQ açık kalıyor.
         */

        writel(
            IRQ_TX,
            priv->mmio + REG_IRQ_MASK
        );


        napi_schedule(
            &priv->napi
        );
    }


    /* --------------------------------------------------------
     * TX COMPLETE EVENT
     * --------------------------------------------------------
     */

    if(status & IRQ_TX)
    {
        my_tx_clean(dev);
    }


    /* --------------------------------------------------------
     * IRQ acknowledge
     *
     * Bu davranış GERÇEK NIC'e göre değişir.
     *
     * Burada hayali olarak:
     *
     * write-1-to-clear
     *
     * varsayıyoruz.
     * --------------------------------------------------------
     */

    writel(
        status,
        priv->mmio + REG_IRQ_STATUS
    );


    return IRQ_HANDLED;
}


/* ============================================================
 * RX DESCRIPTOR RING ALLOCATION
 *
 * Descriptor ring:
 *
 * coherent DMA
 * ============================================================
 */

static int my_alloc_rx_desc_ring(
    struct net_device *dev)
{
    struct my_priv *priv;
    size_t size;

    priv = netdev_priv(dev);

    size =
        sizeof(struct my_rx_desc)
        * RX_RING_SIZE;


    priv->rx_ring =
        dma_alloc_coherent(
            dev->dev.parent,
            size,
            &priv->rx_ring_dma,
            GFP_KERNEL
        );


    if(priv->rx_ring == NULL)
        return -ENOMEM;


    memset(
        priv->rx_ring,
        0,
        size
    );


    priv->rx_head = 0;

    return 0;
}


/* ============================================================
 * TX DESCRIPTOR RING ALLOCATION
 * ============================================================
 */

static int my_alloc_tx_desc_ring(
    struct net_device *dev)
{
    struct my_priv *priv;
    size_t size;

    priv = netdev_priv(dev);

    size =
        sizeof(struct my_tx_desc)
        * TX_RING_SIZE;


    priv->tx_ring =
        dma_alloc_coherent(
            dev->dev.parent,
            size,
            &priv->tx_ring_dma,
            GFP_KERNEL
        );


    if(priv->tx_ring == NULL)
        return -ENOMEM;


    memset(
        priv->tx_ring,
        0,
        size
    );


    priv->tx_head = 0;
    priv->tx_clean = 0;

    return 0;
}


/* ============================================================
 * RX PACKET BUFFERS
 *
 * Packet data:
 *
 * streaming DMA
 * ============================================================
 */

static int my_alloc_rx_buffers(
    struct net_device *dev)
{
    struct my_priv *priv;
    int i;

    priv = netdev_priv(dev);


    for(i = 0; i < RX_RING_SIZE; i++)
    {
        struct my_rx_desc *desc;

        desc = &priv->rx_ring[i];


        desc->buf =
            kmalloc(
                RX_BUFFER_SIZE,
                GFP_KERNEL
            );

        if(desc->buf == NULL)
            goto err;


        desc->dma_addr =
            dma_map_single(
                dev->dev.parent,
                desc->buf,
                RX_BUFFER_SIZE,
                DMA_FROM_DEVICE
            );


        if(dma_mapping_error(
                dev->dev.parent,
                desc->dma_addr))
        {
            kfree(desc->buf);

            desc->buf = NULL;

            goto err;
        }


        desc->len = 0;
        desc->done = false;


        /*
         * Baştan NIC ownership.
         */
        desc->owned_by_hw = true;
    }


    return 0;


err:

    while(--i >= 0)
    {
        struct my_rx_desc *desc;

        desc = &priv->rx_ring[i];

        dma_unmap_single(
            dev->dev.parent,
            desc->dma_addr,
            RX_BUFFER_SIZE,
            DMA_FROM_DEVICE
        );

        kfree(desc->buf);

        desc->buf = NULL;
    }

    return -ENOMEM;
}


/* ============================================================
 * FREE RX PACKET BUFFERS
 * ============================================================
 */

static void my_free_rx_buffers(
    struct net_device *dev)
{
    struct my_priv *priv;
    int i;

    priv = netdev_priv(dev);


    if(priv->rx_ring == NULL)
        return;


    for(i = 0; i < RX_RING_SIZE; i++)
    {
        struct my_rx_desc *desc;

        desc =
            &priv->rx_ring[i];


        if(desc->buf == NULL)
            continue;


        dma_unmap_single(
            dev->dev.parent,
            desc->dma_addr,
            RX_BUFFER_SIZE,
            DMA_FROM_DEVICE
        );


        kfree(desc->buf);

        desc->buf = NULL;
    }
}


/* ============================================================
 * FREE TX PENDING BUFFERS
 * ============================================================
 */

static void my_free_pending_tx(
    struct net_device *dev)
{
    struct my_priv *priv;
    int i;

    priv = netdev_priv(dev);


    if(priv->tx_ring == NULL)
        return;


    for(i = 0; i < TX_RING_SIZE; i++)
    {
        struct my_tx_desc *desc;

        desc =
            &priv->tx_ring[i];


        if(desc->skb == NULL)
            continue;


        dma_unmap_single(
            dev->dev.parent,
            desc->dma_addr,
            desc->len,
            DMA_TO_DEVICE
        );


        dev_kfree_skb(desc->skb);

        desc->skb = NULL;
    }
}


/* ============================================================
 * FREE COHERENT DESCRIPTOR RINGS
 * ============================================================
 */

static void my_free_desc_rings(
    struct net_device *dev)
{
    struct my_priv *priv;

    size_t rx_size;
    size_t tx_size;


    priv = netdev_priv(dev);


    rx_size =
        sizeof(struct my_rx_desc)
        * RX_RING_SIZE;

    tx_size =
        sizeof(struct my_tx_desc)
        * TX_RING_SIZE;


    if(priv->rx_ring != NULL)
    {
        dma_free_coherent(
            dev->dev.parent,
            rx_size,
            priv->rx_ring,
            priv->rx_ring_dma
        );

        priv->rx_ring = NULL;
    }


    if(priv->tx_ring != NULL)
    {
        dma_free_coherent(
            dev->dev.parent,
            tx_size,
            priv->tx_ring,
            priv->tx_ring_dma
        );

        priv->tx_ring = NULL;
    }
}


/* ============================================================
 * HARDWARE INITIALIZATION
 * ============================================================
 */

static void my_hw_init(
    struct net_device *dev)
{
    struct my_priv *priv;

    priv = netdev_priv(dev);


    /* --------------------------------------------------------
     * RX descriptor ring DMA address
     * --------------------------------------------------------
     */

    writel(
        lower_32_bits(
            priv->rx_ring_dma
        ),
        priv->mmio
        + REG_RX_RING_BASE_LOW
    );


    writel(
        upper_32_bits(
            priv->rx_ring_dma
        ),
        priv->mmio
        + REG_RX_RING_BASE_HIGH
    );


    /* --------------------------------------------------------
     * TX descriptor ring DMA address
     * --------------------------------------------------------
     */

    writel(
        lower_32_bits(
            priv->tx_ring_dma
        ),
        priv->mmio
        + REG_TX_RING_BASE_LOW
    );


    writel(
        upper_32_bits(
            priv->tx_ring_dma
        ),
        priv->mmio
        + REG_TX_RING_BASE_HIGH
    );


    /* --------------------------------------------------------
     * Initial ring positions
     * --------------------------------------------------------
     */

    writel(
        priv->rx_head,
        priv->mmio + REG_RX_TAIL
    );


    writel(
        priv->tx_head,
        priv->mmio + REG_TX_TAIL
    );


    /*
     * Interrupt'ları probe sırasında kapalı tutuyoruz.
     *
     * Interface UP olunca my_open açacak.
     */
    writel(
        0,
        priv->mmio + REG_IRQ_MASK
    );
}


/* ============================================================
 * INTERFACE OPEN
 * ============================================================
 */

static int my_open(
    struct net_device *dev)
{
    struct my_priv *priv;

    priv = netdev_priv(dev);


    napi_enable(
        &priv->napi
    );


    /*
     * RX + TX interrupts enable.
     */
    writel(
        IRQ_RX | IRQ_TX,
        priv->mmio + REG_IRQ_MASK
    );


    netif_start_queue(dev);


    pr_info(
        "simple_netdev: %s opened\n",
        dev->name
    );


    return 0;
}


/* ============================================================
 * INTERFACE STOP
 * ============================================================
 */

static int my_stop(
    struct net_device *dev)
{
    struct my_priv *priv;

    priv = netdev_priv(dev);


    /*
     * Önce yeni TX packet kabul etme.
     */
    netif_stop_queue(dev);


    /*
     * NIC interruptlarını kapat.
     */
    writel(
        0,
        priv->mmio + REG_IRQ_MASK
    );


    napi_disable(
        &priv->napi
    );


    pr_info(
        "simple_netdev: %s stopped\n",
        dev->name
    );


    return 0;
}


/* ============================================================
 * CHANGE MTU
 * ============================================================
 */

static int my_change_mtu(
    struct net_device *dev,
    int new_mtu)
{
    if(new_mtu < 68 ||
       new_mtu > 9000)
    {
        return -EINVAL;
    }


    pr_info(
        "simple_netdev: %s MTU %u -> %d\n",
        dev->name,
        dev->mtu,
        new_mtu
    );


    dev->mtu = new_mtu;

    return 0;
}


/* ============================================================
 * NET DEVICE OPS
 * ============================================================
 */

static const struct net_device_ops my_netdev_ops =
{
    .ndo_open       = my_open,
    .ndo_stop       = my_stop,
    .ndo_start_xmit = my_start_xmit,
    .ndo_change_mtu = my_change_mtu,
};


/* ============================================================
 * PCI DEVICE TABLE
 *
 * HAYALİ ID!
 *
 * GERÇEK HARDWARE ID DEĞİLDİR.
 * ============================================================
 */

static const struct pci_device_id my_pci_ids[] =
{
    {
        PCI_DEVICE(
            0x1234,
            0x5678
        )
    },

    { 0, }
};


MODULE_DEVICE_TABLE(
    pci,
    my_pci_ids
);


/* ============================================================
 * PCI PROBE
 * ============================================================
 */

static int my_pci_probe(
    struct pci_dev *pdev,
    const struct pci_device_id *id)
{
    struct net_device *dev;
    struct my_priv *priv;

    int ret;


    /* --------------------------------------------------------
     * 1. PCI device enable
     * --------------------------------------------------------
     */

    ret =
        pci_enable_device(pdev);

    if(ret)
        return ret;


    /* --------------------------------------------------------
     * 2. PCI bus master enable
     *
     * DMA yapacak PCI device için önemli.
     * --------------------------------------------------------
     */

    pci_set_master(pdev);


    /* --------------------------------------------------------
     * 3. BAR resources claim
     * --------------------------------------------------------
     */

    ret =
        pci_request_regions(
            pdev,
            "simple_netdev"
        );

    if(ret)
        goto err_disable_device;


    /* --------------------------------------------------------
     * 4. DMA capability
     * --------------------------------------------------------
     */

    ret =
        dma_set_mask_and_coherent(
            &pdev->dev,
            DMA_BIT_MASK(64)
        );


    if(ret)
    {
        ret =
            dma_set_mask_and_coherent(
                &pdev->dev,
                DMA_BIT_MASK(32)
            );

        if(ret)
            goto err_release_regions;
    }


    /* --------------------------------------------------------
     * 5. Allocate net_device + private data
     * --------------------------------------------------------
     */

    dev =
        alloc_etherdev(
            sizeof(struct my_priv)
        );

    if(dev == NULL)
    {
        ret = -ENOMEM;

        goto err_release_regions;
    }


    SET_NETDEV_DEV(
        dev,
        &pdev->dev
    );


    pci_set_drvdata(
        pdev,
        dev
    );


    priv =
        netdev_priv(dev);


    priv->irq = pdev->irq;


    /* --------------------------------------------------------
     * 6. BAR0 -> MMIO
     * --------------------------------------------------------
     */

    priv->mmio =
        pci_iomap(
            pdev,
            0,
            0
        );


    if(priv->mmio == NULL)
    {
        ret = -ENOMEM;

        goto err_free_netdev;
    }


    /* --------------------------------------------------------
     * 7. net_device callbacks
     * --------------------------------------------------------
     */

    dev->netdev_ops =
        &my_netdev_ops;


    /* --------------------------------------------------------
     * 8. NAPI register
     * --------------------------------------------------------
     */

    netif_napi_add(
        dev,
        &priv->napi,
        my_poll
    );


    /* --------------------------------------------------------
     * 9. Coherent RX descriptor ring
     * --------------------------------------------------------
     */

    ret =
        my_alloc_rx_desc_ring(dev);

    if(ret)
        goto err_napi;


    /* --------------------------------------------------------
     * 10. Coherent TX descriptor ring
     * --------------------------------------------------------
     */

    ret =
        my_alloc_tx_desc_ring(dev);

    if(ret)
        goto err_rx_desc;


    /* --------------------------------------------------------
     * 11. Streaming RX buffers
     * --------------------------------------------------------
     */

    ret =
        my_alloc_rx_buffers(dev);

    if(ret)
        goto err_tx_desc;


    /* --------------------------------------------------------
     * 12. IRQ handler
     * --------------------------------------------------------
     */

    ret =
        request_irq(
            priv->irq,
            my_irq_handler,
            IRQF_SHARED,
            "simple_netdev",
            dev
        );

    if(ret)
        goto err_rx_buffers;


    /* --------------------------------------------------------
     * 13. Hardware registers
     * --------------------------------------------------------
     */

    my_hw_init(dev);


    /* --------------------------------------------------------
     * 14. MAC
     *
     * Eğitim driver'ı olduğu için random MAC.
     * --------------------------------------------------------
     */

    eth_hw_addr_random(dev);


    /* --------------------------------------------------------
     * 15. Register net_device
     * --------------------------------------------------------
     */

    ret =
        register_netdev(dev);

    if(ret)
        goto err_irq;


    pr_info(
        "simple_netdev: PCI network device registered as %s\n",
        dev->name
    );


    return 0;


/* ============================================================
 * PROBE ERROR CLEANUP
 *
 * Allocation'ın ters sırası.
 * ============================================================
 */

err_irq:

    writel(
        0,
        priv->mmio + REG_IRQ_MASK
    );

    free_irq(
        priv->irq,
        dev
    );


err_rx_buffers:

    my_free_rx_buffers(dev);


err_tx_desc:

    if(priv->tx_ring != NULL)
    {
        size_t tx_size;

        tx_size =
            sizeof(struct my_tx_desc)
            * TX_RING_SIZE;

        dma_free_coherent(
            &pdev->dev,
            tx_size,
            priv->tx_ring,
            priv->tx_ring_dma
        );

        priv->tx_ring = NULL;
    }


err_rx_desc:

    if(priv->rx_ring != NULL)
    {
        size_t rx_size;

        rx_size =
            sizeof(struct my_rx_desc)
            * RX_RING_SIZE;

        dma_free_coherent(
            &pdev->dev,
            rx_size,
            priv->rx_ring,
            priv->rx_ring_dma
        );

        priv->rx_ring = NULL;
    }


err_napi:

    netif_napi_del(
        &priv->napi
    );


    pci_iounmap(
        pdev,
        priv->mmio
    );


err_free_netdev:

    free_netdev(dev);

    pci_set_drvdata(
        pdev,
        NULL
    );


err_release_regions:

    pci_release_regions(pdev);


err_disable_device:

    pci_disable_device(pdev);

    return ret;
}


/* ============================================================
 * PCI REMOVE
 * ============================================================
 */

static void my_pci_remove(
    struct pci_dev *pdev)
{
    struct net_device *dev;
    struct my_priv *priv;


    dev =
        pci_get_drvdata(pdev);


    if(dev == NULL)
        return;


    priv =
        netdev_priv(dev);


    /*
     * Önce network subsystem'den çıkar.
     *
     * ndo_stop gerekiyorsa subsystem tarafından
     * çağrılır.
     */
    unregister_netdev(dev);


    /*
     * Hardware interrupt üretimini durdur.
     */
    writel(
        0,
        priv->mmio + REG_IRQ_MASK
    );


    /*
     * Gerçek driver'da burada ayrıca
     * NIC RX/TX DMA engines durdurulmalı.
     *
     * Hardware-specific CONTROL register işlemi
     * gerekir.
     */


    free_irq(
        priv->irq,
        dev
    );


    netif_napi_del(
        &priv->napi
    );


    /*
     * Önce streaming DMA.
     */
    my_free_pending_tx(dev);

    my_free_rx_buffers(dev);


    /*
     * Sonra coherent control memory.
     */
    my_free_desc_rings(dev);


    /*
     * MMIO mapping kaldır.
     */
    if(priv->mmio != NULL)
    {
        pci_iounmap(
            pdev,
            priv->mmio
        );

        priv->mmio = NULL;
    }


    free_netdev(dev);


    pci_set_drvdata(
        pdev,
        NULL
    );


    pci_release_regions(pdev);


    pci_disable_device(pdev);


    pr_info(
        "simple_netdev: PCI device removed\n"
    );
}


/* ============================================================
 * PCI DRIVER
 * ============================================================
 */

static struct pci_driver my_pci_driver =
{
    .name     = "simple_netdev",
    .id_table = my_pci_ids,
    .probe    = my_pci_probe,
    .remove   = my_pci_remove,
};


/* ============================================================
 * MODULE INIT / EXIT
 * ============================================================
 */

static int __init simple_netdev_init(void)
{
    return pci_register_driver(
        &my_pci_driver
    );
}


static void __exit simple_netdev_exit(void)
{
    pci_unregister_driver(
        &my_pci_driver
    );
}


module_init(simple_netdev_init);
module_exit(simple_netdev_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ahmet");
MODULE_DESCRIPTION(
    "Educational PCI Ethernet driver skeleton"
);