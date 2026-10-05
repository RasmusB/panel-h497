# H497TLB01 AMOLED on Raspberry Pi CM4: driver and overlay

Linux support for the AUO/Topwin **H497TLB01** 4.97" 720×1280 AMOLED panel (Raydium **RM69052** driver IC), connected over MIPI DSI to a Compute Module 4 (DSI1, 4 lanes).

**Status (2026-10-05): the panel shows an image directly on the CM4's DSI1**, 4 lanes at 120 Mbit/s per lane (about 18 Hz) so far. It took two fixes, and either fault alone kept the panel dark (see the [bring-up log for 2026-10-05](#bring-up-log-2026-10-05-root-causes-found)):
1. **The Pi's D-PHY prepare times were too long.** The hardware adds about one byte clock (8 UI) to the value vc4 programs. Fixed with the patched `vc4` options `dsi_cprep=0 dsi_hsprep=8`.
2. **The datasheet's D3 pinout is wrong.** Pins 27/28 are in the same order as the other pairs (P before N), not reversed. The prototype's D3 rework, done to match the datasheet, introduced the swap; the unpatched carrier board is correct.

---

## Remaining steps

- [x] ~~Decide: continue with this panel or switch.~~ Continuing: the panel works on the Pi (2026-10-05).
- [x] **Fix high-speed reception.** Done 2026-10-05: prepare times in `vc4` plus the D3 pinout (see the [bring-up log](#bring-up-log-2026-10-05-root-causes-found)).
- [ ] **Raise to 60 Hz** (`clock_khz=66900`, 429 Mbit/s per lane). Re-measure TCLK-PREPARE (CKN) and THS-PREPARE (D0N) there and pick `dsi_cprep`/`dsi_hsprep` so both are inside the D-PHY window. Still unknown whether the hardware overhead is a fixed ~64 ns or one byte clock (8 UI). Then try the continuous clock again.
- [ ] **Make the prepare fix permanent:** a proper `vc4` patch (candidate for the Raspberry Pi kernel), instead of the out-of-tree copy with module options.
- [ ] **Clean up the panel driver:** decide which bring-up options (`tc_exact`, `early_power`, `noncont`, …) stay, and set working defaults.
- [ ] **Remove the VBAT runaway risk:** the grey "runaway" seen on 2026-10-03 happened with corrupted video. Confirm VBAT current stays sane with real images at full brightness.
- [x] **Connect the breakout board** with the Pi powered off, then power on.
- [ ] **Check for an image.** The screen is black for about 6 s, then boot text and the login prompt appear, in landscape.
- [ ] **If the screen stays dark,** collect `dmesg | grep -iE 'h497|dsi'` and note whether the panel glows or flickers at all.
- [ ] **If the image rolls, tears or flickers,** try non-burst mode (see [Tuning](#tuning)), then adjust the timings if needed.
- [ ] **Check brightness control:** `echo 50 > /sys/class/backlight/panel-h497/brightness` should visibly dim the panel. If it doesn't, the RM69052 needs a manufacturer-specific brightness register instead of the standard `0x51`.
- [ ] **Check the console direction.** If text is upside down, change `fbcon=rotate:1` to `rotate:3` in `/boot/firmware/cmdline.txt`.
- [ ] **Try overlay rotation instead of `fbcon=rotate`.** Remove `fbcon=rotate:1` from `cmdline.txt` and use `dtoverlay=panel-h497,rotation=90` (or `270`). This also tells desktops which way the panel is mounted. Check which value gives the right direction.
- [x] **Installed driver 1.2 and the new overlay** (lanes/pins/rotation parameters) on the Pi.
- [x] **Breakout board power test** (no panel): VDDI and VDD measured correct when on and 0 V when switched off by the driver. HAT EEPROM answers at 0x50 and is empty (all `0xFF`).
- [x] **First boot with panel on 2 lanes** (`dtoverlay=panel-h497,lanes=2`, set 2026-10-03) because of the prototype's D3 polarity error. Run `./panel-check.sh`.
- [x] **Then test 4 lanes to see the failure mode.** Done: no difference, see the [bring-up log](#bring-up-log-2026-10-03-first-panel-tests). Expected: TE at about 60 Hz (LP commands only use lane 0), but no or garbled video. If 2 lanes also gives TE but no image, the RM69052 may need its lane count set by a register.
- [ ] **Next board revision:** route D3 like the other pairs and **ignore the datasheet's pin 27 = D3N / pin 28 = D3P**, which is wrong (proven 2026-10-05: the reworked board fails, the unpatched one works). Prototype VBAT is fed from 5 V, above the 4.5 V recommended maximum (abs max 5.5 V); production will use the LiPo.
- [ ] **Reboot into driver 1.2** and re-run the power-off test: `echo 4 | sudo tee /sys/class/graphics/fb0/blank`, then `echo 0 | ...`. `dmesg` should show no `sleep in failed`.
- [ ] **Program the HAT EEPROM** (product ID, vendor, and possibly an embedded overlay so the panel is set up automatically).
- [x] **Pushed to GitHub** (https://github.com/RasmusB/panel-h497). CI builds against Raspberry Pi OS bookworm (6.12) and trixie (6.18) kernels for Pi 4 and Pi 5 and passes.
- [ ] **Optional: bigger console font.** Run `sudo dpkg-reconfigure console-setup` and pick Terminus 16x32.
- [ ] **Touchscreen (Synaptics S3402) is not done yet.** See [Touchscreen](#touchscreen-not-done).
- [x] ~~After every kernel update, rebuild and reinstall the driver.~~ Now automatic through DKMS. Just check `dkms status` after a kernel upgrade (see [Kernel updates](#kernel-updates)).
- [ ] **Clean up** once everything works:
  - Remove `dtdebug=1` from `config.txt`.
  - Delete `/boot/firmware/overlays/panel-rm69052.dtbo` and the old `panel-rm69052.dts`.
- [ ] **Optional: submit the driver to mainline Linux** as `panel-raydium-rm69052.c`, with a YAML devicetree binding.

---

## Bring-up log (2026-10-05, root causes found)

Measured with a 300 MHz scope (single probe, short ground spring) at the panel FPC, between the D3 rework and the panel connector. 1 UI = 8.3 ns at 120 Mbit/s.

**Step 1: exact replay of the TC358870 (driver 1.13, `tc_exact=1`).** Same failure as before: clean until video, then "false control". Init sequence and command format are ruled out.

**Step 2: lane count and start-up order.**

| Setup | Result |
|---|---|
| 4 lanes, continuous clock | Error report `0x0040` (false control) |
| 1 lane, continuous clock | No errors, but HS writes don't arrive: panel receives nothing (clock lane never locked, panel powered after the clock started) |
| 1 lane, continuous clock, `early_power=1`, 429 Mbit/s | Clock locks, packets corrupted (`0x1F04`). No false control |
| 1 lane, 120 Mbit/s | SoT sync errors only (`0x0002`) |

So false control comes from lanes 1–3, and lane 0 + clock had a separate HS fault.

**Step 3: electrical measurements (1 lane, 120 Mbit/s).**

| Measurement | Pi (stock vc4) | TC358870 board | Notes |
|---|---|---|---|
| Ground offset CM4 ↔ panel | 0.77 mV | — | ruled out |
| CKP swing | 20–381 mV, sine-like | 60–264 mV at 210.5 MHz | Pi swing unchanged with panel held in reset → **clock termination never switched on** |
| Clock lane start (non-continuous) | TLPX 200 ns, **TCLK-PREPARE 128 ns** (programmed 8 UI ≈ 64 ns) | — | D-PHY max 95 ns. P drops before N: polarity correct |
| D0N | **THS-PREPARE 192 ns** (programmed 16 UI ≈ 128 ns) | — | D-PHY max 135 ns at 120 Mbit/s |

Both prepare times are about one byte clock (8 UI ≈ 64 ns) longer than vc4 programs. The hardware also ignores the lowest 3 bits of the prepare fields (`dsi_hsprep=4` reads back as 0), so only multiples of 8 UI are possible.

**Step 4: prepare fix (patched `vc4`, `dsi_cprep=0 dsi_hsprep=8`).**

| Measurement | Result |
|---|---|
| CKP | **52–250 mV, square**: termination now on, matches the TC358870 board |
| D0N | THS-PREPARE **120 ns**, HS 44–224 mV, clean: terminated |
| Panel errors, 1 lane | SoT sync gone; packets found but corrupted (`0x9F04`). The panel apparently ignores `BA` and always expects 4 lanes |
| Panel errors, 2 lanes | `0xBF04`, no false control → D1 fine |
| Panel errors, 4 lanes | `0x9B44`, **false control**; brief VBAT blip |
| Back to stock prepare (same boot) | SoT sync errors again (`0x0002`): **the vc4 fix is required** |

**Step 5: D2/D3.** Both D2P (pin 15) and D3P (pin 28) behave like P at the probe point: down at 0 V for ~326 ns, dip from N dropping at ~200 ns, HS-0 low (64–72 mV). D2 is terminated (HS 64/300 mV), but **D3 is not** (~420 mV): the panel doesn't accept the start sequence on D3 although the order at the probe matches the datasheet. So the panel's real pinout on 27/28 must be the reverse of the datasheet.

**Step 6: unpatched carrier board** (original D3 routing, same order as the other pairs), 4 lanes, 120 Mbit/s, prepare fix: **image on the panel**, 0 errors at the debug reads after video starts, power mode `9C`.

Reads during video still sometimes time out or return error-report bytes (for example `00 04` = checksum flag). Not yet clear whether that's real packet errors or the vc4 read path while video runs; to be checked at 60 Hz.

Configuration that works:
```
# /boot/firmware/config.txt
dtoverlay=panel-h497
# /etc/modprobe.d/panel-h497.conf
options panel-h497 debug=1 clock_khz=20000 early_power=1 noncont=1 burst=N early_display_on=1
# /etc/modprobe.d/vc4-dsi.conf (patched vc4 in /lib/modules/$(uname -r)/updates/)
options vc4 dsi_cprep=0 dsi_hsprep=8
```

## Bring-up log (2026-10-03, first panel tests)

| Test | Result |
|---|---|
| Panel ID (LP read `DA/DB/DC`) | `82 00 15`, stable: link, power and reset OK |
| Power mode `0A` after init | `98` after Sleep Out, `9C` after Display On: commands take effect |
| Self-diagnostics `0F` | `F0`: all OK |
| DSI error count `05` | 0 in every configuration |
| Scanline `45` | Stuck at 0: display timing never runs |
| All pixels on (`23`), framebuffer filled white | Panel stays black |
| VBAT current (5 V and 3.3 V) | 0 mA in all states: OLED supply never starts |
| 2 lanes at 1 Gbit/s, 2 lanes at 500 Mbit/s, sync pulse mode, 4 lanes at 429 Mbit/s | No change |
| **DCS write in LP mode** | **5/5 received** |
| **DCS write in HS mode** | **0/4 received** |

Later tests (2026-10-03, driver 1.5–1.8):

| Test | Result |
|---|---|
| Non-continuous clock (`noncont=1`) | Panel starts receiving HS traffic, but reports errors: ECC, checksum, data type, VC ID, EoT sync (`0x1F04`) |
| Same at 120 Mbit/s, 1 lane | SoT sync errors only (`0x0002`) |
| 1, 2 and 4 lanes (D3 reworked), 120–429 Mbit/s | Always corrupted packets, never a clean frame |
| `BA` register (assumed lane count), 7 values | No effect |
| THS-EXIT raised from 224 to 512 ns (datasheet asks for ≥ 300 ns) | No effect |
| Continuous clock, panel powered before the DSI host starts (`early_power=1`) | Same errors (`EoT sync`, `false control`) |
| Panel on the TC358870 HDMI board | **Works** (image wraps with a pink stripe on a new source; likely the source's HDMI timing) |

Evening tests (2026-10-03, driver 1.9–1.12, patched `vc4`):

| Test | Result |
|---|---|
| Init set: capture / datasheet verbatim / none (OTP defaults) | Same errors with all three: the init sequence is not the cause |
| Even `htotal` (`hbp=36`) | No change |
| Full-colour `kmstest` pattern on DSI-1 | Panel dark, same errors: not a black-frame problem |
| Panel driver: Display On and brightness sent before video (`early_display_on`, `init_brightness`) | Commands arrive; video still errors |
| Patched `vc4`: event mode (no HSE/VSE), EoT on, both, as the TC358870 | Same errors |
| Patched `vc4`: HBP+HFP as blanking packets (link in HS for nearly the whole line) | Panel lights with uniform grey (foam mura visible), **current climbs slowly towards 700 mA**, brightness commands can't be sent. Black or white framebuffer makes no difference. Interpreted as the driver IC in an undefined state, not as displayed frames. **Hazardous: stopped.** |
| Patched `vc4`: HBP-only blanking (as the TC358870) | Commands pass, panel dark, errors |
| D-PHY timings copied from the TC358870 (THS-PREPARE 56 → 93 ns, THS-ZERO 149 → 205 ns, TCLK-PRE/TRAIL/ZERO longer), then about double | Same errors |

TC358870 reference configuration (decoded from the timestamped capture and its datasheet):
- REFCLK 48 MHz, `MIPI_PLL_CONF 0x94AF` → **422.4 Mbit/s per lane**, 4 lanes.
- `FUNC_MODE 0x0161`: EoT packets on, continuous HS clock.
- `DSITX_MODE 0x81`: event mode, HSA/HBP as long blanking packets.
- `MODE_CONFIG 0x16 → 0x06`: commands in LP during setup, HS after video starts.
- Sequence: init in LP, Sleep Out, 300 ms, Display On, 40 ms, then video.
- D-PHY counters (18.94 ns each): LPX 3, TCLK-PREPARE 2, TCLK-ZERO 19, TCLK-PRE 2, TCLK-POST 10, TCLK-TRAIL 6, THS-PREPARE 4, THS-ZERO 10, THS-TRAIL 5, THS-EXIT 6.

Caveat (2026-10-05): the TC358870 bring-up notes read the same registers differently: `FUNC_MODE` bit 0 as EoTp *disabled*, and `HSYNC_WIDTH 0x7F` (≈ 40 px × 3 + header) as non-burst *sync pulses*. The register layouts aren't public, so neither reading is confirmed. Both variants were tested; the real link structure still needs to be measured at the panel FPC on the TC358870 board.

Driver 1.13 adds `tc_exact=1`, which replays the board exactly: every init command as a DCS long write followed by the `0x03` spacer, no `53`/`51`, Sleep Out (`11 00`), 300 ms, Display On (`29 00`), 40 ms, then video, with non-burst sync pulses and a continuous clock.

The stock `vc4` hard-codes pulse mode (`ST_END`), EoT off (`HSDT_EOT_DISABLE`) and LP stop per frame, ignoring the panel driver's mode flags. A patched copy with runtime options is kept in `vc4-patched/`. Since 2026-10-05 it is **installed** and also has `dsi_cprep`/`dsi_hsprep` for the prepare times.

**Conclusion of 2026-10-03 (superseded 2026-10-05):** HS packets arrived corrupted in every configuration. The D-PHY timing tests that day only made the prepare times *longer*, which made things worse; the real causes were the too-long prepare times and the D3 swap (see the [2026-10-05 log](#bring-up-log-2026-10-05-root-causes-found)).

**Earlier conclusion (before `noncont`):** the panel receives nothing in high-speed mode. Low-power signalling on lane 0 works, so lane 0 wiring is fine. Suspects, in order: clock lane (CKP/CKN, pins 21/22) polarity or routing, HS signal integrity on the breakout, a break in the clock pair. Check the PCB layout, not just the schematic, since the D3 swap was a routing error.

Also noted:
- The Pi's DSI driver returns 0 instead of the byte count for successful reads, so `mipi_dsi_dcs_get_power_mode()` reports `-ENODATA`. The driver reads registers directly to work around this.
- LP commands and reads sometimes time out while video is running. Consider retries in the driver.
- TE (GPIO 22) has no level shifting (1.8 V into a 3.3 V input): add a shifter in the next revision.

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
- **`disable()`** sends Display Off and Sleep In, then waits 120 ms, while the DSI link is still up.
- **`unprepare()`** asserts reset and turns off VDD, then VDDI. It sends no commands: because of `prepare_prev_first`, the DSI controller is already off by the time it runs. Driver 1.1 sent Sleep In here, which timed out.
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
