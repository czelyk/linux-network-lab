#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>

#include <linux/etherdevice.h>
#include <linux/ip.h>
#include <linux/inet.h>

#include <linux/tcp.h>
#include <linux/udp.h>


static unsigned char blocked_macs[][ETH_ALEN] = {
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
    {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01}
};


#define BLOCKED_MAC_COUNT \
    (sizeof(blocked_macs) / sizeof(blocked_macs[0]))


static struct nf_hook_ops nfho;

static __be32 blocked_ip;


/*
 * MAC adresi blacklist içerisinde mi?
 */
static bool is_mac_blocked(
    const unsigned char *mac)
{
    int i;

    for(i = 0; i < BLOCKED_MAC_COUNT; i++)
    {
        if(ether_addr_equal(
                mac,
                blocked_macs[i]))
        {
            return true;
        }
    }

    return false;
}


/*
 * Netfilter hook callback.
 */
static unsigned int mac_firewall_hook(
    void *priv,
    struct sk_buff *skb,
    const struct nf_hook_state *state)
{
    struct ethhdr *eth;
    struct iphdr *iph;


    /*
     * skb yoksa hiçbir şey yapamayız.
     */
    if(skb == NULL)
        return NF_ACCEPT;


    /*
     * Ethernet header.
     */
    eth = eth_hdr(skb);

    if(eth == NULL)
        return NF_ACCEPT;


    pr_info(
        "mac_firewall: SRC_MAC=%pM DST_MAC=%pM\n",
        eth->h_source,
        eth->h_dest
    );


    /*
     * L2 MAC blacklist kontrolü.
     */
    if(is_mac_blocked(eth->h_source))
    {
        pr_info(
            "mac_firewall: BLOCKED MAC SRC=%pM\n",
            eth->h_source
        );

        return NF_DROP;
    }


    /*
     * IPv4 header.
     */
    iph = ip_hdr(skb);

    if(iph == NULL)
        return NF_ACCEPT;


    pr_info(
        "mac_firewall: SRC_IP=%pI4 DST_IP=%pI4 PROTO=%u\n",
        &iph->saddr,
        &iph->daddr,
        iph->protocol
    );


    /*
     * L3 source IP blacklist kontrolü.
     */
    if(iph->saddr == blocked_ip)
    {
        pr_info(
            "mac_firewall: BLOCKED IP SRC=%pI4\n",
            &iph->saddr
        );

        return NF_DROP;
    }


    /*
     * TCP
     */
    if(iph->protocol == IPPROTO_TCP)
    {
        struct tcphdr *tcph;

        tcph = tcp_hdr(skb);

        if(tcph == NULL)
            return NF_ACCEPT;


        pr_info(
            "mac_firewall: TCP SRC_PORT=%u DST_PORT=%u\n",
            ntohs(tcph->source),
            ntohs(tcph->dest)
        );


        /*
         * TCP destination port 5001'i engelle.
         */
        if(ntohs(tcph->dest) == 5001)
        {
            pr_info(
                "mac_firewall: BLOCKED TCP DST PORT 5001\n"
            );

            return NF_DROP;
        }
    }


    /*
     * UDP
     */
    else if(iph->protocol == IPPROTO_UDP)
    {
        struct udphdr *udph;

        udph = udp_hdr(skb);

        if(udph == NULL)
            return NF_ACCEPT;


        pr_info(
            "mac_firewall: UDP SRC_PORT=%u DST_PORT=%u\n",
            ntohs(udph->source),
            ntohs(udph->dest)
        );
    }


    return NF_ACCEPT;
}


/*
 * Module init.
 */
static int __init mac_firewall_init(void)
{
    int ret;


    /*
     * Engellenecek örnek source IP.
     */
    blocked_ip = in_aton("10.10.3.60");


    /*
     * Netfilter hook tanımı.
     */
    nfho.hook = mac_firewall_hook;
    nfho.pf = PF_INET;
    nfho.hooknum = NF_INET_PRE_ROUTING;
    nfho.priority = NF_IP_PRI_FIRST;


    ret = nf_register_net_hook(
        &init_net,
        &nfho
    );


    if(ret)
    {
        pr_err(
            "mac_firewall: hook registration failed\n"
        );

        return ret;
    }


    pr_info(
        "mac_firewall: module loaded\n"
    );


    return 0;
}


/*
 * Module exit.
 */
static void __exit mac_firewall_exit(void)
{
    nf_unregister_net_hook(
        &init_net,
        &nfho
    );


    pr_info(
        "mac_firewall: module unloaded\n"
    );
}


module_init(mac_firewall_init);
module_exit(mac_firewall_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ahmet");
MODULE_DESCRIPTION(
    "Simple Netfilter MAC/IP/TCP/UDP firewall"
);