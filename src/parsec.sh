#!/bin/sh
# Launches the x86_64 Parsec client (run under FEX by Steam) with its bundled
# x86_64 libraries. lib/libavcodec.so.62 is the avdec_v4l2.c shim.
#
# Environment knobs (set in the Steam shortcut's launch options, before %command%):
#   PARSEC_HWDEC=0   use FFmpeg's software decoders instead of the hardware one
#   PARSEC_AVLOG=1   log per-second decode stats to ~/.parsec/stderr.txt
#   PARSEC_DUMP=path record the raw compressed video stream to path
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Route ALSA audio to the host PipeWire server through bundled x86_64 plugins.
export ALSA_PLUGIN_DIR="$HERE/lib/alsa-lib"
export PIPEWIRE_MODULE_DIR="$HERE/lib/pipewire-0.3"
export SPA_PLUGIN_DIR="$HERE/lib/spa-0.2"
mkdir -p "$HOME/.parsec"
exec "$HERE/parsecd" "$@" >"$HOME/.parsec/stderr.txt" 2>&1
