#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/string.h>

static int __init skb_example_init(void)
{
    struct sk_buff *skb;
    unsigned char *ptr;

    pr_info("skb_example: module loaded\n");

    /*
     * 256 byte'lık bir skb data buffer ayır.
     *
     * Başlangıçta kabaca:
     *
     * head
     *  ↓
     *  +--------------------------------------+
     *  |             boş alan                 |
     *  +--------------------------------------+
     *  ↑
     * data
     * tail
     *
     * end buffer'ın sonunu gösterir.
     */
    skb = alloc_skb(256, GFP_KERNEL);

    if(skb == NULL)
    {
        pr_err("skb_example: alloc_skb failed\n");
        return -ENOMEM;
    }

    pr_info("After alloc_skb:\n");
    pr_info("  len      = %u\n", skb->len);
    pr_info("  headroom = %u\n", skb_headroom(skb));
    pr_info("  tailroom = %u\n", skb_tailroom(skb));

    /*
     * Baştan 32 byte boşluk bırak.
     *
     * head             data/tail
     *  ↓                  ↓
     *  +------------------+-------------------+
     *  |    HEADROOM      |      boş          |
     *  +------------------+-------------------+
     */
    skb_reserve(skb, 32);

    pr_info("After skb_reserve(32):\n");
    pr_info("  len      = %u\n", skb->len);
    pr_info("  headroom = %u\n", skb_headroom(skb));
    pr_info("  tailroom = %u\n", skb_tailroom(skb));

    /*
     * Packet'ın sonuna 7 byte ekle.
     *
     * skb_put() tail'i ileri götürür
     * ve skb->len'i artırır.
     */
    ptr = skb_put(skb, 7);

    memcpy(ptr, "merhaba", 7);

    /*
     * Şimdi:
     *
     * head       data               tail
     *  ↓          ↓                  ↓
     *  +----------+------------------+----------+
     *  | headroom |    merhaba       | tailroom |
     *  +----------+------------------+----------+
     */
    pr_info("After skb_put(7):\n");
    pr_info("  len      = %u\n", skb->len);
    pr_info("  data     = %.*s\n",
            skb->len,
            skb->data);

    /*
     * Packet'ın BAŞINA 4 byte ekle.
     *
     * skb_push() data pointer'ını geriye çeker.
     *
     * Önce:
     *
     *      data
     *       ↓
     *      [merhaba]
     *
     * Sonra:
     *
     *  data
     *   ↓
     *  [HDR!][merhaba]
     */
    ptr = skb_push(skb, 4);

    memcpy(ptr, "HDR!", 4);

    pr_info("After skb_push(4):\n");
    pr_info("  len  = %u\n", skb->len);
    pr_info("  data = %.*s\n",
            skb->len,
            skb->data);

    /*
     * Baştaki 4 byte'ı tekrar çıkar.
     *
     * skb_pull() data pointer'ını ileri götürür.
     *
     * [HDR!][merhaba]
     *         ↑
     *        data
     */
    skb_pull(skb, 4);

    pr_info("After skb_pull(4):\n");
    pr_info("  len  = %u\n", skb->len);
    pr_info("  data = %.*s\n",
            skb->len,
            skb->data);

    /*
     * Packet uzunluğunu 4 byte'a indir.
     *
     * "merhaba"
     *
     * ↓
     *
     * "merh"
     */
    skb_trim(skb, 4);

    pr_info("After skb_trim(4):\n");
    pr_info("  len  = %u\n", skb->len);
    pr_info("  data = %.*s\n",
            skb->len,
            skb->data);

    /*
     * skb artık kullanılmayacak.
     */
    kfree_skb(skb);

    pr_info("skb_example: skb freed\n");

    return 0;
}

static void __exit skb_example_exit(void)
{
    pr_info("skb_example: module unloaded\n");
}

module_init(skb_example_init);
module_exit(skb_example_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ahmet");
MODULE_DESCRIPTION("Simple sk_buff example");