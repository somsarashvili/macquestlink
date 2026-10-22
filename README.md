# macquestlink

Play Windows SteamVR games on an Apple Silicon Mac and stream them to a Meta Quest over Wi-Fi with Valve's Steam Link,
using CrossOver Preview, a patched [DXMT](https://github.com/3Shain/dxmt) (Direct3D 11 on Metal) and an NVENC stand-in
that encodes with Apple's VideoToolbox. Developed and tested with Half-Life: Alyx on a Quest 3S.

This is an experiment, not a product. It works on the setup described below; expect to read logs.

## Status

What works (Half-Life: Alyx, Quest 3S, 72 Hz):
- Alyx plays through, including the intro and video scenes, the SteamVR dashboard, overlays and loading screens.
- MacBook Pro M4 Max, Mac wired to the router: ~70-72 fps at SteamVR's 100% resolution with Alyx's own auto-fidelity,
  stream 1216x4864 HEVC (vrlink's foveated layout), encode ~7-8 ms, render-to-encoder path ~9-10 ms of the 13.9 ms
  frame, bitrate ramping to ~150-280 Mbit/s.
- MacBook Air M4 (10-core GPU): playable but GPU-bound: ~24 fps at SteamVR 100%, ~36 fps at 50%.
- Game audio is streamed to the headset (choppy at times, see Known issues). The Quest's microphone reaches the bottle
  but has not been tried in a game.

These numbers come from development builds that also had some optional CPU/network optimisations. This repository is
the minimal baseline (only what is needed to run and play); it builds and passes the headless checks
(`scripts/nulltest.sh`), and its own headset numbers are still to be measured.

Not supported: D3D12 games (DXMT is D3D11 only), Wine's ARM64/FEX bottles for play (they run, at about half the frame
rate), native macOS Steam games via separate CrossOver prefixes (SteamVR and the game must share one bottle).

## How it works

```
Half-Life: Alyx (hlvr.exe, D3D11)                         Windows processes in one CrossOver bottle (x86_64 Wine,
   | shared textures + keyed mutexes                      running under Rosetta 2)
   v
vrcompositor.exe (SteamVR compositor, D3D11)               all D3D11 goes through DXMT -> Metal
   | shared textures + keyed mutexes
   v
vrserver.exe -> driver_vrlink.dll (Steam Link driver)      renders the stream frame as NV12, calls NVENC
   | NVENC API
   v
nvEncodeAPI64.dll (nvenc_shim/)                            copies the frame to C:\macquestlink\frames.bin
   | localhost TCP (requests) + shared file (pixels)
   v
macquestlink-encoder (encoder/, native macOS)              VideoToolbox HEVC encode, returns the bitstream
   |
   v
vrlink -> UDP -> Steam Link app on the Quest
```

Pieces in this repository:
- `patches/dxmt/`: DXMT changes SteamVR needs: NV12 textures and plane render targets, cross-process shared buffers,
  MSAA clamp to what Apple GPUs support, cross-process keyed-mutex fixes (`docs/keyed-mutex.md`), overlay/dashboard
  fixes (`docs/steamvr-menu.md`), timestamp queries and D3D10-interface fixes so SteamVR measures the game's GPU time
  (Alyx's auto-fidelity depends on it).
- `nvenc_shim/`: a fake `nvEncodeAPI64.dll` for vrlink, plus fixes for Wine gaps in vrserver (audio endpoints, route
  change notification; `docs/vrlink.md`).
- `encoder/`: the native VideoToolbox helper. The shim starts it if it is not running.
- `wts_shim/`: `wtsapi32.dll` for SteamVR. Wine fails the session queries vrcompositor uses to check whether the
  workstation is locked, so the compositor treated the headset as display-locked and overwrote the game's poses.
- `config/`: `dxmt.conf` (reports an NVIDIA vendor ID to Steam and vrserver; Steam only offers HEVC streaming then) and
  the SteamVR settings that matter.
- `scripts/`, `tools/`: setup, daily use and diagnostics.

## Requirements

- Apple Silicon Mac (tested: M4 Max MacBook Pro, M4 MacBook Air), Rosetta 2, macOS with Xcode Command Line Tools.
- [CrossOver Preview](https://www.codeweavers.com/) (commercial; Preview builds, tested with 27.0.0.41073). The
  regular CrossOver release may work but was not tested.
- Steam (Windows) with SteamVR and the game, installed inside the bottle; a Steam account that owns the game.
- Meta Quest 3S (other Quest models with the Steam Link app should behave the same) with the Steam Link app, developer
  mode enabled (for the adb-driven scripts).
- Network: 5 GHz or 6 GHz Wi-Fi for the Quest; the Mac wired to the router is strongly recommended (Mac on Wi-Fi gave
  30-50 ms ping and constant bitrate cuts).
- Homebrew: `brew install mingw-w64 meson ninja cmake git python android-platform-tools`.
- For game audio in the headset: two loopback audio devices (output fed back to input), e.g.
  [BlackHole](https://github.com/ExistentialAudio/BlackHole) 2ch and 16ch.

## Download a release

![build](https://github.com/somsarashvili/macquestlink/actions/workflows/build.yml/badge.svg)

CI (`.github/workflows/build.yml`) runs the Build steps below on a macOS arm64 runner. Each `v*` tag gets a GitHub
release with `macquestlink-<version>.tar.gz`; other runs keep it as a workflow artifact. It unpacks to the repository
layout with the binaries in `build/`: shim, `wtsapi32.dll`, helper, the patched DXMT DLLs in `build/dxmt/`, and the
tools in `build/tools/`. With it, skip Build; in Setup step 2 copy `build/dxmt/{d3d11,dxgi,d3d10core}.dll` instead,
and `scripts/build.sh install` installs the prebuilt files. If macOS blocks the downloaded helper, run
`xattr -dr com.apple.quarantine build`.

## Build

```sh
scripts/fetch-deps.sh              # DXMT, nv-codec-headers, OpenVR headers (pinned) + toolchains (LLVM build takes a while)
scripts/dxmt-patch.sh build        # patch DXMT and build it -> third_party/dxmt/build/src/{d3d11,dxgi,d3d10}/*.dll
scripts/build.sh                   # shim, wtsapi32 shim, helper -> build/
scripts/build.sh tools             # optional diagnostic tools -> build/tools/ (with the OpenVR SDK's openvr_api.dll)
scripts/package.sh [version]       # optional: release archive -> dist/macquestlink-<version>.tar.gz
```

An LLVM 15.0.7 / Wine tree from an earlier build can be reused with `MQL_LLVM_DIR` / `MQL_WINE_DIR`.

Windows tools run in the bottle, e.g.
`CX_BOTTLE_PATH=~/CXPBottles "$CX/wine" --bottle SteamX64 build/tools/vrstats.exe` (see Setup for `CX`).

## Setup

The scripts assume the bottle `~/CXPBottles/SteamX64` and CrossOver Preview in `/Applications`. Override with
`BOTTLE` (name or absolute path), `BOTTLE_PATH` and `CROSSOVER`, in the environment or in `local.env` in the repo root
(sourced by the scripts, not tracked). `QUEST` (the headset's adb address) has no default.

```sh
cat > local.env <<'EOF'
QUEST=192.168.1.50:5555
EOF
```

Below, `CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin"` and
`B=~/CXPBottles/SteamX64`.

1. **Create the bottle** (x86_64 Wine; the whole process runs under Rosetta):
   ```sh
   CX_BOTTLE_PATH=~/CXPBottles "$CX/cxbottle" --bottle SteamX64 --create --template win10_64 --winearch win64
   ```
   In `$B/cxbottle.conf`, the `[Bottle]` section must have `"WineArch" = "win64"` and `"Preview" = "1"` (CrossOver Preview only runs bottles
   marked for it). Add to `[EnvironmentVariables]`:
   ```
   "CX_GRAPHICS_BACKEND" = "dxmt"
   "DXMT_CONFIG_FILE" = "C:/macquestlink/dxmt.conf"
   "DXMT_LOG_PATH" = "C:/macquestlink/dxmtlogs"
   "DXMT_LOG_LEVEL" = "warn"
   "WINEMSYNC" = "1"
   ```
2. **Use the patched DXMT.** CrossOver's `wine` script drops `WINEDLLOVERRIDES`, so set the overrides in the registry:
   ```sh
   for dll in d3d11 dxgi d3d10core; do
       CX_BOTTLE_PATH=~/CXPBottles "$CX/wine" --bottle SteamX64 reg add 'HKCU\Software\Wine\DllOverrides' \
           /v $dll /d native,builtin /f
   done
   ```
   Back up and replace the three DLLs in the bottle's 64-bit system directory (`winemetal` stays CrossOver's own):
   ```sh
   mkdir -p build/system32-backup && cp $B/drive_c/windows/system32/{d3d11,dxgi,d3d10core}.dll build/system32-backup/
   D=third_party/dxmt/build/src
   for f in $D/d3d11/d3d11.dll $D/dxgi/dxgi.dll $D/d3d10/d3d10core.dll; do
       cp $f $B/drive_c/windows/system32/${f:t}.new && mv $B/drive_c/windows/system32/${f:t}.new $B/drive_c/windows/system32/${f:t}
   done
   ```
3. **Install Steam** in the bottle (CrossOver's "Install a Windows Application" with Steam, or run `SteamSetup.exe`
   with `"$CX/wine" --bottle SteamX64`), sign in with "Remember me", install SteamVR and the game. Don't connect the
   headset while Steam is downloading; that froze Steam.
4. **Install macquestlink** (Steam and SteamVR must not be running):
   ```sh
   mkdir -p $B/drive_c/macquestlink && cp config/dxmt.conf $B/drive_c/macquestlink/
   scripts/build.sh install           # shim into Steam\ and SteamVR\bin\win64, wtsapi32.dll, helper into C:\macquestlink
   ```
   Always install the shim and the helper together (they share a protocol).
5. **SteamVR settings.** With SteamVR closed, merge `config/steamvr.vrsettings` into
   `$B/drive_c/Program Files (x86)/Steam/config/steamvr.vrsettings`: 72 Hz (`preferredRefreshRate`), NV12 stream
   (`driver_vrlink.10bit = false`, required), no motion smoothing or interleaved reprojection (DXMT has no async
   reprojection), resolution pinned at 100% (`supersampleManualOverride` + `supersampleScale`; otherwise SteamVR picks
   a scale from its GPU benchmark), no SteamVR Home, no desktop view, vrlink's debug graph window off.
6. **Steam Client Service.** When SteamVR starts, Steam asks its 32-bit service to raise vrcompositor's GPU priority;
   the service calls a D3DKMT function Wine lacks, crashes into a hanging debugger, and Steam waits forever.
   `scripts/vrreset.sh` disables the service (`Start = 4`) on every run; nothing else needs it.
7. **Audio** (optional): with your first loopback device as the Mac's default output, run
   `build/tools/mkaudiodev out create "Steam Streaming Speakers"`; with the second as the default input, run
   `build/tools/mkaudiodev in create "Steam Streaming Microphone"`; then set your normal defaults back. (A sub-device
   UID can be passed as a third argument instead.) `mkaudiodev out` / `mkaudiodev in` remove them. vrlink looks for
   these names; the shim switches Wine's default devices to them while SteamVR runs.
8. **Quest.** Enable developer mode, connect by USB once and run `adb tcpip 5555`, then put
   `QUEST=<quest ip>:5555` in `local.env`. Install Steam Link on the Quest, start it once by hand and pair it with the
   bottle's Steam (enter the code shown in the headset).

## Daily use

1. Quit native macOS Steam (Steam Link otherwise finds two hosts / port 27036 clashes). If the Mac is wired, turn its
   Wi-Fi off when both are on the same subnet (vrlink may pick the Wi-Fi address). Don't use macOS Screen Sharing
   while playing (it halves the frame rate).
2. `scripts/vrreset.sh 546560`: stops everything in the bottle, starts the helper and Steam, waits for the logon,
   starts Steam Link on the Quest and connects, then launches the game (546560 = Half-Life: Alyx) and brings its
   window to the front once.
3. `scripts/vrrestart.sh`: restarts only SteamVR and reconnects (after a settings or driver change).
4. After any crash, run `scripts/vrreset.sh` again rather than restarting parts.

Helpers:
- `scripts/alyxsave.sh [save]`: (re)starts Alyx with VConsole, loads a save (default `s6/steamvr_quit`, the autosave
  Alyx writes when SteamVR quits), closes the SteamVR dashboard and unpauses.
- `python3 tools/dashboard.py`: text dashboard at `http://<mac>.local:8765` (stream fps, bitrate, frame path, Bad
  link/min, CPU); it also writes `C:\macquestlink\stats.txt`, which `tools/vrstats.exe` shows as a small panel in the
  headset (`C:\macquestlink\nostats` hides it).
- `scripts/measure.sh [seconds]`: one-line measurement of a running session.
- `scripts/nulltest.sh start [appid]` / `stop`: headless smoke test with SteamVR's null driver (no headset).
- `scripts/displayguard.sh`: kills the bottle when WindowServer reports a stuck display, before its watchdog logs you
  out (seen once).
- `scripts/vrstatus.sh`, `scripts/fpsrecord.sh`, `scripts/scrcpy-loop.sh` (mirror the Quest with scrcpy).

## Files and switches in `C:\macquestlink`

| File | Meaning |
|---|---|
| `dxmt.conf` | DXMT per-process options (vendor ID; `dxgi.keyedMutexWaitOnAcquire = false` turns off the consumer-side keyed-mutex wait) |
| `nvenc_shim.log` | shim log (session setup, a timing summary every 450 frames: queue, GPU wait, copy + encode, total path) |
| `encoder.log` | helper log (a summary every 450 frames: fps, encode ms, Mbit/s, keyframes, oversized frames) |
| `audio_hook.log` | audio endpoint fixes in vrserver |
| `dxmtlogs/` | DXMT logs per process (written when the process exits) |
| `frames.bin`, `encoder.session` | shared frame buffer; last session size so the helper can pre-create the encoder |
| `noslices` (create to enable) | encode one slice per frame instead of the 4 vrlink asks for |
| `nostats` (create to enable) | hide the vrstats panel |

`MQL_BITRATE_KBPS` (bottle environment) fixes the bitrate instead of following vrlink.

## Known issues

- **Half frame rate (~36-40 fps)** right after launch: macOS App Nap keeps a never-activated Wine window at low
  priority. The scripts activate the game window once; if you launch by hand, click the game's window on the Mac.
- **Bad link / bitrate cuts**: vrlink's ping budget is tight at 72 Hz; Wi-Fi on the Mac side, roaming to 2.4 GHz on
  the Quest (use a 5 GHz-only SSID or turn off band steering) and host stalls cause them (`docs/vrlink.md`).
- **Slow bitrate ramp**: vrlink takes ~90 s after connecting to reach full bitrate (soft picture until then).
- **Oversized frames** after bitrate changes (VideoToolbox overshoots on the first frame after a reconfigure).
- **Choppy audio** at times (Steam Link's jitter buffer underflows). After a forced kill of vrserver the bottle's
  default output stays on "Steam Streaming Speakers" (silent on the Mac) until SteamVR runs and exits normally.
- **Shimmer** on fine geometry: Apple GPUs top out at 4x MSAA (Alyx asks for 8x).
- **SteamVR opens its dashboard on start**, and Steam opens the game's overlay in it (the game pauses). Close it with
  the menu button (`alyxsave.sh` does it automatically).
- **Occasional crashes** of `vrwebhelper.exe` (no visible effect) and, once, a WindowServer watchdog logout
  (`scripts/displayguard.sh`).
- With the null driver, SteamVR's first-run Room Setup can take the scene-app slot; `nulltest.sh` closes it.
- The Quest needs a lit room and must stay inside its boundary, or tracking stops and the stream stalls.

## Performance notes

- The game runs under Rosetta 2 inside x86_64 Wine; an ARM64 (FEX) bottle ran Alyx at about half the frame rate.
- At 72 Hz without async reprojection the frame rate snaps to 72/36/24; frame time just over 13.9 ms halves it. A
  120 Hz refresh gives 60/40 fps steps instead.
- On the M4 Max the remaining per-frame cost is CPU (~4 ms of an ~11 ms frame feeding the GPU through Rosetta, Wine and
  DXMT); on the M4 Air it is the GPU (the main 4x MSAA pass alone takes ~7 ms).
- See `ROADMAP.md` for open work.

## License

macquestlink's own code is MIT licensed (`LICENSE`). The DXMT patches in `patches/dxmt/` are derivative works of DXMT
and are LGPL-2.1-or-later (`patches/dxmt/LICENSE`). Dependencies, and what is not redistributed (CrossOver, Steam,
SteamVR, the game), are listed in `THIRD_PARTY.md`.

Not affiliated with Valve, Meta, CodeWeavers or the DXMT project. Steam, SteamVR and Half-Life are trademarks of Valve
Corporation; Meta Quest is a trademark of Meta Platforms; CrossOver is a trademark of CodeWeavers.
