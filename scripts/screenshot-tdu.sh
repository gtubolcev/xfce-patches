#!/usr/bin/env bash
# Take the screenshot of the Disk Usage tab in Thunar's Properties dialog for
# the README (docs/thunar-disk-usage.png).
#
#   scripts/screenshot-tdu.sh <dir-with-debs> [out.png]
#
# Meant for CI and docker, as root in an ubuntu:26.04 container: installs the
# thunar-disk-usage-plugin .deb from <dir-with-debs> and Thunar, fills a folder
# with files of fixed sizes, opens its Properties dialog on a virtual X server
# (Xvfb) with xfwm4 for the window frame and takes the picture with
# ImageMagick. Theme, font and the folder are fixed, so the picture only
# changes when the plugin does.
#
# Locally:
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:26.04 scripts/screenshot-tdu.sh out docs/thunar-disk-usage.png
set -euo pipefail

DEBS=$(realpath "${1:?usage: $0 <dir-with-debs> [out.png]}")
OUT=$(realpath -m "${2:-docs/thunar-disk-usage.png}")

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
  "$DEBS"/thunar-disk-usage-plugin_*.deb thunar \
  xvfb xfwm4 xfconf dbus dbus-x11 xdotool imagemagick x11-utils libglib2.0-bin \
  greybird-gtk-theme elementary-xfce-icon-theme fonts-noto-core

export HOME=$(mktemp -d) LANG=C.UTF-8 DISPLAY=:99
mkdir -p "$HOME/.config/gtk-3.0"
cat > "$HOME/.config/gtk-3.0/settings.ini" <<'EOF'
[Settings]
gtk-theme-name=Greybird
gtk-icon-theme-name=elementary-xfce
gtk-font-name=Noto Sans 11
EOF

# the folder to show: <number of files> <size of each file in MiB> <path>
DIR=$HOME/Media
while read -r n mib path; do
  mkdir -p "$DIR/$path"
  for i in $(seq "$n"); do
    fallocate -l "${mib}M" "$DIR/$path/file-$i"
  done
done <<'EOF'
40 4 Photos/2024/Holidays
25 4 Photos/2024/Family
12 25 Photos/2024/Raw
30 4 Photos/2025/Mountains
8 25 Photos/2025/Raw
15 3 Photos/2025/Garden
3 120 Videos/Trips/Norway
2 90 Videos/Trips/Alps
12 15 Videos/Clips
24 9 Music/Albums/Jazz
30 11 Music/Albums/Classical
18 8 Music/Albums/Rock
10 30 Music/Podcasts
4 80 Archive/Backups
60 1 Archive/Old projects/Website
20 2 Archive/Old projects/Thesis
30 2 Documents/Scans
12 6 Documents/Books
EOF

Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
XVFB=$!
trap 'kill $XVFB 2>/dev/null || true' EXIT
for _ in $(seq 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done

export OUT DIR
dbus-run-session -- bash -euo pipefail <<'EOF'
q() { xfconf-query -c "$1" -n -p "$2" "${@:3}"; }
q xfwm4 /general/theme -t string -s Greybird
q xsettings /Net/ThemeName -t string -s Greybird
q xsettings /Net/IconThemeName -t string -s elementary-xfce

xfwm4 --compositor=off &
sleep 2
thunar --daemon &
for _ in $(seq 50); do
  gdbus introspect --session --dest org.xfce.FileManager --object-path /org/xfce/FileManager >/dev/null 2>&1 && break
  sleep 0.2
done
gdbus call --session --dest org.xfce.FileManager --object-path /org/xfce/FileManager \
  --method org.xfce.FileManager.DisplayFileProperties "file://$DIR" "$DISPLAY" "" >/dev/null

win=$(xdotool search --sync --onlyvisible --name '^Media - Properties$' | head -1)
xdotool windowsize "$win" 520 640 windowactivate --sync "$win"
sleep 1
# General, Emblems, Permissions, then the plugin's tab
for _ in 1 2 3; do xdotool key ctrl+Next; sleep 0.3; done
sleep 2
import -frame -window "$win" "$OUT"
EOF

echo "screenshot: $OUT"
