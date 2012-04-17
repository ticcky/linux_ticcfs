obj-m := ticcfs.o

default: ticcfs.c
	make -C /xdisk/devel/linux/linux-2.6.32.57 SUBDIRS=$(PWD) modules
	cp  ticcfs.ko /tmp/pos/

