#!/bin/bash
# Installs the x86_64 Linux Parsec client on Steam Frame, run under FEX by
# Steam, with hardware video decoding. No root needed; everything goes under
# your home directory. See README.md for what each piece is for.
#
#   ./install.sh               install / update, and add the Steam shortcut
#   ./install.sh --no-steam    install / update only
set -euo pipefail

PREFIX="${PARSEC_PREFIX:-$HOME/.local/share/parsec}"
ICON_DIR="$HOME/.local/share/icons/hicolor/256x256/apps"
REPO="$(cd "$(dirname "$0")" && pwd)"

# The FEX guest rootfs on SteamOS ships glibc 2.41, so the extra x86_64
# libraries come from an Arch Linux Archive snapshot built against it.
ARCH_SNAPSHOT="${ARCH_SNAPSHOT:-https://archive.archlinux.org/repos/2025/06/01}"
PARSEC_DEB="${PARSEC_DEB:-https://builds.parsec.app/package/parsec-linux.deb}"
# FFmpeg 8.x LGPL shared build (libavcodec.so.62, libavutil.so.60, libswresample.so.6).
FFMPEG_URL="${FFMPEG_URL:-https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-linux64-lgpl-shared-8.1.tar.xz}"

GUEST_ROOTFS=/usr/share/guestos/fex-mesa
ARCH_PACKAGES=(
    libxi libxcursor libxfixes libxrender libxrandr libsm libice   # X11 extras Parsec dlopens
    libjpeg-turbo libpng                                           # image loading
    alsa-lib libpipewire pipewire pipewire-audio                   # audio via the host PipeWire
)

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# --- preflight ---------------------------------------------------------------
[ "$(uname -m)" = aarch64 ] || die "this is for Steam Frame (aarch64); this machine is $(uname -m)"
[ -f "$GUEST_ROOTFS/usr/lib/libc.so.6" ] || die "FEX guest rootfs not found at $GUEST_ROOTFS (not SteamOS on Steam Frame?)"
for tool in curl bsdtar tar xz python3 clang ld.lld patchelf; do
    command -v "$tool" >/dev/null || die "missing required tool: $tool"
done
# Replacing libraries under a running Parsec can crash it.
if pgrep -f "^$PREFIX/parsecd" >/dev/null; then
    die "Parsec is running; close it first"
fi

WORK="$(mktemp -d "${XDG_CACHE_HOME:-$HOME/.cache}/parsec-on-frame.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
fetch() { curl --http1.1 -fL --retry 3 -sS "$1" -o "$2"; }

mkdir -p "$PREFIX/lib" "$ICON_DIR" "$HOME/.parsec"

# --- Parsec ------------------------------------------------------------------
say "Downloading Parsec"
fetch "$PARSEC_DEB" "$WORK/parsec.deb"
mkdir -p "$WORK/parsec"
bsdtar -xOf "$WORK/parsec.deb" 'data.tar.*' | bsdtar -xf - -C "$WORK/parsec"
install -m755 "$WORK/parsec/usr/bin/parsecd" "$PREFIX/parsecd"
install -m644 "$WORK/parsec/usr/share/icons/hicolor/256x256/apps/parsecd.png" "$ICON_DIR/parsecd.png"
# Parsec's loader normally copies these from /usr/share/parsec/skel on first run.
for f in "$WORK"/parsec/usr/share/parsec/skel/*; do
    [ -e "$HOME/.parsec/$(basename "$f")" ] || cp "$f" "$HOME/.parsec/"
done

# --- x86_64 libraries from Arch ------------------------------------------------
say "Downloading x86_64 libraries from the Arch Linux Archive ($ARCH_SNAPSHOT)"
for repo in core extra; do
    fetch "$ARCH_SNAPSHOT/$repo/os/x86_64/$repo.db" "$WORK/$repo.db"
done
mkdir -p "$WORK/arch"
for pkg in "${ARCH_PACKAGES[@]}"; do
    file=""
    for repo in core extra; do
        file=$( (bsdtar -xOf "$WORK/$repo.db" "$pkg-[0-9]*/desc" 2>/dev/null || true) | awk '/%FILENAME%/{getline; print}')
        [ -n "$file" ] && break
    done
    [ -n "$file" ] || die "package $pkg not found in the snapshot"
    echo "    $file"
    fetch "$ARCH_SNAPSHOT/$repo/os/x86_64/$file" "$WORK/$file"
    bsdtar -xf "$WORK/$file" -C "$WORK/arch" usr/lib 2>/dev/null || true
done
A="$WORK/arch/usr/lib"
L="$PREFIX/lib"
rm -rf "$L/alsa-lib" "$L/pipewire-0.3" "$L/spa-0.2"
mkdir -p "$L/alsa-lib" "$L/pipewire-0.3" "$L/spa-0.2"
cp -a "$A"/libXi.so.6* "$A"/libXcursor.so.1* "$A"/libXfixes.so.3* "$A"/libXrender.so.1* \
      "$A"/libXrandr.so.2* "$A"/libSM.so.6* "$A"/libICE.so.6* \
      "$A"/libjpeg.so.8* "$A"/libpng16.so.16* \
      "$A"/libasound.so.2* "$A"/libpipewire-0.3.so.0* "$L/"
