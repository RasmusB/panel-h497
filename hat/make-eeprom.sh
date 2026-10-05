#!/usr/bin/env bash
# Build the PsioPi HAT EEPROM image (classic v1 format, panel overlay
# embedded) and optionally write it to the board's CAT24C512.
#
# Usage: hat/make-eeprom.sh            build hat/psiopi.eep
#        hat/make-eeprom.sh --flash    build, back up the EEPROM, write, verify
set -euo pipefail

HAT="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HAT")"
EEP="$HAT/psiopi.eep"
TYPE=24c512

"$ROOT/compile-overlay.sh" "$ROOT/panel-h497.dts" >/dev/null
eepmake -v1 "$HAT/eeprom_settings.txt" "$EEP" "$ROOT/panel-h497.dtbo"
echo "Built $EEP ($(stat -c %s "$EEP") bytes)"

[[ "${1:-}" == "--flash" ]] || exit 0

BACKUP="$HAT/eeprom-backup-$(date +%Y%m%d%H%M%S).bin"
# Not in /tmp: with fs.protected_regular, root can't write to a file another
# user created in a sticky directory.
READBACK="$HAT/.readback.bin"

# Talk to the at24 driver directly instead of using eepflash.sh: it relies on
# /sys/class/i2c-adapter, which kernel 6.12 no longer has. The EEPROM is on
# GPIO 0/1: i2c-0 when the i2c0 mux is enabled (as with the touch overlay),
# otherwise a bit-banged bus on those pins, like eepflash.sh does.
if [[ -e /sys/bus/i2c/devices/i2c-0 ]]; then
    BUS=0
else
    BUS=9
    [[ -e /sys/bus/i2c/devices/i2c-$BUS ]] ||
        sudo dtoverlay i2c-gpio i2c_gpio_sda=0 i2c_gpio_scl=1 bus=$BUS
    sleep 1
fi
ADAPTER=/sys/bus/i2c/devices/i2c-$BUS
DEV=/sys/bus/i2c/devices/$BUS-0050

sudo modprobe at24
[[ -d "$DEV" ]] || echo "$TYPE 0x50" | sudo tee "$ADAPTER/new_device" >/dev/null
cleanup() {
    sudo rm -f "$READBACK"
    echo 0x50 | sudo tee "$ADAPTER/delete_device" >/dev/null || true
}
trap cleanup EXIT

sudo dd if="$DEV/eeprom" of="$BACKUP" status=none
sudo chown "$(id -u):$(id -g)" "$BACKUP"
echo "Backed up the current contents to $BACKUP"

sudo dd if="$EEP" of="$DEV/eeprom" status=none
sudo dd if="$DEV/eeprom" of="$READBACK" status=none

if cmp -n "$(stat -c %s "$EEP")" "$EEP" "$READBACK"; then
    echo "Written and verified (i2c-$BUS)."
else
    echo "Verify FAILED: read-back differs from $EEP" >&2
    exit 1
fi
