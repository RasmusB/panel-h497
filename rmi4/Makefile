# Synaptics RMI4 touch driver (I2C transport, F01 + F12) as an out-of-tree
# module, for Raspberry Pi kernels built without CONFIG_RMI4_*.
# Sources: see README.md.
obj-m := rmi_core.o rmi_i2c.o
rmi_core-y := rmi_bus.o rmi_driver.o rmi_f01.o rmi_2d_sensor.o rmi_f12.o

# The Kconfig symbols the sources test with #ifdef; include/ provides
# <linux/rmi.h>, which the kernel headers package does not ship.
ccflags-y := -DCONFIG_RMI4_2D_SENSOR -DCONFIG_RMI4_F12 -I$(src)/include

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
