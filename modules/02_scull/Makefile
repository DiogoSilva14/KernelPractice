# Enable/Disable debugging by commenting
DEBUG = y

EXTRA_CFLAGS += -Wall -Wextra -Wno-unused-parameter
# Add debugging flags to CFLAGS
ifeq ($(DEBUG),y)
	EXTRA_CFLAGS += -O -g -DSCULL_DEBUG # "-O" is needed to expand inlines
endif

ifneq ($(KERNELRELEASE),)
	obj-m := scull.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KERNELDIR) M=$$PWD modules

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions
endif
