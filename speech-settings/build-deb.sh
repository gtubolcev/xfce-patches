#!/bin/sh
# Builds the xfce4-speech-settings .deb in the speech-build docker image.
# BUILD: scratch dir for the meson build; OUT: where the .deb goes.
set -e
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$HERE/../_tmp/speech-build/deb}
OUT=${OUT:-/home/grigory/Storage/Claude/speech-settings/deb}
VER=0.1.0-3~local
P=xfce4-speech-settings

mkdir -p "$BUILD" "$OUT"
BUILD=$(cd "$BUILD" && pwd)
rm -rf "$BUILD/b" "$BUILD/root"
docker run --rm -u $(id -u):$(id -g) -v $HERE:/src:ro -v $BUILD:$BUILD -w $BUILD \
  speech-build sh -c "
    meson setup b /src --prefix=/usr --buildtype=release -Dx11=enabled >/dev/null &&
    ninja -C b && meson test -C b --print-errorlogs &&
    DESTDIR=$BUILD/root ninja -C b install >/dev/null"

W=$BUILD/root
D=$W/usr/share/doc/$P
strip $W/usr/bin/$P
mkdir -p $W/DEBIAN $D
cat > $D/copyright <<X
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: xfce4-speech-settings

Files: *
Copyright: 2026 Grigory Tuboltsev
License: GPL-2+
 On Debian systems, the full text of the GNU General Public License
 version 2 can be found in /usr/share/common-licenses/GPL-2.
X
cat > $W/DEBIAN/control <<X
Package: $P
Version: $VER
Architecture: amd64
Maintainer: Grigory Tuboltsev <noreply@localhost>
Installed-Size: $(du -sk $W/usr | cut -f1)
Depends: libc6, libglib2.0-0t64, libgtk-3-0t64 (>= 3.24), libxfce4ui-2-0 (>= 4.18), libxfce4util7 (>= 4.18), libspeechd2 (>= 0.11), speech-dispatcher
Section: x11
Priority: optional
Description: Speech settings dialog for Xfce
 Chooses the text-to-speech engine and voice of speech-dispatcher and sets
 the rate, pitch and volume. Appears in the Xfce settings manager.
X
dpkg-deb --root-owner-group -Zzstd --build $W "$OUT/${P}_${VER}_amd64.deb"
