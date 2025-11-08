# in-tree kernel variable.
obj-m := tcp_leo.o tcp_leo_cubic.o tcp_leo_bbrv1.o tcp_bbrv1.o
#obj-m += tcp_bbrv3.o
obj-m +=  tcp_sat_pipe_bbrv1.o

CFLAGS_tcp_leo_cubic.o := -DTCP_LEO_CUBIC
CFLAGS_tcp_leo_bbrv1.o := -DTCP_LEO_BBR
#CFLAGS_tcp_bbrv3.o := -I$(SRC)/net/ipv4

# out-of-tree rules.
DIR=	/lib/modules/`uname -r`/build
all:
	make -C $(DIR) M=$(PWD) modules
clean:
	make -C $(DIR) M=$(PWD) clean
