#DIR=	/usr/src/linux-source-6.1
DIR=	/lib/modules/`uname -r`/build
obj-m := tcp_cubic_starlink.o
all:
	make -C $(DIR) M=$(PWD) modules
clean:
	make -C $(DIR) M=$(PWD) clean
