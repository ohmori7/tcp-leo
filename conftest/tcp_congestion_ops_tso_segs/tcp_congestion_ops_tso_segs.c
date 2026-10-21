#include <linux/module.h>
#include <linux/kernel.h>
#include <net/tcp.h>

static int __init
conftest_init(void)
{

	return sizeof(((struct tcp_congestion_ops *)0)->tso_segs);
}

static void __exit
conftest_exit(void)
{
}

module_init(conftest_init);
module_exit(conftest_exit);

MODULE_LICENSE("GPL");
