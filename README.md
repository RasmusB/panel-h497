# panel-h497

Linux driver and device tree overlay for the AUO/Topwin **H497TLB01** 4.97" 720×1280 AMOLED panel (Raydium RM69052) on Raspberry Pi, over MIPI DSI.

- Video mode, 60 Hz, 1–4 DSI lanes
- Brightness control via `/sys/class/backlight/panel-h497`
- Configurable lanes, rotation, DSI port and GPIOs through overlay parameters
- Rebuilt automatically on kernel updates (DKMS)

> **Status:** working on a Compute Module 4 (DSI1, 4 lanes, 60 Hz) with the stock `vc4` driver. Note: the H497TLB01 datasheet swaps D3P/D3N (pins 27/28); route D3 like the other pairs.

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
VER=1.14
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
| `rotation=<deg>` | `270` | Panel mounting rotation: `0`, `90`, `180`, `270`. `270` is landscape as mounted in the PsioPi. Desktops and the console follow it; don't also set `fbcon=rotate` in `cmdline.txt`, which would override it |
| `reset_gpio=<n>` | `27` | Panel reset (RESX), active low |
| `vddi_gpio=<n>` | `18` | Enable for the 1.8 V VDDI regulator |
| `vdd_gpio=<n>` | `23` | Enable for the 3.1 V VDD regulator |

Example: `dtoverlay=panel-h497,lanes=2,rotation=90,reset_gpio=5`

## HAT ID EEPROM (PsioPi mainboard)

The PsioPi mainboard has a CAT24C512 HAT ID EEPROM on GPIO 0/1. With the overlay embedded in it, the firmware loads the panel overlay automatically and no `dtoverlay=` line is needed in `config.txt`. The driver (DKMS) still has to be installed.

```bash
hat/make-eeprom.sh            # build hat/psiopi.eep from hat/eeprom_settings.txt + panel-h497.dts
hat/make-eeprom.sh --flash    # also back up the EEPROM, write the image and verify it (needs sudo)
```

The image uses the classic (v1) HAT format: vendor `RasmusB`, product `PsioPi Mainboard`, product ID `0x0001`, version `0x0001` (v0.1). The embedded overlay always uses its defaults; for other parameters, use `dtoverlay=panel-h497,...` in `config.txt` instead. After changing the overlay, rebuild and flash the EEPROM again. The WP pin is left floating (internal pull-down), so the EEPROM is writable.

Check after a reboot:

```bash
cat /proc/device-tree/hat/vendor /proc/device-tree/hat/product; echo
sudo vclog -m | grep -i hat
```

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
