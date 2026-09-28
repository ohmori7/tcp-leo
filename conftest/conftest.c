#include <linux/module.h>
#include <linux/kernel.h>
#include <net/tcp.h>

static void conftest_main(struct sock *sk, u32 ack, int flag,
    const struct rate_sample *rs)
{
}

static struct tcp_congestion_ops tcp_conftest_cong_ops __read_mostly = {
	.name           = "conftest",
	.owner          = THIS_MODULE,
	.cong_control   = conftest_main,
};

static int __init conftest_init(void)
{

	return tcp_register_congestion_control(&tcp_conftest_cong_ops);
}

static void __exit conftest_exit(void)
{
}

module_init(conftest_init);
module_exit(conftest_exit);

MODULE_LICENSE("GPL");
