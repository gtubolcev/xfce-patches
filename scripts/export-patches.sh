#!/usr/bin/env bash
# Turn commits on src/<pkg> (branch work) since the upstream base tag into
# packages/<pkg>/patches/*.patch + series, ready for CI.
#
#   scripts/export-patches.sh <source-package> [codename]
#
# With a codename (e.g. noble), the port for that older Ubuntu release is
# exported instead: commits on the branch named after it, into
# packages/<pkg>/<codename>/patches, based on packages/<pkg>/<codename>/BASE.
set -euo pipefail

PKG=${1:?usage: $0 <source-package> [codename]}
DIST=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src/$PKG
PKGDIR=$ROOT/packages/$PKG${DIST:+/$DIST}
BRANCH=${DIST:-work}
BASE=$(cat "$PKGDIR/BASE")
OUT=$PKGDIR/patches

rm -rf "$OUT"
mkdir -p "$OUT"
git -C "$SRC" format-patch --zero-commit --no-signature -o "$OUT" "$BASE..$BRANCH"
(cd "$OUT" && ls -1 ./*.patch | sed 's|^\./||' | sort > series)
echo "exported $(wc -l < "$OUT/series") patch(es) to $OUT"
