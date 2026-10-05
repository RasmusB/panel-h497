#!/usr/bin/env bash
# Build the DKMS .deb packages:
#   panel-h497-dkms   panel driver (driver/) + overlay, copied to
#                     /boot/firmware/overlays by postinst
#   rmi4-psiopi-dkms  Synaptics RMI4 touch driver (rmi4/)
# Versions come from each dkms.conf (PACKAGE_VERSION), Debian revision -1.
# Output: packaging/out/*.deb. Install with: sudo apt install ./packaging/out/*.deb
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/packaging/out"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
MAINTAINER="RasmusB <RasmusB@users.noreply.github.com>"
HOMEPAGE="https://github.com/RasmusB/panel-h497"
mkdir -p "$OUT"

conf_value() { sed -n "s/^$2=\"\(.*\)\"/\1/p" "$1"; }

# make_deb <pkg> <srcdir> <description> <long description> <files...>
make_deb() {
    local pkg=$1 src=$2 short=$3 long=$4; shift 4
    local name ver dir
    name=$(conf_value "$ROOT/$src/dkms.conf" PACKAGE_NAME)
    ver=$(conf_value "$ROOT/$src/dkms.conf" PACKAGE_VERSION)
    dir="$STAGE/$pkg"

    mkdir -p "$dir/DEBIAN" "$dir/usr/src/$name-$ver" "$dir/usr/share/doc/$pkg"
    (cd "$ROOT/$src" && cp -r --parents "$@" "$dir/usr/src/$name-$ver/")

    cat > "$dir/usr/share/doc/$pkg/copyright" <<COPY
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: $name
Source: $HOMEPAGE

Files: *
License: GPL-2.0
 On Debian systems, the full text of the GNU General Public License
 version 2 is in /usr/share/common-licenses/GPL-2.
COPY

    cat > "$dir/DEBIAN/control" <<CTRL
Package: $pkg
Version: $ver-1
Architecture: all
Maintainer: $MAINTAINER
Depends: dkms (>= 3)
Recommends: linux-headers-rpi-v8 | linux-headers-rpi-2712
Section: kernel
Priority: optional
Homepage: $HOMEPAGE
Description: $short
$(echo "$long" | fold -s -w 72 | sed 's/^/ /; s/ *$//')
CTRL

    # Register with DKMS and build for every installed kernel that has
    # headers; later kernels are handled by DKMS's own kernel hooks.
    cat > "$dir/DEBIAN/postinst" <<POST
#!/bin/sh
set -e
NAME=$name
VER=$ver
if [ "\$1" = configure ]; then
    if [ -z "\$(dkms status -m \$NAME -v \$VER)" ]; then
        dkms add -m \$NAME -v \$VER
    fi
    for kdir in /lib/modules/*; do
        [ -e "\$kdir/build" ] || continue
        k=\$(basename "\$kdir")
        rc=0
        dkms install -m \$NAME -v \$VER -k "\$k" || rc=\$?
        # 77: kernel excluded by BUILD_EXCLUSIVE_KERNEL in dkms.conf
        case \$rc in
        0|77) ;;
        *) echo "\$NAME: build failed for kernel \$k (see /var/lib/dkms/\$NAME/\$VER/build/make.log)" >&2 ;;
        esac
    done
fi
exit 0
POST

    cat > "$dir/DEBIAN/prerm" <<PRERM
#!/bin/sh
set -e
case "\$1" in
remove|upgrade|deconfigure)
    if [ -n "\$(dkms status -m $name -v $ver)" ]; then
        dkms remove -m $name -v $ver --all || true
    fi
    ;;
esac
exit 0
PRERM
    chmod 755 "$dir/DEBIAN/postinst" "$dir/DEBIAN/prerm"
}

# Panel driver, plus the overlay for setups without the PsioPi HAT EEPROM.
# /boot/firmware is FAT, where dpkg can't make its backup hard links when
# replacing a file, so the overlay is shipped in /usr/lib and copied over
# (as Debian's firmware packages do).
make_deb panel-h497-dkms driver \
    "DRM panel driver for the AUO H497TLB01 AMOLED (RM69052), DKMS" \
    "Raspberry Pi DSI driver for the 4.97\" 720x1280 H497TLB01 AMOLED panel, built with DKMS for every installed kernel. Also installs the panel-h497 device tree overlay. The PsioPi mainboard loads the overlay from its HAT EEPROM instead." \
    panel-h497.c Makefile dkms.conf
"$ROOT/compile-overlay.sh" "$ROOT/panel-h497.dts" >/dev/null
P="$STAGE/panel-h497-dkms"
mkdir -p "$P/usr/lib/panel-h497"
cp "$ROOT/panel-h497.dtbo" "$P/usr/lib/panel-h497/"
sed -i 's|^exit 0$|OVL=/boot/firmware/overlays\
if [ "$1" = configure ] \&\& [ -d "$OVL" ]; then\
    cp /usr/lib/panel-h497/panel-h497.dtbo "$OVL/panel-h497.dtbo"\
fi\
exit 0|' "$P/DEBIAN/postinst"
cat > "$P/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    rm -f /boot/firmware/overlays/panel-h497.dtbo
fi
exit 0
POSTRM
chmod 755 "$P/DEBIAN/postrm"

# Touch driver (vendored upstream RMI4)
make_deb rmi4-psiopi-dkms rmi4 \
    "Synaptics RMI4 touch driver (I2C, F01/F12) for the H497 panel, DKMS" \
    "The upstream Linux RMI4 driver, which the Raspberry Pi kernel does not include, with an optional reset GPIO for the I2C transport. Drives the Synaptics S3402 touch controller of the H497 panel." \
    Makefile dkms.conf $(cd "$ROOT/rmi4" && echo *.c *.h) include

for d in "$STAGE"/*/; do
    dpkg-deb --root-owner-group -Zxz --build "$d" "$OUT" >/dev/null
done
ls -1 "$OUT"/*.deb
