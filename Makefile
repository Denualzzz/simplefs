KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

.PHONY: all kernel user clean

all: kernel user

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules

user: user/simplefsctl

user/simplefsctl: user/simplefsctl.c include/simplefs_ioctl.h
	$(CC) $(CFLAGS) -Iinclude -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel clean
	rm -f user/simplefsctl
