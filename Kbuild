#
obj-m := tcp_leo.o
obj-m += tcp_leo_cubic.o
CFLAGS_tcp_leo_cubic.o := -DTCP_LEO_CUBIC
obj-m += tcp_leo_bbrv1.o
CFLAGS_tcp_leo_bbrv1.o := -DTCP_LEO_BBR
#
obj-m += tcp_bbrv1.o
#
obj-m += tcp_sat_pipe_bbrv1.o
#
obj-m += tcp_illinois.o
#
obj-m += tcp_pcc_alegro.o
obj-m += tcp_pcc_vivace.o

# do not allow any warnings.
ccflags-y += -Werror

# new congestion control call back arguments at least since Linux 6.13.7.
ifeq ($(NEW_CC),Y)
	ccflags-y += -DNEW_CC
endif

# hrtimer_setup() replaces hrtimer_init() since Linux 7.0.
ifeq ($(HAVE_HRTIMER_SETUP),Y)
	ccflags-y += -DHAVE_HRTIMER_SETUP
endif

# sk->tcp_retransmit_timer was introduced since Linux 7.0.
ifeq ($(HAVE_SK_TCP_RETRANSMIT_TIMER),Y)
	ccflags-y += -DHAVE_SK_TCP_RETRANSMIT_TIMER
endif

# tcp_congestion_ops.tso_segs was introduced only in Google BBR code.
ifeq ($(HAVE_TCP_CONGESTION_OPS_TSO_SEGS),Y)
	ccflags-y += -DHAVE_TCP_CONGESTION_OPS_TSO_SEGS
endif

# build BBRv3 with Google BBR Linux 6.13.7.
# DCTCP is currently available with Google BBR code.
ifeq ($(HAVE_TCP_DCTCP),Y)
	obj-m += tcp_bbrv3.o
	# for tcp_dctcp.h.
	CFLAGS_tcp_bbrv3.o += -I$(KDIR)/net/ipv4/

	obj-m += tcp_leo_bbrv3.o
	CFLAGS_tcp_leo_bbrv3.o += -I$(KDIR)/net/ipv4/
endif
