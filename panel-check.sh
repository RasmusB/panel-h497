#!/usr/bin/env bash
# Collects panel-h497 status in one go: overlay, driver, DRM, power, and
# whether the panel is alive (TE pulses).
# Usage: ./panel-check.sh [te_gpio]   (default TE on GPIO22)
TE_GPIO=${1:-22}

section() { printf '\n=== %s ===\n' "$1"; }

section "System"
uname -r
dkms status panel-h497 2>/dev/null
echo "loaded srcversion:    $(cat /sys/module/panel_h497/srcversion 2>/dev/null || echo 'NOT LOADED')"
echo "installed srcversion: $(modinfo -F srcversion panel-h497 2>/dev/null)"

section "Overlay (firmware log)"
sudo vclog -m 2>/dev/null | grep -iE 'dterror|panel-h497' || echo "no overlay lines (is dtoverlay=panel-h497 in config.txt?)"

section "Kernel messages"
dmesg | grep -iE 'h497|dsi' | grep -v 'dependency cycle' || echo "none"

section "DRM connector"
for c in /sys/class/drm/card*-DSI-*; do
    [ -e "$c" ] || { echo "no DSI connector"; break; }
    echo "$(basename "$c"): status=$(cat "$c/status") enabled=$(cat "$c/enabled") dpms=$(cat "$c/dpms") modes=$(cat "$c/modes" | tr '\n' ' ')"
done

section "Power and reset"
for r in /sys/class/regulator/*; do
    n=$(cat "$r/name")
    case $n in panel*) echo "$n: $(cat "$r/state")" ;; esac
done
pinctrl get 18,23,27 2>/dev/null

section "Brightness"
b=/sys/class/backlight/panel-h497
[ -d $b ] && echo "brightness=$(cat $b/brightness)/$(cat $b/max_brightness) bl_power=$(cat $b/bl_power)" || echo "no backlight device"

section "TE pulses on GPIO$TE_GPIO (panel alive?)"
if command -v gpiomon >/dev/null; then
    n=$(timeout 2 gpiomon -r -F '%e' gpiochip0 "$TE_GPIO" 2>/dev/null | wc -l)
    echo "$n rising edges in 2 s (~$((n / 2)) Hz)"
    if [ "$n" -ge 100 ]; then
        echo "-> panel is running and accepted the init sequence (expect ~60 Hz)"
    elif [ "$n" -gt 0 ]; then
        echo "-> some activity, but not a steady 60 Hz"
    else
        echo "-> no TE pulses: panel not running, init not accepted, or TE not wired"
    fi
else
    echo "gpiomon not installed (sudo apt install gpiod)"
fi
