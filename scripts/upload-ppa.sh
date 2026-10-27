#!/usr/bin/env bash
# Build a signed Ubuntu source package with our patches and upload it to a
# Launchpad PPA, which then builds the .debs for amd64 and the other arches.
#
#   scripts/upload-ppa.sh <source-package> [codename]
#
# codename defaults to the running release; an older one (e.g. noble) uses the
# port in packages/<pkg>/<codename>/patches. PPA=... overrides the target
# (ppa:gtubolcev/xfce-patches).
#
# The version gets a "+xfcepatches<timestamp>" suffix, so every upload is newer
# than the previous one and than the Ubuntu package it is based on. The orig
# tarball is not uploaded, Launchpad takes it from the Ubuntu archive.
#
# The Ubuntu source package is taken from Launchpad (the newest version
# published for that release). Needs a GPG key known to Launchpad;
# SIGN_KEY=... picks the key (else the one of DEBEMAIL); NO_UPLOAD=1 only
# builds the source package (unsigned) into _build/ppa/.
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [codename]}
. /etc/os-release
DIST=${2:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/xfce-patches}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$ROOT/_build/ppa/$PKG-$DIST
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}

PATCHDIR=$ROOT/packages/$PKG/patches
[ -d "$ROOT/packages/$PKG/$DIST" ] && PATCHDIR=$ROOT/packages/$PKG/$DIST/patches

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"
UBUNTU_VERSION=$(curl -fsS "https://api.launchpad.net/devel/ubuntu/+archive/primary?ws.op=getPublishedSources&source_name=$PKG&exact_match=true&status=Published&distro_series=https://api.launchpad.net/devel/ubuntu/$DIST" \
  | python3 -c 'import json, sys; print("\n".join(e["source_package_version"] for e in json.load(sys.stdin)["entries"]))' \
  | sort -V | tail -1)
[ -n "$UBUNTU_VERSION" ] || { echo "error: no $PKG in Ubuntu $DIST" >&2; exit 1; }
# Launchpad answers 502 now and then, so retry
for try in 1 2 3 4 5; do
  dget --download-only --allow-unauthenticated \
    "https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/$PKG/$UBUNTU_VERSION/${PKG}_${UBUNTU_VERSION#*:}.dsc" && break
  [ "$try" -lt 5 ] || exit 1
  rm -f ./*
  sleep 60
done
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
dch --newversion "$VERSION" --distribution "$DIST" --force-distribution \
  "Apply XFCE-patches (https://github.com/gtubolcev/xfce-patches): ${PATCHES[*]}"

if [ -n "${NO_UPLOAD:-}" ]; then
  dpkg-buildpackage -S -sd -d -nc -us -uc
  exit 0
fi

dpkg-buildpackage -S -sd -d -nc ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
