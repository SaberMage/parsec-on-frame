#!/bin/bash
# Native launcher for Parsec outside Steam's game launcher (e.g. the Steam Frame
# dashboard's "Launch Program" menu): runs parsec.sh under FEX ourselves, the
# way Steam's FEX compatibility tool would.
HERE=$(dirname "$(readlink -f "$0")")
FEX_TOOL="$HOME/.local/share/Steam/steamapps/common/FEX-Emu/fex-compat-tool"
if [ ! -x "$FEX_TOOL" ]; then
    echo "FEX-Emu isn't installed yet; launch any x86 game (or the Parsec shortcut) from Steam once." >&2
    exit 1
fi
# One Parsec at a time (the menu happily starts a second one otherwise).
if pgrep -f "^$HERE/parsecd" >/dev/null; then
    echo "Parsec is already running. If it's frozen, use \"Parsec (Force Quit)\"." >&2
    exit 0
fi
export STEAM_COMPAT_DATA_PATH="$HERE/fex-data"
mkdir -p "$STEAM_COMPAT_DATA_PATH"
# Not a Gamescope app window here; keep the Gamescope Vulkan WSI layer out of it.
export ENABLE_GAMESCOPE_WSI=0
# Parsec doesn't set a window icon; the dashboard shows windows' _NET_WM_ICON.
# The helper follows this PID, which exec below hands over to the FEX process.
ICON="$HOME/.local/share/icons/hicolor/256x256/apps/parsecd.png"
python3 "$HERE/set-window-icon.py" parsecd "$ICON" $$ >/dev/null 2>&1 &
exec python3 "$FEX_TOOL" waitforexitandrun -- "$HERE/parsec.sh" "$@"
