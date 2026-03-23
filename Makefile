# Makefile for x120x kernel module
#
# Out-of-tree build (development):
#   make
#   sudo insmod src/x120x.ko
#
# DKMS handles production builds automatically.

KVER    ?= $(shell uname -r)
KDIR    ?= /lib/modules/$(KVER)/build
PWD     := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/src clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules_install
	depmod -A

.PHONY: all clean install
