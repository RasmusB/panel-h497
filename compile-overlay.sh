#!/usr/bin/env bash
# Compile a device tree overlay source (.dts) into a .dtbo for Raspberry Pi.
# Usage: ./compile-overlay.sh <file.dts>
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <file.dts>" >&2
    exit 1
fi

SRC="$1"
if [[ ! -f "$SRC" ]]; then
    echo "Error: '$SRC' not found" >&2
    exit 1
fi

for tool in cpp dtc; do
    command -v "$tool" >/dev/null || { echo "Error: '$tool' not installed (sudo apt install device-tree-compiler cpp)" >&2; exit 1; }
done

NAME="$(basename "$SRC")"
NAME="${NAME%.dts}"
NAME="${NAME%-overlay}"
OUT="$(dirname "$SRC")/${NAME}.dtbo"

# Kernel headers provide dt-bindings/*.h used by #include
INCLUDES=(-I "$(dirname "$SRC")")
for d in /usr/src/linux-headers-"$(uname -r)"/include \
         /usr/src/linux-headers-*-common-rpi/include \
         /usr/src/linux-headers-*/include; do
    [[ -d "$d/dt-bindings" ]] && INCLUDES+=(-I "$d")
done

TMP="$(mktemp --suffix=.dts)"
trap 'rm -f "$TMP"' EXIT

echo "Preprocessing $SRC ..."
cpp -nostdinc -undef -x assembler-with-cpp -P "${INCLUDES[@]}" "$SRC" -o "$TMP"

echo "Compiling -> $OUT ..."
dtc -@ -I dts -O dtb -o "$OUT" "$TMP"

# Pick boot overlay directory (Bookworm uses /boot/firmware)
if [[ -d /boot/firmware/overlays ]]; then
    BOOT=/boot/firmware
else
    BOOT=/boot
fi

cat <<EOF

Done: $OUT

How to apply the overlay
------------------------
Permanently (loaded at boot):
  1. Copy the overlay to the boot partition:
       sudo cp "$OUT" $BOOT/overlays/
  2. Add this line to $BOOT/config.txt (under [all]):
       dtoverlay=$NAME
  3. Reboot:
       sudo reboot

At runtime (for testing, not persistent):
       sudo dtoverlay -d "$(cd "$(dirname "$OUT")" && pwd)" $NAME
   Check loaded overlays:  dtoverlay -l
   Remove it again:        sudo dtoverlay -r $NAME

Note: overlays that touch display/DSI hardware usually need the boot-time
method, since the display pipeline is set up before userspace starts.
EOF
