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
# Not in /tmp: with fs.protected_regular, root (eepflash.sh) can't write to
# a file another user created in a sticky directory.
READBACK="$HAT/.readback.bin"
trap 'sudo rm -f "$READBACK"' EXIT

sudo eepflash.sh -y -r -t=$TYPE -f="$BACKUP" >/dev/null
sudo chown "$(id -u):$(id -g)" "$BACKUP"
echo "Backed up the current contents to $BACKUP"

sudo eepflash.sh -y -w -t=$TYPE -f="$EEP" >/dev/null
sudo eepflash.sh -y -r -t=$TYPE -f="$READBACK" >/dev/null

if cmp -n "$(stat -c %s "$EEP")" "$EEP" "$READBACK"; then
    echo "Written and verified."
else
    echo "Verify FAILED: read-back differs from $EEP" >&2
    exit 1
fi
