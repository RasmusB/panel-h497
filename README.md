# panel-h497

Linux driver and device tree overlay for the AUO/Topwin **H497TLB01** 4.97" 720×1280 AMOLED panel (Raydium RM69052) on Raspberry Pi, over MIPI DSI.

- Video mode, 60 Hz, 1–4 DSI lanes
- Brightness control via `/sys/class/backlight/panel-h497`
- Configurable lanes, rotation, DSI port and GPIOs through overlay parameters
- Rebuilt automatically on kernel updates (DKMS)

> **Status:** early development, not yet verified on hardware.

## Supported hardware

| Board | Status |
|---|---|
| Compute Module 4 (DSI1, 4 lanes) | Primary target |
| Raspberry Pi 4B (DSI1, 2 lanes) | Should work with `lanes=2`, untested |
| Raspberry Pi 5 / CM5 | Not yet supported (needs its own overlay) |

## Install

Requires Raspberry Pi OS (64-bit) with kernel headers (`linux-headers-rpi-v8`, normally installed).

```bash
sudo apt install --no-install-recommends dkms device-tree-compiler cpp

# Driver (DKMS)
VER=1.3
sudo mkdir -p /usr/src/panel-h497-$VER
sudo cp driver/{panel-h497.c,Makefile,dkms.conf} /usr/src/panel-h497-$VER/
sudo dkms install panel-h497/$VER

# Overlay
./compile-overlay.sh panel-h497.dts
sudo cp panel-h497.dtbo /boot/firmware/overlays/
```

Then add to `/boot/firmware/config.txt`, after `dtoverlay=vc4-kms-v3d`:

```
dtoverlay=panel-h497
```

Set `display_auto_detect=0` in the same file, then reboot.

## Overlay parameters

| Parameter | Default | Description |
|---|---|---|
| `dsi0` | off (DSI1) | Use the DSI0 port instead of DSI1 |
| `lanes=<n>` | `4` | Number of DSI data lanes wired (1–4). Pi 4B: `2` |
| `rotation=<deg>` | `0` | Panel mounting rotation: `0`, `90`, `180`, `270` |
| `reset_gpio=<n>` | `27` | Panel reset (RESX), active low |
| `vddi_gpio=<n>` | `18` | Enable for the 1.8 V VDDI regulator |
| `vdd_gpio=<n>` | `23` | Enable for the 3.1 V VDD regulator |

Example: `dtoverlay=panel-h497,lanes=2,rotation=90,reset_gpio=5`

## Brightness

```bash
echo 128 > /sys/class/backlight/panel-h497/brightness   # 0-255
```

Writable by members of the `video` group. Desktop brightness controls and `brightnessctl` work automatically.

## Timing tuning

Module parameters in `/etc/modprobe.d/panel-h497.conf`:

```
options panel-h497 burst=N clock_khz=66900 hfp=60 hsync=40 hbp=35 vfp=8 vsync=8 vbp=8
```

## Troubleshooting

```bash
dkms status                                     # driver built for the running kernel?
sudo vclog -m | grep -iE 'dterror|panel-h497'   # overlay applied?
dmesg | grep -iE 'h497|dsi'                     # driver probe / panel errors
cat /sys/class/drm/card*-DSI-*/status
```

## License

The driver is licensed under GPL-2.0 (see `LICENSE`). The overlay is dual-licensed GPL-2.0 or MIT.
