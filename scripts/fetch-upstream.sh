#!/usr/bin/env bash
# Clone the upstream git repo of an XFCE component at the version installed
# on this system, into src/<pkg> on a branch "work":
#
#   scripts/fetch-upstream.sh <source-package> [gitlab-path]
#
# e.g. scripts/fetch-upstream.sh thunar
#      scripts/fetch-upstream.sh xfdesktop4 xfce/xfdesktop
# Develop there with normal commits, then run scripts/export-patches.sh <pkg>.
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [gitlab-path]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$ROOT/src/$PKG
NAME=${PKG%4}                     # xfdesktop4 -> xfdesktop
[ "$PKG" = xfwm4 ] && NAME=xfwm4

# Upstream version of the installed binary package of the same name (or any
# binary built from this source).
VER=$(dpkg-query -W -f='${source:Upstream-Version}\n' "$PKG" 2>/dev/null | head -1 || true)
[ -n "$VER" ] || VER=$(dpkg-query -W -f='${source:Package} ${source:Upstream-Version}\n' \
  | awk -v p="$PKG" '$1==p {print $2; exit}')
[ -n "$VER" ] || { echo "error: cannot find installed version for $PKG" >&2; exit 1; }

if [ -n "${2:-}" ]; then
  GROUPS_TRY=("$2")
else
  GROUPS_TRY=("xfce/$NAME" "apps/$NAME" "panel-plugins/$NAME" "thunar-plugins/$NAME" "bindings/$NAME")
fi
URL=
for g in "${GROUPS_TRY[@]}"; do
  if git ls-remote --exit-code "https://gitlab.xfce.org/$g.git" HEAD >/dev/null 2>&1; then
    URL="https://gitlab.xfce.org/$g.git"; break
  fi
done
[ -n "$URL" ] || { echo "error: upstream repo not found, pass gitlab path explicitly" >&2; exit 1; }

TAG="$NAME-$VER"
echo "upstream: $URL  tag: $TAG"
if [ ! -d "$DEST/.git" ]; then
  git clone "$URL" "$DEST"
fi
git -C "$DEST" fetch --tags origin
git -C "$DEST" rev-parse -q --verify "refs/tags/$TAG" >/dev/null \
  || { echo "error: tag $TAG not found; available:" >&2; git -C "$DEST" tag -l "$NAME-*" | tail >&2; exit 1; }
git -C "$DEST" switch -C work "$TAG" 2>/dev/null || git -C "$DEST" switch work
mkdir -p "$ROOT/packages/$PKG"
echo "$TAG" > "$ROOT/packages/$PKG/BASE"
echo "ready: $DEST (branch work, based on $TAG)"
