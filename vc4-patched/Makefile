# Out-of-tree build of the Raspberry Pi vc4 driver (kernel commit
# c1432b4bae5b, 6.6.31+rpt) with DSI bring-up options in vc4_dsi.c.
vc4-y := vc4_bo.o vc4_crtc.o vc4_drv.o vc4_dpi.o vc4_dsi.o vc4_fence.o \
	vc4_firmware_kms.o vc4_kms.o vc4_gem.o vc4_hdmi.o vc4_hdmi_phy.o \
	vc4_vec.o vc4_hvs.o vc4_irq.o vc4_perfmon.o vc4_plane.o \
	vc4_render_cl.o vc4_trace_points.o vc4_txp.o vc4_v3d.o \
	vc4_validate.o vc4_validate_shaders.o vc4_debugfs.o
obj-m := vc4.o
ccflags-y := -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
