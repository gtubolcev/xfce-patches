#!/usr/bin/env bash
# Build a signed Ubuntu source package with our patches and upload it to a
# Launchpad PPA, which then builds the .debs for amd64 and the other arches.
#
#   scripts/upload-ppa.sh <source-package> [ppa]      (ppa: ppa:gtubolcev/xfce-patches)
#
# The version gets a "+xfcepatches<timestamp>" suffix, so every upload is newer
# than the previous one and than the Ubuntu package it is based on. The orig
# tarball is not uploaded, Launchpad takes it from the Ubuntu archive.
#
# Needs deb-src enabled (apt-get source) and a GPG key known to Launchpad;
# NO_UPLOAD=1 only builds the source package (unsigned) into _build/ppa/.
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [ppa]}
PPA=${2:-ppa:gtubolcev/xfce-patches}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$ROOT/_build/ppa/$PKG
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}
. /etc/os-release

PATCHDIR=$ROOT/packages/$PKG/patches
[ -d "$ROOT/packages/$PKG/$VERSION_CODENAME" ] && PATCHDIR=$ROOT/packages/$PKG/$VERSION_CODENAME/patches

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"
apt-get source --download-only "$PKG"
dpkg-source -x ./*.dsc
SRCDIR=$(find . -mindepth 1 -maxdepth 1 -type d | head -1)
cd "$SRCDIR"

mapfile -t PATCHES < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$PATCHDIR/series")
for p in "${PATCHES[@]}"; do
  cp "$PATCHDIR/$p" "debian/patches/xfcepatches-$p"
  echo "xfcepatches-$p" >> debian/patches/series
  # same check as quilt --fuzz=0 in CI; the source package keeps them unapplied
  patch -p1 -F0 --dry-run -s < "debian/patches/xfcepatches-$p" >/dev/null 2>&1 \
    || { echo "error: $p does not apply" >&2; exit 1; }
done

VERSION="$(dpkg-parsechangelog -S Version)+xfcepatches$(date -u +%Y%m%d%H%M)"
dch --newversion "$VERSION" --distribution "$VERSION_CODENAME" --force-distribution \
  "Apply XFCE-patches (https://github.com/gtubolcev/xfce-patches): ${PATCHES[*]}"

if [ -n "${NO_UPLOAD:-}" ]; then
  dpkg-buildpackage -S -sd -d -us -uc
  exit 0
fi

dpkg-buildpackage -S -sd -d
cd ..
dput "$PPA" ./*_source.changes