cp -a "$A"/alsa-lib/libasound_module_{pcm,ctl}_pipewire.so "$L/alsa-lib/"
cp -a "$A"/pipewire-0.3/*.so "$L/pipewire-0.3/"
cp -a "$A"/spa-0.2/support "$A"/spa-0.2/audioconvert "$L/spa-0.2/"

# --- FFmpeg ------------------------------------------------------------------
say "Downloading FFmpeg"
fetch "$FFMPEG_URL" "$WORK/ffmpeg.tar.xz"
tar -xJf "$WORK/ffmpeg.tar.xz" -C "$WORK"
F=$(echo "$WORK"/ffmpeg-*/lib)
for so in libavcodec.so.62 libavutil.so.60 libswresample.so.6; do
    [ -e "$F/$so" ] || die "$FFMPEG_URL doesn't contain $so (need an FFmpeg 8.x build)"
done
rm -f "$L"/libavcodec* "$L"/libavutil.so* "$L"/libswresample.so*
cp -a "$F"/libavutil.so.60* "$F"/libswresample.so.6* "$L/"
# The real libavcodec gets a different soname; libavcodec.so.62 is the shim.
cp -L "$F/libavcodec.so.62" "$L/libavcodec-real.so.62"
patchelf --set-soname libavcodec-real.so.62 "$L/libavcodec-real.so.62"

# --- shim ----------------------------------------------------------------------
say "Building the libavcodec shim"
# -O1 on purpose: the -O2 build crashes under FEX.
clang -target x86_64-linux-gnu -O1 -fPIC -fno-stack-protector -shared -nostdlib \
    -fuse-ld=lld -Wl,-soname,libavcodec.so.62 -Wl,--no-undefined \
    -Wno-builtin-requires-header \
    -o "$L/libavcodec.so.62" "$REPO/src/avdec_v4l2.c" \
    -L"$GUEST_ROOTFS/usr/lib" -l:libc.so.6 \
    -L"$L" -l:libavcodec-real.so.62 -l:libavutil.so.60
# Preloaded: caps FFmpeg's infinite waits on the V4L2 decoder (poll_timeout.c).
clang -target x86_64-linux-gnu -O1 -fPIC -fno-stack-protector -shared -nostdlib \
    -fuse-ld=lld -Wl,--no-undefined \
    -o "$L/poll_timeout.so" "$REPO/src/poll_timeout.c" \
    -L"$GUEST_ROOTFS/usr/lib" -l:libc.so.6
install -m755 "$REPO/src/parsec.sh" "$PREFIX/parsec.sh"
install -m755 "$REPO/src/parsec-desktop.sh" "$PREFIX/parsec-desktop.sh"
install -m755 "$REPO/src/parsec-force-quit.sh" "$PREFIX/parsec-force-quit.sh"
install -m755 "$REPO/src/set-window-icon.py" "$PREFIX/set-window-icon.py"

# --- desktop entry (Steam Frame dashboard "Launch Program" menu, app menus) ----
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/parsec.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Parsec
Comment=Remote desktop streaming (x86_64 client under FEX)
Exec=$PREFIX/parsec-desktop.sh
Icon=$ICON_DIR/parsecd.png
Terminal=false
Categories=Network;RemoteAccess;
EOF
# A hung Parsec ignores the dashboard's close; this SIGKILLs it.
cat > "$HOME/.local/share/applications/parsec-force-quit.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Parsec (Force Quit)
Comment=Kill a frozen Parsec
Exec=$PREFIX/parsec-force-quit.sh
Icon=$ICON_DIR/parsecd.png
Terminal=false
Categories=Network;RemoteAccess;
EOF

# --- Steam shortcut ------------------------------------------------------------
if [ "${1:-}" != "--no-steam" ]; then
    say "Adding the Steam shortcut"
    python3 "$REPO/tools/steam_shortcut.py" add "$PREFIX/parsec.sh" "$ICON_DIR/parsecd.png" || cat <<EOF
Couldn't add the shortcut automatically. Add it by hand in desktop mode:
  Steam > Add a Game > Add a Non-Steam Game > $PREFIX/parsec.sh
  then Properties > Compatibility > force "FEX-Emu" (fex-stable).
EOF
fi

say "Done. Launch \"Parsec\" from your Steam library, or from the dashboard's Launch Program menu."
if command -v iw >/dev/null && iw dev 2>/dev/null | grep -q 'type managed'; then
    ifc=$(iw dev | awk '/Interface/{i=$2} /type managed/{print i; exit}')
    if iw dev "$ifc" get power_save 2>/dev/null | grep -q on; then
        cat <<EOF

Tip: Wi-Fi power saving is on for $ifc. It causes large latency spikes while
streaming. Turn it off for your network with:
  nmcli connection modify "<your network>" 802-11-wireless.powersave 2
and then reconnect.
EOF
    fi
fi
