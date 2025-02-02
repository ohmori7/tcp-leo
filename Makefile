DIR=	/lib/modules/`uname -r`/build
obj-m := tcp_leo_cubic.o
all:
	make -C $(DIR) M=$(PWD) modules
clean:
	make -C $(DIR) M=$(PWD) clean
