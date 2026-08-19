#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#define RX_RING_SIZE 64
#define RX_BUFFER_SIZE 2048

static struct net_device *my_dev;

struct my_rx_desc
{
    void *buf;
    dma_addr_t dma_addr;
    unsigned int len;
    bool done;
};

struct my_priv
{
    struct napi_struct napi;

    struct my_rx_desc rx_ring[RX_RING_SIZE];

    unsigned int rx_head;
}


/*
 * Interface UP olduğunda kernel bu fonksiyonu çağırır.
 */
static int my_open(struct net_device *dev)
{
    pr_info("simple_netdev: %s opened\n", dev->name);

    netif_start_queue(dev);

    return 0;
}


/*
 * Interface DOWN olduğunda kernel bu fonksiyonu çağırır.
 */
static int my_stop(struct net_device *dev)
{
    pr_info("simple_netdev: %s stopped\n", dev->name);

    netif_stop_queue(dev);

    return 0;
}

static void my_receive(
    struct sk_buff *skb,
    struct net_device *dev)
{
    skb->dev = dev;

    skb->protocol = eth_type_trans(
        skb,
        dev
    );

    skb->ip_summed = CHECKSUM_NONE;

    pr_info(
        "simple_netdev: RX packet on %s, len=%u\n",
        dev->name,
        skb->len
    );

    netif_rx(skb);
}

sstatic netdev_tx_t my_start_xmit(
    struct sk_buff *skb,
    struct net_device *dev)
{
    struct sk_buff *rx_skb;

    pr_info(
        "simple_netdev: TX packet on %s, len=%u\n",
        dev->name,
        skb->len
    );

    rx_skb = skb_clone(
        skb,
        GFP_ATOMIC
    );

    if(rx_skb != NULL)
    {
        my_receive(
            rx_skb,
            dev
        );
    }
    else
    {
        pr_err(
            "simple_netdev: skb_clone failed\n"
        );
    }

    dev_kfree_skb(skb);

    return NETDEV_TX_OK;
}

static int my_change_mtu(
    struct net_device *dev,
    int new_mtu)
{
    pr_info(
        "simple_netdev: %s MTU change: %u -> %d\n",
        dev->name,
        dev->mtu,
        new_mtu
    );

    if(new_mtu < 68 || new_mtu > 9000)
    {
        pr_err(
            "simple_netdev: invalid MTU: %d\n",
            new_mtu
        );

        return -EINVAL;
    }

    dev->mtu = new_mtu;

    return 0;
}


/*
 * Network device operasyonları.
 *
 * Kernel:
 *
 * interface UP   -> ndo_open
 * interface DOWN -> ndo_stop
 */
static const struct net_device_ops my_netdev_ops = {
    .ndo_open = my_open,
    .ndo_stop = my_stop,
    .ndo_start_xmit = my_start_xmit,
    .ndo_change_mtu = my_change_mtu,
};


static int __init simple_netdev_init(void)
{
    int ret;

    my_dev = alloc_etherdev(0);

    if(my_dev == NULL)
    {
        pr_err("simple_netdev: alloc_etherdev failed\n");
        return -ENOMEM;
    }

    strscpy(
        my_dev->name,
        "myeth%d",
        IFNAMSIZ
    );

    /*
     * Bizim callback tablomuzu
     * net_device'a bağlıyoruz.
     */
    my_dev->netdev_ops = &my_netdev_ops;

    ret = register_netdev(my_dev);

    if(ret < 0)
    {
        pr_err("simple_netdev: register_netdev failed\n");

        free_netdev(my_dev);
        return ret;
    }

    pr_info(
        "simple_netdev: registered interface %s\n",
        my_dev->name
    );

    return 0;
}

static int my_alloc_rx_ring(
    struct net_device *dev)
{
    struct my_priv *priv;
    int i;

    priv = netdev_pring(dev);

    for(i = 0; i < RX_RING_SIZE; i++)
    {
        priv->rx_ring[i].buf = 
            kmalloc(
                RX_BUF_SIZE,
                GFP_KERNEL
            );
        if(priv->rx_ring[i].buf == NULL)
            return -ENOMEM;

        priv->rx_ring[i].dma_addr =
            dma_map_single(
                dev->dev.parent,
                prix->rx_ring[i].buf,
                RX_BUF_SIZE,
                DMA_FROM_DEVICE
            );

        if(dma_mapping_error(
            dev->dev.parent,
            prix->rx_ring[i].dma_addr))
        {
            kfree(priv->rx_ring[i].buf);
            return -EIO;
        }

        priv->rx_ring[i].len = 0;
        priv->rx_ring[i].done = false;
        
    }
    priv->rx_head = 0;

    return 0;
}


static int my_poll(
    struct napi_struct *napi,
    int budget)
{
    struct my_priv *priv;
    struct net_device *dev;
    struct my_rx_desc *desc;
    struct sk_buff *skb;

    int work_done = 0;

    priv = container_of(
        napi,
        struct my_priv,
        napi
    );

    dev = napi->dev;

    while(work_done < budget)
    {
        desc = &priv->rx_ring[priv->rx_head];

        if(!desc->done)
            break;

        dma_sync_single_for_cpu(
            dev->dev.parent,
            desc->dma_addr,
            desc->len,
            DMA_FROM_DEVICE
        );

        skb = netdev_alloc_skb(
            dev,
            desc->len
        );

        if(skb != NULL)
        {
            memcpy(
                
                skb_put(skb, desc->len),
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

        netif_received_skb(skb);
    }

    desc->done = false;
    desc->len = 0;

    dma_sync_single_for_device(
            dev->dev.parent,
            desc->dma_addr,
            RX_BUF_SIZE,
            DMA_FROM_DEVICE
        );

        priv->rx_head =
            (priv->rx_head + 1)
            % RX_RING_SIZE;

        work_done++;
    }

    if(work_done < budget)
    {
        napi_complete_done(
            napi,
            work_done
        );

        /*
         * Gerçek driver:
         * RX interrupt tekrar enable
         */
    }

    return work_done;
}


static void __exit simple_netdev_exit(void)
{
    unregister_netdev(my_dev);
    free_netdev(my_dev);

    pr_info("simple_netdev: module unloaded\n");
}


module_init(simple_netdev_init);
module_exit(simple_netdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ahmet");
MODULE_DESCRIPTION("Simple virtual net_device example");