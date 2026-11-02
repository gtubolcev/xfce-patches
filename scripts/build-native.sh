#!/usr/bin/env bash
# Build the .deb of a subproject of our own (a directory with debian/ in the
# repo root, e.g. thunar-disk-usage), as opposed to build-package.sh, which
# patches an Ubuntu source package.
#
#   scripts/build-native.sh <dir> [out-dir]
#
# The version gets the release tag and the codename, so that the packages of
# different releases have different names: 0.1.0~resolute, with
# RELEASE=v0.5.0 0.1.0+v0.5.0~resolute.
#
# Works in CI (as root in an ubuntu or debian container) and locally (SUDO
# is picked automatically when not root).
set -euo pipefail

DIR=${1:?usage: $0 <dir> [out-dir]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$ROOT/out}")
WORK=${WORK:-$ROOT/_build}/$DIR

if [ "$(id -u)" -ne 0 ]; then SUDO=${SUDO-sudo}; else SUDO=${SUDO-}; fi
export DEBFULLNAME=${DEBFULLNAME:-"XFCE-patches CI"}
export DEBEMAIL=${DEBEMAIL:-"noreply@users.noreply.github.com"}
. /etc/os-release

rm -rf "$WORK"
mkdir -p "$WORK/src" "$OUT"
# arch/ is the Arch Linux packaging, PLAN.md a private file
tar -C "$ROOT/$DIR" --exclude=./arch --exclude=./PLAN.md -cf - . | tar -C "$WORK/src" -xf -
cd "$WORK/src"

VERSION="$(dpkg-parsechangelog -S Version)${RELEASE:++$RELEASE}~$VERSION_CODENAME"
dch --newversion "$VERSION" --force-bad-version --distribution "$VERSION_CODENAME" \
  --force-distribution "Build for $PRETTY_NAME."

$SUDO mk-build-deps --install --remove \
  --tool 'apt-get -y --no-install-recommends -o Debug::pkgProblemResolver=yes' \
  debian/control

dpkg-buildpackage -us -uc -b -j"$(nproc)"

cd ..
find . -maxdepth 1 -name '*.deb' ! -name '*-dbgsym_*' -exec cp -v {} "$OUT"/ \;
