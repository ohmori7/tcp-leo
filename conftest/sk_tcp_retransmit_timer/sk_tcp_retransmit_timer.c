#include <linux/module.h>
#include <linux/kernel.h>
#include <net/sock.h>

static int __init
conftest_init(void)
{

	return sizeof(((struct sock *)0)->tcp_retransmit_timer);
}

static void __exit
conftest_exit(void)
{
}

module_init(conftest_init);
module_exit(conftest_exit);

MODULE_LICENSE("GPL");
