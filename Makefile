#
DIR=	/lib/modules/$(shell uname -r)/build
PWD=	$(shell pwd)
#
NEW_CC := $(shell							\
	$(MAKE) -C $(DIR) M=$(PWD)/conftest modules >/dev/null 2>&1 &&	\
	echo Y || echo N)
# out-of-tree rules.
all:
	make -C $(DIR) M=$(PWD) KDIR=$(DIR) NEW_CC=$(NEW_CC) modules
clean:
	make -C $(DIR) M=$(PWD) clean
