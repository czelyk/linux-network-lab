#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

static struct net_device *my_dev;

static int __init simple_netdev_init(void)
{
    int ret;

    my_dev = alloc_etherdev(0);

    if(my_dev == NULL)
    {
        pr_err("Simple_netdev: alloc_etherdev failed\n");
        return -ENOMEM;
    }

    strscpy(
        my_dev->name,
        "myeth%d",
        IFNAMSIZ
    );

    ret = register_netdev(my_dev);
    
    if(ret < 0)
    {
        pr_err("Simple_netdev: register_netdev failed\n");

        free_netdev(my_dev);
        return ret;
    }

    pr_info(
        "simple_netdev: registered interface %s\n",
            my_dev->name
    );

    return 0;
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