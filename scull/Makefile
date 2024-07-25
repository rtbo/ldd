ifneq ($(KERNELRELEASE),)
	obj-m := scull.o
else
	KDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

$(info KDIR = $(KDIR))

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

bear:
	bear --append --output $(PWD)/../.vscode/compile_commands.json -- $(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif