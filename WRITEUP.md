# H497TLB01 AMOLED on Raspberry Pi CM4: driver and overlay

Linux support for the AUO/Topwin **H497TLB01** 4.97" 720×1280 AMOLED panel (Raydium **RM69052** driver IC), connected over MIPI DSI to a Compute Module 4 (DSI1, 4 lanes).

**Status (2026-10-05): the panel works directly on the CM4's DSI1 at 720×1280, 60 Hz, 4 lanes, with the stock `vc4` driver.** Driver 1.14 needs no module options; blank/unblank and brightness work. Two faults were found (see the [bring-up log](#bring-up-log-2026-10-05-root-causes-found)):
1. **Datasheet v0.2's D3 pinout is wrong.** Pins 27/28 are in the same order as the other pairs (P before N), not reversed. AUO corrected this in datasheet rev 0.6 (2014-01-08); see [Datasheet v1.8](#datasheet-v18-2015). The prototype's D3 rework, done to match the datasheet, introduced the swap; the unpatched carrier board is correct. **This was the fault that blocked 60 Hz.**
2. **The Pi's D-PHY prepare times are too long at low bit rates.** The hardware adds one byte clock (8 UI) to the value vc4 programs. At 120 Mbit/s that puts TCLK-/THS-PREPARE out of spec (the panel can't sync); at 60 Hz (429 Mbit/s) the stock values land inside the window. Only matters for low refresh rates; the patched `vc4` in `vc4-patched/` (`dsi_cprep`/`dsi_hsprep`) fixes it. Worth reporting to Raspberry Pi.

---

## Remaining steps

- [x] ~~Decide: continue with this panel or switch.~~ Continuing: the panel works on the Pi (2026-10-05).
- [x] **Fix high-speed reception.** Done 2026-10-05: prepare times in `vc4` plus the D3 pinout (see the [bring-up log](#bring-up-log-2026-10-05-root-causes-found)).
- [x] **Raise to 60 Hz.** Works with the stock `vc4`; the hardware overhead is one byte clock, so stock prepare times are in spec at 429 Mbit/s.
- [x] **Clean up the panel driver defaults** (1.14): non-continuous clock, `early_power`, burst, `early_display_on` on; `debug` off; initial brightness = backlight level. No `/etc/modprobe.d/panel-h497.conf` needed.
- [ ] **Report the low-rate prepare-time issue** to the Raspberry Pi kernel (measurements in the bring-up log), optionally with a `vc4` patch that accounts for the one-byte-clock overhead.
- [x] **Checksum flags during video: fixed by the v1.8 init (driver 1.16, 2026-10-05).** With the capture-based init, the panel reported a checksum error (`0x0400`) in every ~50 ms interval while video ran, yet the image was correct (0 clean answers in 340 ID reads, independent of framebuffer content; earlier suspicion: vc4's pixel-packet CRC). The datasheet v1.8 init stops it: 100 ID reads at 50 ms give **86–93 clean replies (`82 00`) and no error reports**, versus 0 clean with the capture (the rest are vc4 timeouts, see below). The panel's DSI error counter (DCS `05`) stays at 0 over 5 s, and DCS reads during video now return data (power mode `9C`). Bisected through debugfs on top of the capture init: only **page 2 `EA`** matters. Any write of all five bytes with bytes 3–5 zero, or with a single bit set in one of them, stops the reports; bytes 1–2 don't matter; a 1- or 2-byte write has no effect; `EA 7F 20 FF FF FF` swaps the checksum flag for EoT sync/ECC flags (`0x0304`). The register is undocumented, so it may configure error checking or reporting rather than fix a real link error; either way the image and current are unchanged (74 vs 75 mA, white at brightness 128).
- [ ] **Reads during video are unreliable** on vc4 (timeouts, wrong values). Only matters for debugging.
- [x] **Commands during video time out now and then** (2026-10-05, kernel 6.12): vc4 slips low-power commands into the running video and gives up after 500 ms (`DSI transfer failed whilst in HS mode stat: 0x00020003`, `-ETIMEDOUT`); 3 of 40 brightness writes failed. At one boot `systemd-backlight` failed to restore the brightness twice in a row. Driver 1.15 retries commands sent during video (brightness, Display On/Off, Sleep In) up to 3 times on `-ETIMEDOUT`.
- [ ] **Remove the VBAT runaway risk:** the grey "runaway" seen on 2026-10-03 happened with corrupted video. Confirm VBAT current stays sane with real images at full brightness.
- [x] **Connect the breakout board** with the Pi powered off, then power on.
- [x] **Image** (2026-10-05): boot text and login prompt, landscape, 60 Hz.
- [x] **Brightness control:** the standard DCS `0x51` works; `/sys/class/backlight/panel-h497` dims the panel.
- [x] **Rotation** only through the overlay (`rotation=270`, the default); `fbcon=rotate` removed from `cmdline.txt`. Console and desktops follow the DRM panel orientation.
- [x] **Kernel update** (2026-10-05): `apt full-upgrade` from 6.6.31 to 6.12.109. DKMS rebuilt the driver for both kernels (`rpi-v8` and `rpi-2712`) during the upgrade; after reboot image, HAT overlay, rotation, brightness and blank/unblank all work, no DSI errors.
- [x] **Removed the 6.6.31 fallback** (2026-10-05): `/boot/firmware/kernel8-6.6.31.img`, `initramfs8-6.6.31`, `/root/kernel-fallback-6.6.31/`. The 6.6.31 kernel, header and kbuild packages were purged too; DKMS removed its 6.6.31 builds through its kernel hook.
- [x] **Removed leftovers of older systems** (2026-10-05): unowned module folders from earlier custom kernels (`6.6.45-v8*`, `6.6.47-v8*`), `/boot/firmware/kernel8-backup.img`, old files in `/boot` (DTBs, `kernel8-backup.img`, `firmware.bak`), and the legacy packages `raspberrypi-kernel`, `raspberrypi-bootloader` (arm64 and armhf), `libraspberrypi0:armhf`, `vcdbg:armhf` (with `/lib/modules/6.1.21*`). Kept: the bookworm placeholders `/boot/config.txt`, `/boot/cmdline.txt` and `/boot/issue.txt`.
  **Pitfall:** purging the armhf legacy packages deleted most of `/boot/firmware/overlays` (369 → 90 files, including `vc4-kms-v3d*.dtbo` and `overlay_map.dtb`), although dpkg listed no files there for them. The firmware then silently skipped `vc4-kms-v3d` and the panel stayed dark. Fix: rerun the firmware hook for each installed kernel, which copies DTBs, overlays and kernel from the kernel package again: `sudo DEB_MAINT_PARAMS=configure /etc/kernel/postinst.d/z50-raspi-firmware <kver> /boot/vmlinuz-<kver>`. Check `ls /boot/firmware/overlays | wc -l` after removing any boot-related package.
- [x] **Installed driver 1.2 and the new overlay** (lanes/pins/rotation parameters) on the Pi.
- [x] **Breakout board power test** (no panel): VDDI and VDD measured correct when on and 0 V when switched off by the driver. HAT EEPROM answers at 0x50 and is empty (all `0xFF`).
- [x] **First boot with panel on 2 lanes** (`dtoverlay=panel-h497,lanes=2`, set 2026-10-03) because of the prototype's D3 polarity error. Run `./panel-check.sh`.
- [x] **Then test 4 lanes to see the failure mode.** Done: no difference, see the [bring-up log](#bring-up-log-2026-10-03-first-panel-tests). Expected: TE at about 60 Hz (LP commands only use lane 0), but no or garbled video. If 2 lanes also gives TE but no image, the RM69052 may need its lane count set by a register.
- [ ] **Next board revision:** route D3 like the other pairs and **ignore datasheet v0.2's pin 27 = D3N / pin 28 = D3P**, which is wrong (proven 2026-10-05: the reworked board fails, the unpatched one works; datasheet v1.8 has it right). **HAT EEPROM WP:** the solder jumper from WP to 3.3 V uses a *closed* footprint, so the EEPROM is permanently write-protected (writes fail with data-byte NAKs). Use an open jumper, plus a defined pull-down so WP isn't left floating. **Touch I2C:** SCL and SDA are swapped on the 1.8 V side of the TCA9800 level shifter (between the shifter and panel pins 35/36); the S3402 only answers with the lines swapped. **Add a level shifter for TP_INT and TE:** both are 1.8 V panel outputs wired straight to 3.3 V Pi inputs. TE only guarantees a high level of 0.7 × VDDI ≈ 1.26 V (datasheet v1.8 p.7), which a 3.3 V input may not read as high. TP_INT has no external pull-up, and the Pi's internal one (to 3.3 V, used on the prototype) exceeds the touch domain's 2.0 V absolute maximum and can back-power TP_VDDI while the panel is off. Use a 2-channel 1.8 V → 3.3 V shifter (or BSS138 open-drain stages), with TP_INT pulled up to TP_VDDI on the panel side, then drop the Pi pull-up from the overlay. **OTP_PWR** (pin 10; pin 30 in v1.8 numbering) must be left floating on the system side (datasheet v1.8 p.6): check the board. Prototype VBAT is fed from 5 V, above the 4.5 V maximum (datasheet v1.8 gives 4.5 V as the absolute maximum, too); production will use the LiPo.
- [x] **Power-off test** (blank/unblank via `/sys/class/graphics/fb0/blank`): clean since driver 1.14, no errors in `dmesg`.
- [x] **Program the HAT EEPROM** (2026-10-05): classic v1 format, vendor `RasmusB`, product `PsioPi Mainboard`, ID `0x0001`, version `0x0001` (v0.1), panel overlay embedded (defaults incl. `rotation=270`). Built and flashed with `hat/make-eeprom.sh`. The firmware logs `Loaded HAT overlay`; `config.txt` no longer needs a `dtoverlay=panel-h497` line, and `fbcon=rotate` is gone from `cmdline.txt` (the console follows the overlay's panel orientation). The board's WP jumper had to be cut first (see board revision notes); with WP floating, writes work.
- [x] **Debian packages** (2026-10-05): `packaging/build-debs.sh` builds `panel-h497-dkms` and `rmi4-psiopi-dkms` with `dpkg-deb`; CI attaches them to every run. Installed on the Pi from the packages instead of the manual DKMS steps.
- [x] **Pushed to GitHub** (https://github.com/RasmusB/panel-h497). CI builds against Raspberry Pi OS bookworm (6.12) and trixie (6.18) kernels for Pi 4 and Pi 5 and passes.
- [ ] **Optional: bigger console font.** Run `sudo dpkg-reconfigure console-setup` and pick Terminus 16x32.
- [x] **Touchscreen (Synaptics S3402)** works (2026-10-05): RMI4 driver as the `rmi4-psiopi` DKMS package, touch node in the overlay, landscape coordinates. See [Touchscreen](#touchscreen).
- [x] ~~After every kernel update, rebuild and reinstall the driver.~~ Now automatic through DKMS. Just check `dkms status` after a kernel upgrade (see [Kernel updates](#kernel-updates)).
- [x] **Clean up** (2026-10-05): removed `dtdebug=1` from `config.txt`, the old `panel-rm69052` overlay (`/boot/firmware/overlays/` and the local `.dts`/`.dtbo`), and the bring-up backups of `config.txt` and the module options (the two original backups from 2026-10-01 are kept).
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

Reads during video still sometimes time out or return error-report bytes (for example `00 04` = checksum flag).

Configuration that worked at 120 Mbit/s (superseded by step 8):
```
# /boot/firmware/config.txt
dtoverlay=panel-h497
# /etc/modprobe.d/panel-h497.conf
options panel-h497 debug=1 clock_khz=20000 early_power=1 noncont=1 burst=N early_display_on=1
# /etc/modprobe.d/vc4-dsi.conf (patched vc4 in /lib/modules/$(uname -r)/updates/)
options vc4 dsi_cprep=0 dsi_hsprep=8
```

**Step 7: 60 Hz (429 Mbit/s per lane, 1 UI = 2.33 ns).**

| Setup | D0N time at 0 V after trigger | Image |
|---|---|---|
| Patched `vc4`, `dsi_cprep=8 dsi_hsprep=16` | ~40 ns (THS-PREPARE ≈ 50 ns, bottom of the 49–99 ns window), overshoot to 460 mV at HS entry | yes |
| Patched `vc4`, stock values (24 UI) | ~60 ns (THS-PREPARE ≈ 70 ns, centred) | yes |
| **Stock `vc4`** | — | **yes** |

This proves the overhead is one byte clock (8 UI), not a fixed 64 ns. Checksum flags (`0x0400`) appear in both prepare settings. A blank/unblank with the patched module at 60 Hz once left the DSI controller stuck (`instat: 0x00000000`, every transfer times out) until reboot; not seen with the stock module.

**Step 8: which panel driver options are needed (stock `vc4`, 60 Hz).**

| Configuration | Image |
|---|---|
| Non-continuous clock + `early_power` | ✅, also after blank/unblank |
| Continuous clock + `early_power` | ✅ at boot (but `early_power` only covers the first power-up, so blank/unblank would fail) |
| Continuous clock, no `early_power` | ❌ panel receives nothing in HS (clock started before panel awake) |
| Continuous clock + `late_init` | ❌ init commands time out: with a continuous clock, stock vc4 has an LP window only once per frame |
| Burst instead of non-burst | ✅ |
| Display On after video starts (no `early_display_on`) | ✅, but commands during video are unreliable, so the default stays "before video" |

Final configuration, now the driver 1.14 defaults: non-continuous clock, `early_power`, burst, `early_display_on`, stock `vc4`, no module options. After unblank the panel briefly showed a dim level (fixed `init_brightness` of `0x20` until the backlight caught up); 1.14 sends the stored backlight level before video instead, and skips the brightness command when blanking (it timed out during video).

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

The stock `vc4` hard-codes pulse mode (`ST_END`), EoT off (`HSDT_EOT_DISABLE`) and LP stop per frame, ignoring the panel driver's mode flags. A patched copy with runtime options is kept in `vc4-patched/`. Since 2026-10-05 it also has `dsi_cprep`/`dsi_hsprep` for the prepare times. It is **not installed**: at 60 Hz the stock `vc4` works.

**Conclusion of 2026-10-03 (superseded 2026-10-05):** HS packets arrived corrupted in every configuration. The D-PHY timing tests that day only made the prepare times *longer*, which made things worse; the real causes were the too-long prepare times and the D3 swap (see the [2026-10-05 log](#bring-up-log-2026-10-05-root-causes-found)).

**Earlier conclusion (before `noncont`):** the panel receives nothing in high-speed mode. Low-power signalling on lane 0 works, so lane 0 wiring is fine. Suspects, in order: clock lane (CKP/CKN, pins 21/22) polarity or routing, HS signal integrity on the breakout, a break in the clock pair. Check the PCB layout, not just the schematic, since the D3 swap was a routing error.

Also noted:
- The Pi's DSI driver returns 0 instead of the byte count for successful reads, so `mipi_dsi_dcs_get_power_mode()` reports `-ENODATA`. The driver reads registers directly to work around this.
- LP commands and reads sometimes time out while video is running. Consider retries in the driver.
- TE (GPIO 22) has no level shifting (1.8 V into a 3.3 V input), and the datasheet only guarantees a high level of 0.7 × VDDI ≈ 1.26 V. Nothing uses TE in video mode (only `panel-check.sh` counts pulses as a liveness probe), but the next revision gets a level shifter for it anyway (see [Remaining steps](#remaining-steps)).

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

   **Datasheet v1.8** (2015-07-21, H497TLB01.4) is newer and corrects v0.2 in places. A copy is public at [panoxdisplay.com](https://www.panoxdisplay.com/uploadfile/datasheet/H497TLB01%20.pdf); it is marked "all rights reserved", so it isn't kept in the repo. Findings in [Datasheet v1.8](#datasheet-v18-2015).

2. **I2C capture (`i2c-dump.csv`)** from a working HDMI→DSI converter board that uses a Toshiba **TC358870** bridge at I2C address 0x0F. The bridge sends panel commands from its own I2C registers, so the capture contains:
   - **The real init sequence**, as DCS packets written through registers 0x0500 and 0x0504. The driver used it up to 1.15; since 1.16 it uses the datasheet v1.8 init, which stops the checksum error reports.
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
| `driver/dkms.conf` | DKMS package definition (`panel-h497`, current version in `PACKAGE_VERSION`, 1.14 as of 2026-10-05). |
| `panel-h497.dts` / `.dtbo` | Device tree overlay, display only, with parameters (see [Overlay parameters](#overlay-parameters)). |
| `README.md` | Customer-facing install and usage instructions. |
| `LICENSE` | GPL-2.0 |
| `.github/workflows/build.yml` | CI: builds the driver against the current Raspberry Pi OS kernels (bookworm and trixie; Pi 4 and Pi 5 kernels) and compiles the overlay, on every push and weekly. |
| `compile-overlay.sh` | Compiles a `.dts` into a `.dtbo`, running the preprocessor for `#include`s, and prints install instructions. |
| `packaging/build-debs.sh` | Builds the `panel-h497-dkms` and `rmi4-psiopi-dkms` packages into `packaging/out/`. |
| `hat/eeprom_settings.txt` / `hat/make-eeprom.sh` | HAT ID EEPROM contents for the PsioPi mainboard, and the script that builds (and with `--flash` writes and verifies) the image with the overlay embedded. |
| `vc4-patched/` | Copy of the Raspberry Pi `vc4` driver (6.6.31) with bring-up options (`dsi_cprep`/`dsi_hsprep`, EoT, sync mode, blanking). Not installed; only needed for low bit rates. |
| `i2c-dump.csv` | Raw I2C capture from the TC358870 board (local only, not in git). |
| `rmi4/` | Synaptics RMI4 touch driver (vendored upstream, DKMS package `rmi4-psiopi`). |

## How the driver works

The Pi's graphics driver (vc4) generates the video signal. The panel driver tells it how to power, configure and describe the panel. The kernel connects the two through the overlay's `compatible = "auo,h497tlb01"`.

- **`probe()`** runs at boot. It gets the two regulators and the reset GPIO, holding the panel in reset from the start. It sets up DSI: 4 lanes, RGB888, video mode with burst, **non-continuous clock**, commands in low-power mode. It sets `prepare_prev_first`, so the DSI link is up before commands are sent. With `early_power` (default) it already powers and resets the panel here, so the panel is awake when the Pi starts the clock lane. Finally it registers the panel and the brightness device.
- **`get_modes()`** reports 720×1280 at 66.9 MHz with the EDID timings, and a physical size of 62 × 110 mm.
- **`prepare()`** runs the power-on sequence from the datasheet (skipping steps 1–5 if `early_power` already did them at probe):
  1. Hold reset.
  2. Turn on VDDI, then VDD.
  3. Wait 40 ms.
  4. Pulse reset for 15 ms.
  5. Wait 120 ms for the chip to load its factory settings (OTP).
  6. Send the init table.
  7. Send `35 00` (TE on) and `53 20` (brightness control on).
  8. Send Sleep Out (`0x11`) and wait 120 ms.
  9. Send the current backlight level (`0x51`).
  10. With `early_display_on` (default): wait 180 ms, send Display On (`0x29`), wait 40 ms. Everything goes out before video starts, because low-power commands during video are unreliable on vc4.
- **`enable()`** only sends Display On if `early_display_on` is off.
- **`disable()`** sends Display Off and Sleep In, then waits 120 ms, while the DSI link is still up.
- **`unprepare()`** asserts reset and turns off VDD, then VDDI. It sends no commands: because of `prepare_prev_first`, the DSI controller is already off by the time it runs. Driver 1.1 sent Sleep In here, which timed out.
- **Brightness** is a backlight device at `/sys/class/backlight/panel-h497`, range 0–255 (128 until systemd restores the saved value). Changes are sent as DCS `0x51 <value>`. Changes made while the panel is off are saved and applied when it powers on. The brightness-to-0 command the backlight core sends when blanking is skipped: it timed out during video, and Display Off follows anyway.
- **Module parameters** let you tune timings without rebuilding (see below).

### Init sequence (datasheet v1.8)

Since driver 1.16 the init table is the datasheet v1.8 "Display Initial Setting" (p.17). Driver 1.17 dropped the `init_set` option and its alternatives (datasheet v0.2 verbatim, and no init at all); they are in git history. Up to 1.15 the table was the I2C capture (also in git history), which lacked `C0`, `C1`, `EA` and page 5 `C3` and had `BB 77×7` and `BE 32 38 78`.

```
Page 0: F0 55 AA 52 08 00 | B0 00 10 10 | BA 60 | BB 00×7
        C0 C0 04 00 20 02 E4 E1 C0 | C1 C0 04 00 20 04 E4 E1 C0
Page 2: F0 55 AA 52 08 02 | EA 7F 20 00 00 00 | CA 04 | E1 00 | E2 0A
        E3 40 | E7 00×4 | ED 48 00 E0 13 08 00 91 08 | FD 00 08 1C 00 00 01
        C3 11 24 04 0A 02 04 00 1C 10 F0 00
Page 3: F0 55 AA 52 08 03 | E0 00 | F1 00 00 00 00 00 15 | F6 08
Page 5: F0 55 AA 52 08 05 | C3 00 10 50 50 50 | C4 00 14 | C9 04
Page 1: F0 55 AA 52 08 01 | B0 06×3 | B1 14×3 | B2 00×3 | B4 66×3
        B5 44×3 | B6 54×3 | B7 24×3 | B9 04×3 | BA 14×3 | BE 22 38 78
Common: 35 00 (TE on) | 53 20 (brightness ctrl, added) | 11 (Sleep Out) | 29 (Display On)
```

## System changes

| What | Where | Backup |
|---|---|---|
| `dkms` package (installed with `--no-install-recommends`) | apt | n/a |
| Packages `panel-h497-dkms` and `rmi4-psiopi-dkms` (sources in `/usr/src/panel-h497-<ver>/`, `/usr/src/rmi4-psiopi-<ver>/`, registered with DKMS by their `postinst`) | apt / dpkg | none (new) |
| Kernel modules, built by DKMS | `/lib/modules/<kernel>/updates/dkms/` (`panel-h497`, `rmi_core`, `rmi_i2c`; 6.12.109 v8 and 2712) | none (new files) |
| Overlay | Embedded in the board's HAT EEPROM; also installed as `/boot/firmware/overlays/panel-h497.dtbo` (older build, default rotation 0, unused) | none |
| Removed `dtoverlay=panel-rm69052`; no panel `dtoverlay` line any more (the HAT overlay replaces it); `dtdebug=1` removed; `display_auto_detect=0` (was already set) | `/boot/firmware/config.txt` | `config.txt.bak-202610012156` (original) |
| `fbcon=rotate:1` added on 2026-10-01, removed again on 2026-10-05 | `/boot/firmware/cmdline.txt` | `cmdline.txt.bak-202610012219` (original) |
| `apt full-upgrade`, kernel 6.6.31 → 6.12.109 (2026-10-05) | apt | none (6.6.31 fallback removed after testing) |

No udev rule was needed for brightness. The existing `/lib/udev/rules.d/60-backlight.rules` already makes `brightness` writable by the `video` group, and the user is in that group. systemd also saves and restores the brightness across reboots.

## Verified on hardware (2026-10-05, kernel 6.12.109, driver 1.14)

| Check | Result |
|---|---|
| HAT EEPROM | `/proc/device-tree/hat/product` = `PsioPi Mainboard`; firmware log `Loaded HAT overlay` |
| Driver | loaded from `/lib/modules/6.12.109+rpt-rpi-v8/updates/dkms/`, no module options |
| vc4 | stock module, `bound fe700000.dsi` |
| `/sys/class/drm/card1-DSI-1` | `connected`, `enabled`, `720x1280` at 60 Hz |
| Image | correct, landscape (`fbcon` rotation 3 from the overlay's `rotation=270`) |
| Brightness | `/sys/class/backlight/panel-h497`, writes work without sudo |
| Blank/unblank | image returns at the stored brightness, no errors in `dmesg` |

## Overlay parameters

From driver 1.1. The driver reads `dsi-lanes` (default 4) and the standard `rotation` property from the device tree.

| Parameter | Default | Description |
|---|---|---|
| `dsi0` | off (DSI1) | Use the DSI0 port |
| `lanes=<n>` | `4` | DSI data lanes, 1–4. Pi 4B display connector: `2` |
| `rotation=<deg>` | `270` | Mounting rotation `0`/`90`/`180`/`270`. Sets the DRM panel orientation, which the console and desktops follow. `270` is landscape as mounted in the PsioPi (`90` turns it upside down). |
| `reset_gpio=<n>` | `27` | RESX, active low |
| `vddi_gpio=<n>` | `18` | VDDI 1.8 V LDO enable |
| `vdd_gpio=<n>` | `23` | VDD 3.1 V LDO enable |

Verified by merging the overlay into `bcm2711-rpi-cm4.dtb` with `dtmerge`, with and without each parameter. On the PsioPi the overlay comes from the HAT EEPROM with its defaults; parameters only apply when it's loaded with `dtoverlay=panel-h497,...` instead.

Bandwidth note: 720×1280 at 60 Hz in RGB888 needs about 1.6 Gbit/s, so roughly 800 Mbit/s per lane on 2 lanes. That's near the Pi 4's DSI limit. If 2 lanes is unstable, lower `clock_khz` for a lower refresh rate.

## Tuning

Module parameters, set in `/etc/modprobe.d/panel-h497.conf`, then reboot:

None are needed on the PsioPi; the defaults are the verified configuration.

```
options panel-h497 burst=N
options panel-h497 clock_khz=66900 hfp=60 hsync=40 hbp=35 vfp=8 vsync=8 vbp=8
```

| Parameter | Default | Meaning |
|---|---|---|
| `burst` | `Y` | `N` switches to non-burst mode with sync pulses. Both work. |
| `noncont` | `Y` | Non-continuous clock. With a continuous clock the panel misses the clock start after blank/unblank. |
| `early_power` | `Y` | Power and reset the panel at probe, before the Pi starts the clock lane. |
| `early_display_on` | `Y` | Send Display On before video starts. |
| `init_brightness` | `-1` | Brightness sent before video; `-1` = current backlight level. |
| `debug` | `N` | Read back ID, power mode and error count during power-up (reads during video time out). |
| `tc_exact`, `late_init`, `no_eot`, `lane_reg` | off | Bring-up experiments; see the bring-up logs. |
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

Verified on 2026-10-05 with `apt full-upgrade` from 6.6.31 to 6.12.109: DKMS built and signed the module for both new kernels (`rpi-v8` and `rpi-2712`) during the upgrade, and the panel worked after the reboot without any manual step.

After a kernel upgrade, before rebooting, confirm the new kernel shows `installed`:

```bash
dkms status
# panel-h497/1.14, <new-kernel-version>, aarch64: installed
```

If the build failed, for example because a future kernel changed the display API, the panel stays dark until the driver is fixed. The log is in `/var/lib/dkms/panel-h497/<version>/build/make.log`. CI builds the driver weekly against the current Raspberry Pi OS kernels, so API breaks should show up there first.

**Changing a driver:** DKMS builds from the packaged copy in `/usr/src/`, not from `~/workspace/dts/`. After editing, bump `PACKAGE_VERSION` in the module's `dkms.conf`, then rebuild and install the packages:

```bash
packaging/build-debs.sh
sudo apt install ./packaging/out/panel-h497-dkms_*_all.deb ./packaging/out/rmi4-psiopi-dkms_*_all.deb
```

The old version's `prerm` removes it from DKMS, the new version's `postinst` builds it for every installed kernel. Packaging notes:
- `/boot/firmware` is FAT, where dpkg can't make the backup hard links it needs to replace a file. The overlay is therefore shipped in `/usr/lib/panel-h497/` and copied to `/boot/firmware/overlays/` by `postinst` (removed by `postrm`), as Debian's firmware packages do.
- `rmi4-psiopi` uses 6.12 sources and is limited to kernels 6.12 and newer (`BUILD_EXCLUSIVE_KERNEL`); DKMS prints an `Error! ... BUILD_EXCLUSIVE` notice for older kernels, which `postinst` treats as a skip (exit code 77).

Without the packages, by hand (development only):

```bash
OLD=1.16; V=1.17   # installed and new version
sudo dkms remove panel-h497/$OLD --all
sudo mkdir -p /usr/src/panel-h497-$V
sudo cp ~/workspace/dts/driver/{panel-h497.c,Makefile,dkms.conf} /usr/src/panel-h497-$V/
sudo dkms install panel-h497/$V
```

## Datasheet v1.8 (2015)

Findings from the H497TLB01.4 datasheet v1.8 (see [Sources](#sources-of-information)) that v0.2 lacks or gets wrong:

- **Pin numbering is reversed.** v1.8 numbers the 39-pin FPC from the other end: pin *n* in v1.8 is pin 40 − *n* in v0.2. Checked against D2P (15), CKP/CKN (21/22), D3 and the touch I2C pins. This document uses v0.2 numbering.
- **D3 is corrected.** v1.8 has D3N on 12 and D3P on 13 (v0.2 pins 28/27); the revision history says rev 0.6 (2014-01-08) "Revised pin 27 MIPI DSI data3+, pin 28 MIPI DSI data3-". This matches the hardware finding.
- **Touch pins** (v1.8 / v0.2): TP_RESX 3/37, TP_SCL 4/36, TP_SDA 5/35, TP_INT 6/34 ("interrupt output", type not given), TP_VDDI 7/33, TP_VCC 8/32.
- **Other pins:** TE 29/11 (output), OTP_PWR 30/10 ("Driver IC R/W use only, system side must floating"), VBAT 34–38/2–6.
- **Absolute maximum ratings** (p.6): VBAT 4.5 V; VDDI and TP_VDDI −0.3 to 2.0 V; VCI and TP_VCC −0.3 to 4.0 V.
- **Operating conditions** (p.7): VBAT 2.9–4.5 V (typ. 3.7), VDDI and TP_VDDI 1.65–1.95 V, VCI and TP_VCC 2.7–3.6 V (typ. 3.1). RESX input: high ≥ 0.8 × VDDI, low ≤ 0.2 × VDDI. TE output: high ≥ 0.7 × VDDI, low ≤ 0.3 × VDDI. Touch I/O levels are given as fractions of TP_VDDI (I2C noise margins, p.13).
- **Display current** (p.7, white, 60 Hz): IBAT 300 mA typ., 360 mA max (460 mA at VBAT 2.9 V); IVCI 60/80 mA; IVDDI 1/10 mA. Deep standby < 1 µA.
- **Touch current** (p.8): active TP_VDDI 13 mA (1 finger) to 18.5 mA (10 fingers), TP_VCC 12.5 mA; doze 0.4/0.35 mA; deep sleep 13.3/8 µA.
- **Touch timing** (p.14, p.16): TP_RESX pulse ≥ 100 ns; bootloader starts ≤ 2 ms after reset (≤ 46 ms after power-up); reboot ≤ 16 ms; power-up ≤ 60 ms. Touch spec (p.19): 10 fingers, ≥ 100 Hz report rate, wake-up gestures (double tap, swipe).
- **Init code** (p.17): the driver's default since 1.16 (see [Init sequence](#init-sequence-datasheet-v18)). The earlier capture-based table was nearly the same; v1.8 differs in `BB` (00 × 7, capture 77 × 7) and `BE` (22 38 78, capture 32 38 78), and adds `C0`/`C1` (page 0), `EA 7F 20 00 00 00` (page 2) and `C3 00 10 50 50 50` (page 5). `EA` stops the checksum error reports during video (see [Remaining steps](#remaining-steps)); the rest made no visible or measurable difference.
- **Touch registers** (p.20–21): I2C address 0x20, F12 finger data from 0x0006, object types (finger, stylus, palm, gloved finger). The upstream RMI4 driver already handles this.

## Touchscreen

Wiring (PsioPi prototype): the CM4 IO board's DSI connector I2C (GPIO 44/45, `i2c_csi_dsi`) → TCA9800 level shifter → panel pins 35/36 (SDA/SCL; pins 5/4 in v1.8 numbering). TP_INT on GPIO 25, TP_RESX on GPIO 24 (active low, 1.8 V via divider). Touch supplies are the panel rails (VDD 3.3 V on the prototype, VDDI 1.8 V, switched by GPIO 23/18).

Bring-up 2026-10-05:
- The Raspberry Pi kernel has **no RMI4 driver** (`# CONFIG_RMI4_CORE is not set`, and `include/linux/rmi.h` is not in the headers), so it has to come as a DKMS module.
- With hardware I2C on GPIO 44/45 nothing answers at 0x20 (only the IO board's 0x2f and 0x51), with RESX released and the supplies on. TP_INT sat at 18 mV even with the Pi's pull-up.
- Measured at the panel connector: TP_VCC 3.284 V, TP_VDDI 1.800 V, RESX 1.8 V, TCA9800 supplies and EN as expected.
- A bit-banged bus with the lines swapped (`dtoverlay i2c-gpio i2c_gpio_sda=45 i2c_gpio_scl=44 bus=11`) finds the controller at **0x20**: **SCL/SDA are swapped on the 1.8 V side of the TCA9800** (board routing).
- RMI4 identification: manufacturer 0x01 (Synaptics), product **`S3402BR`**. Page Description Table (page 0): **F34** at 0xE9 (flash), **F01** at 0xE3 (query base 0x24), **F12** at 0xDD (2D sensor). TP_INT low is the controller's ATTN request, not a fault.
- TP_INT needs a pull-up; none on the board, the Pi's internal one is used for now. That pull-up goes to 3.3 V, but TP_INT is in the 1.8 V TP_VDDI domain (absolute maximum 2.0 V). Datasheet v1.8 doesn't give TP_INT's output type (open-drain or push-pull) or its levels, and the S3402 datasheet is only available under NDA. A sibling Synaptics datasheet (S7817, seen only as a search summary) says the ATTN pull-up must go to the controller's I/O supply, because its ESD diodes otherwise leak pull-up current into that supply when power is off. The next board revision adds a level shifter (see [Remaining steps](#remaining-steps)).
- After swapping SCL/SDA on the board (rework), the controller answers on the hardware bus.

Solution:
- **Driver:** `rmi4/` vendors the upstream RMI4 driver (core, F01, 2D sensor, F12, I2C transport) from `raspberrypi/linux` `rpi-6.12.y` and builds it with DKMS as `rmi4-psiopi`. One addition in `rmi_i2c.c`: optional `reset-gpios`, held while the supplies come up and asserted again before they go off. See `rmi4/README.md`.
- **Overlay** (`panel-h497.dts`, parameter `touch`, default on): enables `i2c0if`/`i2c0mux`, puts `touchscreen@20` (`syna,rmi4-i2c`) on `i2c_csi_dsi` with IRQ GPIO 25 (level low, Pi pull-up), reset GPIO 24, `vdd`/`vio` from the panel regulators, F01 (`syna,nosleep-mode`) and F12 nodes.
- **Shared rails:** `panel_vdd` has `vin-supply = <&panel_vddi>`, so VDD never comes up before VDDI. The touch driver keeps both rails enabled while bound; when the screen blanks, only the panel driver drops its references (checked in `regulator_summary`: users 3/2 → 2/1 → 3/2) and the touch stays powered.
- **Orientation:** the raw axes are portrait (720 × 1280). With `rotation=270`, landscape top left/top right/bottom right/bottom left gave raw (117,1162), (119,101), (671,141), (614,1186). `touchscreen-inverted-y` + `touchscreen-swapped-x-y` in the F12 node (the driver inverts before swapping) give 1280 × 720 with origin top left: (149,136), (1162,127), (1139,605), (144,629).
- dmesg: `rmi4_f01 ... found RMI device, manufacturer: Synaptics, product: S3402BR, fw id: 1460222`, input device `Synaptics S3402BR`.
- Not tested yet: a desktop session (compositors map touch to the output themselves).
- **In the EEPROM** (2026-10-05): the overlay with touch (image 4,004 bytes) was flashed with `hat/make-eeprom.sh --flash`; after a reboot without any panel lines in `config.txt`, the firmware loads the HAT overlay and both panel and touch come up. `eepflash.sh` (raspi-utils 20240402) no longer works on kernel 6.12 (it uses `/sys/class/i2c-adapter`, which is gone), so the script now drives the at24 driver through `/sys/bus/i2c/devices/` itself, on `i2c-0` (the i2c0 mux channel on GPIO 0/1, present once the touch overlay enables the mux).

## Undo everything

1. Stop the firmware from applying the HAT overlay: add `force_eeprom_read=0` to `/boot/firmware/config.txt` (or erase the EEPROM). If a `dtoverlay=panel-h497` line was added by hand, remove it.
2. `cmdline.txt` no longer has panel changes. The original is `cmdline.txt.bak-202610012219`.
3. Remove the driver and the overlay:
   ```bash
   sudo apt remove panel-h497-dkms rmi4-psiopi-dkms
   ```
4. Reboot.

If the Pi won't boot, mount the boot partition on another machine and restore the backups.
