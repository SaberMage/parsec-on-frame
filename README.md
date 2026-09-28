# parsec-on-frame

Run the **Parsec** remote desktop client on **Steam Frame**, with hardware video
decoding and low latency.

Parsec only ships an x86_64 Linux client, and Steam Frame is an ARM64
(Snapdragon) device. This project runs the official x86_64 client under
[FEX](https://github.com/FEX-Emu/FEX), the x86 emulator that SteamOS ships and
Steam uses for x86 games, and fixes the problems that come up along the way. The
biggest fix is a small stand-in `libavcodec` that routes video decoding to the
Snapdragon's hardware decoder and fixes its latency.

Nothing here needs root, and nothing outside your home directory is touched.
No third-party binaries are redistributed: the installer downloads Parsec,
FFmpeg and a few libraries from their official sources, and compiles the shim
locally.

> Status: works well for desktop streaming. Latency is much better than a naive
> setup but still a bit behind a native desktop client. Tested on SteamOS
> 0.3.0 (VR variant, build 20260922) with Parsec 150-104a and FFmpeg 8.1.

## Install

In desktop mode (or over SSH), with Steam running:

```sh
git clone https://github.com/SaberMage/parsec-on-frame
cd parsec-on-frame
./install.sh
```

Then launch **Parsec**, log in, and connect. There are two ways to launch it:

- **As a game:** from your Steam library. It runs full-screen through Steam's
  FEX compatibility tool, like any x86 game.
- **As a desktop window:** from the Steam Frame dashboard's **+ → Launch
  Program** menu. It opens in its own window next to your other desktop apps.
  The menu lists `.desktop` launchers, so this also puts Parsec in the desktop
  mode app menu.

- FEX is downloaded by Steam the first time something needs it. If Parsec won't
  start the first time, give Steam a minute to finish downloading FEX, or
  launch any x86 game once.
- In Parsec's **Settings → Client**, set **Decoder** to **Software**. That's
  FFmpeg's decoder path in Parsec, which this project routes to the hardware
  decoder. H.264 and H.265 both work.
- **Turn off Wi-Fi power saving.** It causes latency spikes of hundreds of
  milliseconds while streaming:

  ```sh
  nmcli connection modify "<your network>" 802-11-wireless.powersave 2
  nmcli connection up "<your network>"
  ```

To update (for example after Parsec or SteamOS updates), run `./install.sh`
again. To remove everything, run `./uninstall.sh`; add `--purge` to also delete
your Parsec login and settings in `~/.parsec`.

`./install.sh --no-steam` skips the Steam shortcut. To add it by hand:
**Steam → Add a Game → Add a Non-Steam Game →
`~/.local/share/parsec/parsec.sh`**, then under **Properties → Compatibility**,
force **FEX-Emu**.

## What's installed

```
~/.local/share/parsec/
├── parsecd                  official Parsec x86_64 loader (downloads its own parsecd-*.so into ~/.parsec)
├── parsec.sh                launcher: sets library/audio paths, logs to ~/.parsec/stderr.txt
├── parsec-desktop.sh        native launcher for Launch Program: runs parsec.sh under FEX itself
├── fex-data/                FEX state for launches outside Steam
└── lib/
    ├── libavcodec.so.62     the shim (src/avdec_v4l2.c)
    ├── libavcodec-real.so.62, libavutil.so.60, libswresample.so.6   FFmpeg 8.1 (BtbN LGPL build)
    ├── libX*, libSM, libICE, libjpeg, libpng                        missing from the FEX rootfs
    └── libasound, libpipewire, alsa-lib/, pipewire-0.3/, spa-0.2/   x86 ALSA → host PipeWire audio
```

Steam gets a non-Steam shortcut named **Parsec** with FEX (`fex-stable`) as its
compatibility tool. `tools/steam_shortcut.py` creates it through the Steam
client's local DevTools port, so Steam doesn't need a restart.

`~/.local/share/applications/parsec.desktop` is what the dashboard's **Launch
Program** menu picks up. That menu lists whatever Steam's non-Steam-app scanner
(`SteamClient.Apps.ScanForInstalledNonSteamApps`) finds in the standard
`.desktop` locations, minus a small blocklist of system tools. Choosing an entry
runs its command line directly on the device (`LaunchNonSteamApp`) with no
compatibility tool, so `parsec-desktop.sh` starts FEX's `fex-compat-tool`
itself. It also sets `ENABLE_GAMESCOPE_WSI=0`: the window is a normal desktop
window, not a Gamescope game, and with the Gamescope Vulkan layer active,
Parsec shows a "Hooking has failed" error dialog.

## How it works (and what went wrong without it)

1. **Missing x86 libraries.** Valve's FEX guest rootfs
   (`/usr/share/guestos/fex-mesa`) is minimal. Parsec also needs a few X11
   extras, libjpeg/libpng, ALSA and FFmpeg. These come from the
   [Arch Linux Archive](https://archive.archlinux.org/) snapshot of 2025-06-01,
   because that rootfs ships glibc 2.41 and current Arch builds need newer.
2. **Audio.** An x86 `libasound` would try to load the host's ARM64 PipeWire
   plugin. The launcher points `ALSA_PLUGIN_DIR`, `PIPEWIRE_MODULE_DIR` and
   `SPA_PLUGIN_DIR` at bundled x86 copies, which talk to the host PipeWire
   server over its socket.
3. **"No decoders" / error -17.** Parsec looks up `avcodec_close()`, which
   FFmpeg 8 removed. Without it, Parsec silently rejects FFmpeg and has no
   decoders at all, so the Decoder list is empty. The shim provides it.
4. **Hardware decoding.** The Frame has no VA-API driver and no Vulkan Video
   decode, but its Qualcomm **Iris** decoder is a V4L2 mem2mem device, and FEX
   passes V4L2 ioctls through. The shim hands Parsec FFmpeg's `h264_v4l2m2m` or
   `hevc_v4l2m2m` decoder when it asks for H.264 or HEVC. That takes about 2 ms
   per 1080p frame, versus about 7 ms with software decoding under emulation.
5. **Error -10 ("couldn't find a compatible video decoder").** Parsec opens
   the decoder before it knows the video size, and the Iris driver rejects
   0×0 buffers. The shim opens it at 3840×2160, and the driver reconfigures to
   the real size on the first keyframe. If the hardware decoder still can't be
   opened, the shim falls back to FFmpeg's software decoder.
6. **Error -14 ("failed to decode the video stream").** Around keyframes, the
   Iris driver signals a source change, and FFmpeg's V4L2 code sometimes
   mistakes the end-of-sequence buffer for end of stream (`AVERROR_EOF`). The
   shim catches that, opens a fresh decoder, and replays the packets since the
   last keyframe into it, so the stream continues without a freeze.
7. **Hundreds of milliseconds of lag.** While a frame is still being decoded,
   FFmpeg's V4L2 decoder returns `EAGAIN` rather than waiting, and Parsec moves
   on. Frames then come out late, and the backlog settles at **6 frames** in
   practice. At desktop frame rates (often 10–30 fps) that's 200–600 ms. When
   a frame is owed, the shim waits for it (usually about 2 ms, capped at
   20 ms). Measured on a real Parsec H.265 stream, the lag from packet to frame
   dropped from one or more frame intervals to about 1.7 ms at any frame rate.

## Troubleshooting

Parsec's own log is `~/.parsec/log.txt`, and the client's stdout/stderr goes
to `~/.parsec/stderr.txt`. Put these in the Steam shortcut's launch options
(before `%command%`) to change behavior or add logging:

| Launch option | Effect |
|---|---|
| `PARSEC_HWDEC=0 %command%` | Use FFmpeg's software decoders instead of the hardware one |
| `PARSEC_AVLOG=1 %command%` | Log per-second decode stats: frames `held` (should stay at 0 or 1), waits for owed frames and timeouts |
| `PARSEC_DUMP=/path/stream.bin %command%` | Record the raw compressed video stream, for offline testing |

`tools/decode_latency.c` replays a recorded or test stream through the shim
with Parsec's call pattern, and reports frames held and packet-to-frame lag.
Build and run instructions are at the top of the file.

Known issues:

- The FEX GL/Vulkan thunks don't activate with Valve's rootfs layout, so
  Parsec's rendering goes through emulated x86 Mesa. That probably accounts
  for most of the latency that's left.
- `mangoapp` crash messages in the journal are unrelated to Parsec.
- If Parsec updates to a build that needs a newer FFmpeg ABI, or SteamOS
  updates its FEX rootfs's glibc, re-run `./install.sh`, and possibly bump
  `ARCH_SNAPSHOT` or `FFMPEG_URL`. Both can be overridden from the
  environment.

## License

MIT for the code in this repository. Parsec is proprietary software by
Parsec/Unity, and is downloaded from its official site. FFmpeg is LGPL
(BtbN's LGPL build), and the Arch packages are under their own licenses.
Neither is included in this repository.
