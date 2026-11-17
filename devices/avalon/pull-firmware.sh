#!/usr/bin/env bash
# devices/avalon/pull-firmware.sh - Extract OnePlus Nord 4 (avalon) firmware from a user-supplied dump path or stock.zip.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="${1:-${SRC:-${STOCK_SRC:-}}}"

if [ -n "$SRC" ]; then
    exec python3 "$HERE/extract-blobs.py" --out "$ROOT" "$SRC"
else
    exec python3 "$HERE/extract-blobs.py" --out "$ROOT"
fi
