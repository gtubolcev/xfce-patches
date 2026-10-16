#!/usr/bin/env bash
# Turn commits on src/<pkg> (branch work) since the upstream base tag into
# packages/<pkg>/patches/*.patch + series, ready for CI.
#
#   scripts/export-patches.sh <source-package>
set -euo pipefail

PKG=${1:?usage: $0 <source-package>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src/$PKG
BASE=$(cat "$ROOT/packages/$PKG/BASE")
OUT=$ROOT/packages/$PKG/patches

rm -rf "$OUT"
mkdir -p "$OUT"
git -C "$SRC" format-patch --zero-commit --no-signature -o "$OUT" "$BASE..work"
(cd "$OUT" && ls -1 ./*.patch | sed 's|^\./||' | sort > series)
echo "exported $(wc -l < "$OUT/series") patch(es) to $OUT"
