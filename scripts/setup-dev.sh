#!/usr/bin/env bash
# One-time setup of the local dev machine (run with sudo):
#   sudo scripts/setup-dev.sh
# Enables deb-src for the Ubuntu archive and installs the toolchain for
# building XFCE components and Debian packages.
set -euo pipefail

SRC=/etc/apt/sources.list.d/ubuntu.sources
if ! grep -q 'deb-src' "$SRC"; then
  cp "$SRC" "$SRC.bak-xfcepatches"
  sed -i 's/^Types: deb$/Types: deb deb-src/' "$SRC"
fi

apt-get update
apt-get install -y --no-install-recommends \
  build-essential devscripts debhelper dh-exec equivs quilt fakeroot \
  git git-buildpackage pkgconf \
  meson ninja-build autoconf automake libtool gettext intltool \
  xfce4-dev-tools gtk-doc-tools gobject-introspection libgirepository1.0-dev \
  libglib2.0-dev libgtk-3-dev libx11-dev libwnck-3-dev \
  libxfce4util-dev libxfce4ui-2-dev libxfconf-0-dev libxfce4panel-2.0-dev \
  libxfce4windowing-dev libexo-2-dev libgarcon-1-dev libthunarx-3-dev \
  clang-format gdb valgrind
