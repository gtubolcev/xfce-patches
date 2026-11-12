#!/usr/bin/env bash
# Build an Arch Linux package with our patches applied.
#
#   scripts/build-arch.sh <source-package> [out-dir]
#
# Copies packages/<pkg>/arch/PKGBUILD, its extra files and patches/*.patch
# into a build directory and runs makepkg there. With RELEASE=v0.1.2 the package version gets it (4.20.5_0.1.2), so that each release upgrades the
# previous one. Resulting .pkg.tar.zst files land in out-dir.
#
# Meant for CI, as root in an archlinux container: installs base-devel and
# builds as an unprivileged user, since makepkg refuses to run as root.
# Locally (not root) it just runs makepkg -s.
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [out-dir]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(realpath -m "${2:-$ROOT/out}")
WORK=${WORK:-$ROOT/_build}/$PKG-arch
PKGDIR=$ROOT/packages/$PKG/arch

RELEASE=${RELEASE:-}
export XFCEPATCHES_RELEASE=${RELEASE#v}

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
find "$PKGDIR" -maxdepth 1 -type f ! -name BASE -exec cp {} "$WORK"/ \;
cp "$PKGDIR"/patches/*.patch "$WORK"/

if [ "$(id -u)" -eq 0 ]; then
  pacman -Syu --noconfirm --needed base-devel git
  id builder >/dev/null 2>&1 || useradd -m builder
  echo 'builder ALL=(ALL) NOPASSWD: /usr/bin/pacman' > /etc/sudoers.d/builder
  chown -R builder "$WORK"
  su builder -c "cd '$WORK' && XFCEPATCHES_RELEASE='$XFCEPATCHES_RELEASE' makepkg -s --noconfirm"
else
  (cd "$WORK" && makepkg -s --noconfirm)
fi

find "$WORK" -maxdepth 1 -name '*.pkg.tar.zst' ! -name '*-debug-*' -exec cp -v {} "$OUT"/ \;
