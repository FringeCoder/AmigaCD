# External module for the MiSTer's kernel. KDIR: a kernel tree of the same
# release, configured from the board's /proc/config.gz and modules_prepare'd
# (see build.sh in this directory).
obj-m := armwin.o

KDIR ?= /root/kmod/linux-6.18.38
CROSS_COMPILE ?= arm-none-linux-gnueabihf-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) clean
