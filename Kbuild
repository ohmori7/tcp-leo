# in-tree kernel variable.
obj-m := tcp_leo.o tcp_leo_cubic.o tcp_leo_bbrv1.o tcp_bbrv1.o
#
obj-m += tcp_sat_pipe_bbrv1.o
#
obj-m += tcp_illinois.o
#
obj-m += tcp_pcc_alegro.o
obj-m += tcp_pcc_vivace.o

# do not allow any warnings.
ccflags-y += -Werror

CFLAGS_tcp_leo_cubic.o := -DTCP_LEO_CUBIC
CFLAGS_tcp_leo_bbrv1.o := -DTCP_LEO_BBR

ifeq ($(NEW_CC),Y)
	obj-m += tcp_bbrv3.o
	ccflags-y += -DNEW_CC
	# for tcp_dctcp.h.
	CFLAGS_tcp_bbrv3.o += -I$(KDIR)/net/ipv4/
endif
