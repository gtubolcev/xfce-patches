#!/usr/bin/env bash
# Build an Ubuntu source package with our patches applied.
#
#   scripts/build-package.sh <source-package> [out-dir]
#
# Fetches the source via `apt-get source` (needs deb-src enabled), copies
# packages/<pkg>/patches/*.patch into debian/patches (on an older Ubuntu
# release with a port of its own: packages/<pkg>/<codename>/patches), bumps the version with a
# local suffix (e.g. 4.20.7-1 -> 4.20.7-1+xfcepatches1, or with RELEASE=v0.1.2
# -> 4.20.7-1+xfcepatches1+v0.1.2, so that each release upgrades the previous
# one), installs the build dependencies and runs dpkg-buildpackage. Resulting
# .deb files land in out-dir.
#
# Works both in CI (as root in an ubuntu:26.04 or 24.04 container) and locally
# (set SUDO=sudo, or it is picked automatically when not root).
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [out-dir]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$ROOT/out}")
WORK=${WORK:-$ROOT/_build}/$PKG
SUFFIX=${SUFFIX:-+xfcepatches}

if [ "$(id -u)" -ne 0 ]; then SUDO=${SUDO-sudo}; else SUDO=${SUDO-}; fi
export DEBFULLNAME=${DEBFULLNAME:-"XFCE-patches CI"}
export DEBEMAIL=${DEBEMAIL:-"noreply@users.noreply.github.com"}
. /etc/os-release

PATCHDIR=$ROOT/packages/$PKG/patches
[ -d "$ROOT/packages/$PKG/$VERSION_CODENAME" ] && PATCHDIR=$ROOT/packages/$PKG/$VERSION_CODENAME/patches

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
cd "$WORK"
apt-get source "$PKG"
SRCDIR=$(find . -mindepth 1 -maxdepth 1 -type d | head -1)
cd "$SRCDIR"

mapfile -t PATCHES < <(
  if [ -f "$PATCHDIR/series" ]; then
    sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$PATCHDIR/series"
  elif [ -d "$PATCHDIR" ]; then
    (cd "$PATCHDIR" && ls -1 ./*.patch 2>/dev/null | sed 's|^\./||' | sort)
  fi
)

if [ ${#PATCHES[@]} -gt 0 ]; then
  grep -q '3.0 (quilt)' debian/source/format \
    || { echo "error: $PKG is not '3.0 (quilt)', cannot add patches" >&2; exit 1; }
  mkdir -p debian/patches
  touch debian/patches/series
  for p in "${PATCHES[@]}"; do
    cp "$PATCHDIR/$p" "debian/patches/xfcepatches-$p"
    echo "xfcepatches-$p" >> debian/patches/series
  done
  # apt-get source already applied the distro patches; apply ours on top.
  export QUILT_PATCHES=debian/patches
  quilt push -a --fuzz=0 || [ $? -eq 2 ]
  MSG="Apply XFCE-patches: ${PATCHES[*]}"
else
  MSG="Rebuild without changes (XFCE-patches pipeline test)"
fi

if [ -n "${RELEASE:-}" ]; then
  dch --newversion "$(dpkg-parsechangelog -S Version)${SUFFIX}1+$RELEASE" \
    --distribution "$VERSION_CODENAME" --force-distribution "$MSG"
else
  dch --local "$SUFFIX" --distribution "$VERSION_CODENAME" --force-distribution "$MSG"
fi

$SUDO mk-build-deps --install --remove \
  --tool 'apt-get -y --no-install-recommends -o Debug::pkgProblemResolver=yes' \
  debian/control

dpkg-buildpackage -us -uc -b -j"$(nproc)"

cd ..
cp -v ./*.deb "$OUT"/
