# in-tree kernel variable.
obj-m := tcp_leo_cubic.o tcp_leo_bbrv1.o

# out-of-tree rules.
DIR=	/lib/modules/`uname -r`/build
all:
	make -C $(DIR) M=$(PWD) modules
clean:
	make -C $(DIR) M=$(PWD) clean
