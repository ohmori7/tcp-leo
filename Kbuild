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

ifeq ($(NEW_CC),Y)
	ccflags-y += -DNEW_CC

	obj-m += tcp_bbrv3.o
	# for tcp_dctcp.h.
	CFLAGS_tcp_bbrv3.o += -I$(KDIR)/net/ipv4/

	obj-m += tcp_leo_bbrv3.o
	CFLAGS_tcp_leo_bbrv3.o += -I$(KDIR)/net/ipv4/
endif
