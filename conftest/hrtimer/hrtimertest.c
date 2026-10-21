#include <linux/module.h>
#include <linux/kernel.h>

static enum hrtimer_restart
cb(struct hrtimer *h)
{

	return HRTIMER_NORESTART;
}

static int __init
conftest_init(void)
{
	struct hrtimer timer;

	hrtimer_setup(&timer, cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	return 0;
}

static void __exit
conftest_exit(void)
{
}

module_init(conftest_init);
module_exit(conftest_exit);

MODULE_LICENSE("GPL");
