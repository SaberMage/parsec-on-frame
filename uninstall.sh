#!/bin/bash
# Removes what install.sh added. Leaves ~/.parsec (your Parsec login and
# settings) unless you pass --purge.
set -euo pipefail
PREFIX="${PARSEC_PREFIX:-$HOME/.local/share/parsec}"
REPO="$(cd "$(dirname "$0")" && pwd)"

python3 "$REPO/tools/steam_shortcut.py" remove || echo "Remove the Parsec shortcut from Steam by hand."
rm -rf "$PREFIX"
rm -f "$HOME/.local/share/icons/hicolor/256x256/apps/parsecd.png" "$HOME/.local/share/applications/parsec.desktop"
if [ "${1:-}" = "--purge" ]; then
    rm -rf "$HOME/.parsec" "$HOME/.parsec-persistent"
fi
echo "Parsec removed."
