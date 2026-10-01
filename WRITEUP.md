# H497TLB01 AMOLED on Raspberry Pi CM4: driver and overlay

Linux support for the AUO/Topwin **H497TLB01** 4.97" 720×1280 AMOLED panel (Raydium **RM69052** driver IC), connected over MIPI DSI to a Compute Module 4 (DSI1, 4 lanes).

**Status (2026-10-01):** driver and overlay are installed and load cleanly. They have **not yet been tested with the actual panel**, because the breakout board hasn't arrived.

---

## Remaining steps

- [ ] **Connect the breakout board** with the Pi powered off, then power on.
- [ ] **Check for an image.** The screen is black for about 6 s, then boot text and the login prompt appear, in landscape.
- [ ] **If the screen stays dark,** collect `dmesg | grep -iE 'h497|dsi'` and note whether the panel glows or flickers at all.
- [ ] **If the image rolls, tears or flickers,** try non-burst mode (see [Tuning](#tuning)), then adjust the timings if needed.
- [ ] **Check brightness control:** `echo 50 > /sys/class/backlight/panel-h497/brightness` should visibly dim the panel. If it doesn't, the RM69052 needs a manufacturer-specific brightness register instead of the standard `0x51`.
- [ ] **Check the console direction.** If text is upside down, change `fbcon=rotate:1` to `rotate:3` in `/boot/firmware/cmdline.txt`.
- [ ] **Try overlay rotation instead of `fbcon=rotate`.** Remove `fbcon=rotate:1` from `cmdline.txt` and use `dtoverlay=panel-h497,rotation=90` (or `270`). This also tells desktops which way the panel is mounted. Check which value gives the right direction.
- [ ] **Install driver 1.1 and the new overlay** (lanes/pins/rotation parameters) on the Pi. See [Overlay parameters](#overlay-parameters).
- [x] **Pushed to GitHub** (https://github.com/RasmusB/panel-h497). CI builds against Raspberry Pi OS bookworm (6.12) and trixie (6.18) kernels for Pi 4 and Pi 5 and passes.
- [ ] **Optional: bigger console font.** Run `sudo dpkg-reconfigure console-setup` and pick Terminus 16x32.
- [ ] **Touchscreen (Synaptics S3402) is not done yet.** See [Touchscreen](#touchscreen-not-done).
- [x] ~~After every kernel update, rebuild and reinstall the driver.~~ Now automatic through DKMS. Just check `dkms status` after a kernel upgrade (see [Kernel updates](#kernel-updates)).
- [ ] **Clean up** once everything works:
  - Remove `dtdebug=1` from `config.txt`.
  - Delete `/boot/firmware/overlays/panel-rm69052.dtbo` and the old `panel-rm69052.dts`.
- [ ] **Optional: submit the driver to mainline Linux** as `panel-raydium-rm69052.c`, with a YAML devicetree binding.

---

## Background

The first two overlay attempts, written by another AI, couldn't work:

| Problem | Details |
|---|---|
| No driver | `compatible = "panel-mipi-dsi"` matches no driver in the kernel, and no driver reads a `dcs-init-sequence` property. The init commands would never be sent. |
| Command mode | `dsi,flags = <0>` selects DSI command mode. The Pi's DSI controller (vc4) only supports video mode. |
| Missing label | `panel-rm69052.dts` targeted `&dsi1_out`, which doesn't exist in the CM4 device tree. |
| No delays | Sleep Out and reset need delays of up to 120 ms. |

The fix was a small kernel driver for the panel, plus an overlay that describes the hardware and points at it.

## Sources of information

1. **Datasheet:** H497TLB01 v0.2, 2013, preliminary. It confirms:
   - RM69052 driver IC.
   - 720×1280, active area 61.92 × 110.08 mm.
   - Video mode at 60 Hz.
   - 4 DSI data lanes on the FPC connector.
   - VDDI 1.8 V and VDD 3.1 V supplies.
   - The power-on and reset sequence.
   - A preliminary init sequence.

   It does **not** contain porch or pixel-clock timings.

2. **I2C capture (`i2c-dump.csv`)** from a working HDMI→DSI converter board that uses a Toshiba **TC358870** bridge at I2C address 0x0F. The bridge sends panel commands from its own I2C registers, so the capture contains:
   - **The real init sequence**, as DCS packets written through registers 0x0500 and 0x0504. Where it differs from the datasheet, the capture wins, because that board is known to work.
   - **The EDID** the bridge presents over HDMI. Its timing descriptor gives the video timings: 66.90 MHz pixel clock; horizontal 720 + 60/40/35 and vertical 1280 + 8/8/8 (front porch / sync / back porch). That works out to exactly 60.0 Hz.
   - **DSI configuration:** 4 lanes (`LANE_ENABLE = 0x14`) in video mode.

   The bridge also sent an empty generic short write (type 0x03) between every command. It looks like a bridge artifact and is **omitted** from the driver.

3. **Board wiring**, confirmed against the carrier board:

| Signal | CM4 GPIO | Notes |
|---|---|---|
| VDDI 1.8 V LDO enable | 18 | Active high |
| VDD 3.1 V LDO enable | 23 | Active high |
| RESX (panel reset) | 27 | Active low |
| TE | 22 | Not used in video mode |
| DSI | DSI1 | 4 lanes |

## Files

All in `~/workspace/dts/`:

| File | Purpose |
|---|---|
| `driver/panel-h497.c` | The DRM panel driver (kernel module). |
| `driver/Makefile` | Builds the module against the installed kernel headers, for quick test builds. Installing is done through DKMS. |
| `driver/dkms.conf` | DKMS package definition (`panel-h497` version 1.0). |
| `panel-h497.dts` / `.dtbo` | Device tree overlay, display only, with parameters (see [Overlay parameters](#overlay-parameters)). |
| `README.md` | Customer-facing install and usage instructions. |
| `LICENSE` | GPL-2.0 |
| `.github/workflows/build.yml` | CI: builds the driver against the current Raspberry Pi OS kernels (bookworm and trixie; Pi 4 and Pi 5 kernels) and compiles the overlay, on every push and weekly. |
| `compile-overlay.sh` | Compiles a `.dts` into a `.dtbo`, running the preprocessor for `#include`s, and prints install instructions. |
| `i2c-dump.csv` | Raw I2C capture from the TC358870 board. |
| `panel-touch-h497.dts` | Earlier overlay with the touchscreen node. Reference only. |
| `panel-rm69052.dts` / `.dtbo` | First, broken attempt. Can be deleted. |

## How the driver works

The Pi's graphics driver (vc4) generates the video signal. The panel driver tells it how to power, configure and describe the panel. The kernel connects the two through the overlay's `compatible = "auo,h497tlb01"`.

- **`probe()`** runs at boot. It gets the two regulators and the reset GPIO, holding the panel in reset from the start. It sets up DSI: 4 lanes, RGB888, video mode with burst, commands in low-power mode. It sets `prepare_prev_first`, so the DSI link is up before commands are sent. Finally it registers the panel and the brightness device.
- **`get_modes()`** reports 720×1280 at 66.9 MHz with the EDID timings, and a physical size of 62 × 110 mm.
- **`prepare()`** runs the power-on sequence from the datasheet:
  1. Hold reset.
  2. Turn on VDDI, then VDD.
  3. Wait 40 ms.
  4. Pulse reset for 15 ms.
  5. Wait 120 ms for the chip to load its factory settings (OTP).
  6. Send the init table.
  7. Send `35 00` (TE on) and `53 20` (brightness control on).
  8. Send Sleep Out (`0x11`) and wait 120 ms.
- **`enable()`** sends Display On (`0x29`) once video is already streaming.
- **`disable()` / `unprepare()`** send Display Off, then Sleep In, wait 120 ms, assert reset, and turn off VDD, then VDDI.
- **Brightness** is a backlight device at `/sys/class/backlight/panel-h497`, range 0–255. Changes are sent as DCS `0x51 <value>`. Changes made while the panel is off are saved and applied when it powers on.
- **Module parameters** let you tune timings without rebuilding (see below).

### Init sequence (from the I2C capture)

```
Page 0: F0 55 AA 52 08 00 | B0 00 10 10 | BA 60 | BB 77×7
Page 2: F0 55 AA 52 08 02 | CA 04 | E1 00 | E2 0A | E3 40 | E7 00×4
        ED 48 00 E0 13 08 00 91 08 | FD 00 08 1C 00 00 01
        C3 11 24 04 0A 02 04 00 1C 10 F0 00
Page 3: F0 55 AA 52 08 03 | E0 00 | F1 00 00 00 00 00 15 | F6 08
Page 5: F0 55 AA 52 08 05 | C4 00 14 | C9 04
Page 1: F0 55 AA 52 08 01 | B0 06×3 | B1 14×3 | B2 00×3 | B4 66×3
        B5 44×3 | B6 54×3 | B7 24×3 | B9 04×3 | BA 14×3 | BE 32 38 78
Common: 35 00 (TE on) | 53 20 (brightness ctrl, added) | 11 (Sleep Out) | 29 (Display On)
```

## System changes

| What | Where | Backup |
|---|---|---|
| `dkms` package (installed with `--no-install-recommends`) | apt | n/a |
| Driver source registered with DKMS | `/usr/src/panel-h497-1.0/` | none (new) |
| Kernel module, built by DKMS | `/lib/modules/6.6.31+rpt-rpi-v8/updates/dkms/panel-h497.ko.xz` | none (new file) |
| Overlay | `/boot/firmware/overlays/panel-h497.dtbo` | none (new file) |
| Removed `dtoverlay=panel-rm69052`, added `dtoverlay=panel-h497` after `dtoverlay=vc4-kms-v3d`; `display_auto_detect=0` (was already set) | `/boot/firmware/config.txt` | `config.txt.bak-202610012156` |
| Landscape console: `fbcon=rotate:1` | `/boot/firmware/cmdline.txt` | `cmdline.txt.bak-202610012219` |

No udev rule was needed for brightness. The existing `/lib/udev/rules.d/60-backlight.rules` already makes `brightness` writable by the `video` group, and the user is in that group. systemd also saves and restores the brightness across reboots.

## Verified so far (without the panel)

| Check | Result |
|---|---|
| Overlay applied (`sudo vclog -m \| grep -i dt`) | Loaded, no `dterror` |
| DSI device | `fe700000.dsi.0` exists, `panel_h497` bound |
| vc4 | `bound fe700000.dsi` |
| `/sys/class/drm/card1-DSI-1` | `connected`, `enabled`, mode `720x1280` |
| Regulators and GPIOs | `panel_vddi` and `panel_vdd` on; GPIO 18, 23 and 27 high; GPIO 22 input |
| Driver errors in `dmesg` | None |
| Brightness | `/sys/class/backlight/panel-h497` present; writes work without sudo |
| Console | 160×45 characters, rotated |

Without a panel attached, successful commands prove nothing. They are sent in low-power mode without waiting for a reply.

## Overlay parameters

From driver 1.1. The driver reads `dsi-lanes` (default 4) and the standard `rotation` property from the device tree.

| Parameter | Default | Description |
|---|---|---|
| `dsi0` | off (DSI1) | Use the DSI0 port |
| `lanes=<n>` | `4` | DSI data lanes, 1–4. Pi 4B display connector: `2` |
| `rotation=<deg>` | `0` | Mounting rotation `0`/`90`/`180`/`270`. Sets the DRM panel orientation, which the console and desktops follow. |
| `reset_gpio=<n>` | `27` | RESX, active low |
| `vddi_gpio=<n>` | `18` | VDDI 1.8 V LDO enable |
| `vdd_gpio=<n>` | `23` | VDD 3.1 V LDO enable |

Verified by merging the overlay into `bcm2711-rpi-cm4.dtb` with `dtmerge`, with and without each parameter.

Bandwidth note: 720×1280 at 60 Hz in RGB888 needs about 1.6 Gbit/s, so roughly 800 Mbit/s per lane on 2 lanes. That's near the Pi 4's DSI limit. If 2 lanes is unstable, lower `clock_khz` for a lower refresh rate.

## Tuning

Module parameters, set in `/etc/modprobe.d/panel-h497.conf`, then reboot:

```
options panel-h497 burst=N
options panel-h497 clock_khz=66900 hfp=60 hsync=40 hbp=35 vfp=8 vsync=8 vbp=8
```

| Parameter | Default | Meaning |
|---|---|---|
| `burst` | `Y` | `N` switches to non-burst mode with sync pulses. Try this first if the image is unstable. |
| `clock_khz` | 66900 | Pixel clock |
| `hfp` / `hsync` / `hbp` | 60 / 40 / 35 | Horizontal front porch / sync / back porch |
| `vfp` / `vsync` / `vbp` | 8 / 8 / 8 | Vertical front porch / sync / back porch |

## Useful commands

```bash
# Driver status under DKMS
dkms status

# Recompile and install the overlay
cd ~/workspace/dts && ./compile-overlay.sh panel-h497.dts
sudo cp panel-h497.dtbo /boot/firmware/overlays/

# Diagnostics
sudo vclog -m | grep -iE 'dterror|panel-h497'
dmesg | grep -iE 'h497|dsi|vc4'
cat /sys/class/drm/card*-DSI-1/{status,enabled,modes}
pinctrl get 18,22,23,27

# Brightness
echo 128 > /sys/class/backlight/panel-h497/brightness
```

## Kernel updates

The driver is an **out-of-tree module**, which has to be compiled for each exact kernel version. **DKMS** does this automatically: when `apt` installs a new kernel, the `/etc/kernel/postinst.d/dkms` hook rebuilds and installs the driver for it. The matching headers come with the new kernel through the `linux-headers-rpi-v8` package.

After a kernel upgrade, before rebooting, confirm the new kernel shows `installed`:

```bash
dkms status
# panel-h497/1.0, <new-kernel-version>, aarch64: installed
```

If the build failed, for example because a future kernel changed the display API, the panel stays dark until the driver is fixed. The log is in `/var/lib/dkms/panel-h497/1.0/build/make.log`.

**Changing the driver:** DKMS builds from its own copy in `/usr/src/panel-h497-1.0/`, not from `~/workspace/dts/driver/`. After editing the driver, bump `PACKAGE_VERSION` in `dkms.conf`, then:

```bash
V=1.1   # new version
sudo dkms remove panel-h497/1.0 --all
sudo mkdir -p /usr/src/panel-h497-$V
sudo cp ~/workspace/dts/driver/{panel-h497.c,Makefile,dkms.conf} /usr/src/panel-h497-$V/
sudo dkms install panel-h497/$V
```

## Touchscreen (not done)

The S3402 touch controller is supported by the kernel's RMI4 drivers (`syna,rmi4-i2c`). The datasheet gives I2C address **0x20**. The draft node in `panel-touch-h497.dts` assumes:
- the `i2c_csi_dsi` bus;
- TP_INT on GPIO 25 (falling edge);
- TP_RESX on GPIO 24.

Those pins haven't been checked against the carrier schematic. The node also needs the TP_VCC (3.1 V) and TP_VDDI (1.8 V) supplies to be on.

## Undo everything

1. In `/boot/firmware/config.txt`, remove `dtoverlay=panel-h497`, or restore `config.txt.bak-202610012156`.
2. In `/boot/firmware/cmdline.txt`, remove ` fbcon=rotate:1`, or restore `cmdline.txt.bak-202610012219`.
3. Remove the driver and the overlay:
   ```bash
   sudo dkms remove panel-h497/1.0 --all
   sudo rm -r /usr/src/panel-h497-1.0
   sudo rm /boot/firmware/overlays/panel-h497.dtbo
   ```
4. Reboot.

If the Pi won't boot, mount the boot partition on another machine and restore the backups.
