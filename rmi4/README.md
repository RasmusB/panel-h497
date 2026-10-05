# rmi4-psiopi

Synaptics RMI4 touch driver for the S3402 on the H497 panel, built with DKMS.
The Raspberry Pi kernel doesn't enable `CONFIG_RMI4_*`, so the upstream
driver is vendored here.

- **Source:** `drivers/input/rmi4/` and `include/linux/rmi.h` from
  [raspberrypi/linux](https://github.com/raspberrypi/linux) branch `rpi-6.12.y`,
  commit `a553c4648f68caaa9fe246adb40269af771a0ee7` (2026-09-24). GPL-2.0, unchanged except as noted below.
- **Built:** core (bus, driver, F01), 2D sensor and **F12**, plus the I2C
  transport. The S3402BR reports F01, F12 and F34 (flash, not built).
- **Change (rmi_i2c.c):** optional `reset-gpios`. The line is held while the
  supplies come up, released after 10 ms, then `syna,startup-delay-ms` is
  waited. It is asserted again before the supplies are switched off (remove,
  suspend), so the reset pin never drives an unpowered controller.

Install:

```bash
V=1.1
sudo mkdir -p /usr/src/rmi4-psiopi-$V
sudo cp rmi4/{Makefile,dkms.conf,*.c,*.h} /usr/src/rmi4-psiopi-$V/
sudo cp -r rmi4/include /usr/src/rmi4-psiopi-$V/
sudo dkms install rmi4-psiopi/$V
```

If a future Raspberry Pi kernel enables RMI4 itself, remove this package.
