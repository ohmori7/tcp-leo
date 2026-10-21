#
DIR=	/lib/modules/$(shell uname -r)/build
PWD=	$(shell pwd)
#
NEW_CC := $(shell							\
	$(MAKE) -C $(DIR) M=$(PWD)/conftest/tcp_congestion_ops_cong_control modules >/dev/null 2>&1 &&	\
	echo Y || echo N)
# hrtimer_setup() replaces hrtimer_init() since Linux 7.0.
HAVE_HRTIMER_SETUP := $(shell						\
	$(MAKE) -C $(DIR) M=$(PWD)/conftest/hrtimer modules >/dev/null 2>&1 &&	\
	echo Y || echo N)
# sk->tcp_retransmit_timer was introduced since Linux 7.0.
HAVE_SK_TCP_RETRANSMIT_TIMER := $(shell					\
	$(MAKE) -C $(DIR) M=$(PWD)/conftest/sk_tcp_retransmit_timer modules >/dev/null 2>&1 && \
	echo Y || echo N)
# tcp_congestion_ops.tso_segs was introduced only in Google BBR code.
HAVE_TCP_CONGESTION_OPS_TSO_SEGS := $(shell				\
	$(MAKE) -C $(DIR) M=$(PWD)/conftest/tcp_congestion_ops_tso_segs modules >/dev/null 2>&1 && \
	echo Y || echo N)
# out-of-tree rules.
all:
	make -C $(DIR) M=$(PWD) KDIR=$(DIR) NEW_CC=$(NEW_CC) HAVE_HRTIMER_SETUP=$(HAVE_HRTIMER_SETUP) HAVE_SK_TCP_RETRANSMIT_TIMER=$(HAVE_SK_TCP_RETRANSMIT_TIMER) HAVE_TCP_CONGESTION_OPS_TSO_SEGS=$(HAVE_TCP_CONGESTION_OPS_TSO_SEGS) modules
clean:
	make -C $(DIR) M=$(PWD) clean
