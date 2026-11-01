#!/usr/bin/env bash
# Take the screenshot of the Schedule tab for the README (docs/theme-schedule.png).
#
#   scripts/screenshot.sh <dir-with-debs> [out.png]
#
# Meant for CI and docker, as root in an ubuntu:26.04 container: installs the
# xfce4-settings .debs from <dir-with-debs>, runs the appearance dialog on a
# virtual X server (Xvfb) with xfwm4 for the window frame and takes the
# picture with ImageMagick. The clock is fixed (libfaketime) and so are the
# theme, the font, the time zone and the rules, so the picture only changes
# when the dialog does.
#
# Locally:
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:26.04 scripts/screenshot.sh out docs/theme-schedule.png
set -euo pipefail

DEBS=$(realpath "${1:?usage: $0 <dir-with-debs> [out.png]}")
OUT=$(realpath -m "${2:-docs/theme-schedule.png}")
FAKE_TIME=${FAKE_TIME:-2026-11-01 12:00:00}

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
  "$DEBS"/xfce4-settings_*.deb "$DEBS"/xfce4-helpers_*.deb \
  xvfb xfwm4 xfconf dbus dbus-x11 xdotool imagemagick faketime tzdata \
  greybird-gtk-theme elementary-xfce-icon-theme fonts-noto-core

export HOME=$(mktemp -d) TZ=Europe/Berlin LANG=C.UTF-8 DISPLAY=:99
mkdir -p "$HOME/.config/gtk-3.0"
cat > "$HOME/.config/gtk-3.0/settings.ini" <<'EOF'
[Settings]
gtk-theme-name=Greybird
gtk-icon-theme-name=elementary-xfce
gtk-font-name=Noto Sans 11
EOF

Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
XVFB=$!
trap 'kill $XVFB 2>/dev/null || true' EXIT
for _ in $(seq 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done 2>/dev/null || sleep 2

export OUT FAKE_TIME
dbus-run-session -- bash -euo pipefail <<'EOF'
q() { xfconf-query -c "$1" -n -p "$2" "${@:3}"; }
q xfwm4 /general/theme -t string -s Greybird
q xsettings /Net/ThemeName -t string -s Greybird
q xsettings /Net/IconThemeName -t string -s elementary-xfce
q xsettings /Xfce/ThemeSchedule/Enabled -t bool -s true
q xsettings /Xfce/ThemeSchedule/Times -t string -s sunrise -t string -s sunset+30
q xsettings /Xfce/ThemeSchedule/Themes -t string -s :light -t string -s :dark
q xsettings /Xfce/ThemeSchedule/ColorSchemes -t string -s prefer-light -t string -s prefer-dark
q xsettings /Xfce/ThemeSchedule/Xfwm4 -t bool -s true -t bool -s true

xfwm4 --compositor=off &
sleep 2

export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1
export FAKETIME="@$FAKE_TIME" FAKETIME_DONT_FAKE_MONOTONIC=1
xfce4-appearance-settings &
unset LD_PRELOAD

win=$(xdotool search --sync --onlyvisible --name '^Appearance$' | head -1)
xdotool windowsize "$win" 680 540 windowactivate --sync "$win"
sleep 1
# the focus is in the theme list; Ctrl+PageDown goes to the next tab
for _ in 1 2 3 4; do xdotool key ctrl+Next; sleep 0.3; done
sleep 1
import -frame -window "$win" "$OUT"
EOF

echo "screenshot: $OUT"
