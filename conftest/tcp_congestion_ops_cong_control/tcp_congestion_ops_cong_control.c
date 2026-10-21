#include <linux/module.h>
#include <linux/kernel.h>
#include <net/tcp.h>

static void tcp_congestion_ops_cong_control_test_main(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
{
}

static struct tcp_congestion_ops tcp_tcp_congestion_ops_cong_control_test_cong_ops __read_mostly = {
	.name           = "tcp_congestion_ops_cong_control_test",
	.owner          = THIS_MODULE,
	.cong_control   = tcp_congestion_ops_cong_control_test_main,
};

static int __init tcp_congestion_ops_cong_control_test_init(void)
{

	return tcp_register_congestion_control(&tcp_tcp_congestion_ops_cong_control_test_cong_ops);
}

static void __exit tcp_congestion_ops_cong_control_test_exit(void)
{
}

module_init(tcp_congestion_ops_cong_control_test_init);
module_exit(tcp_congestion_ops_cong_control_test_exit);

MODULE_LICENSE("GPL");
