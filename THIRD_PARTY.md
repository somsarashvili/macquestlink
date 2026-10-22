# Third-party software

macquestlink's own code (everything outside `patches/dxmt/`) is MIT licensed (see `LICENSE`). It does not include
third-party source code; the dependencies below are fetched at build time by `scripts/fetch-deps.sh` (pinned versions)
or must be installed separately. No Valve, Meta or CodeWeavers binaries are part of this repository.

| Component | Used for | License | How |
|---|---|---|---|
| [DXMT](https://github.com/3Shain/dxmt) @ `50102f9` | D3D11/DXGI on Metal, patched by `patches/dxmt/` | LGPL-2.1-or-later | fetched, built locally |
| DXMT submodules (NVIDIA nvapi headers, mingw-directx-headers) | DXMT build | see their repositories | fetched with DXMT |
| [3Shain Wine 8.16 build](https://github.com/3Shain/wine/releases/tag/v8.16-3shain) | DXMT's Wine import libraries/headers (build only) | LGPL-2.1-or-later | downloaded |
| [LLVM](https://github.com/llvm/llvm-project) 15.0.7 | DXMT's shader compiler (build only) | Apache-2.0 WITH LLVM-exception | built locally |
| [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) @ `eddcea9` | NVENC API declarations for the shim | MIT | fetched, headers only |
| [OpenVR SDK](https://github.com/ValveSoftware/openvr) v2.15.6 (`0924064`) | `openvr_capi.h` for the diagnostic tools | BSD-3-Clause | fetched, headers only |
| mingw-w64 (GCC), Meson, Ninja, CMake | toolchain | GPL/LGPL/MIT/BSD (tools, not linked except the GCC runtime exception) | Homebrew |

The patched DXMT DLLs you build are covered by DXMT's LGPL-2.1-or-later. If you distribute them, distribute the
corresponding source (DXMT at the pinned commit plus `patches/dxmt/`) under the same license.

Release archives (`scripts/package.sh`, attached to GitHub releases by CI) contain binaries built from the above:
the patched DXMT DLLs (`build/dxmt/`, LGPL-2.1-or-later; the corresponding source is DXMT at the pinned commit plus
`patches/dxmt/`, which the archive includes with the license), the shim and helper (MIT, the shim statically links
libgcc under the GCC runtime library exception) and `build/tools/openvr_api.dll` from the OpenVR SDK (BSD-3-Clause,
license in `build/tools/openvr_api.LICENSE`).

Runtime requirements that are not open source and are not redistributed here:
- **CrossOver Preview** (CodeWeavers, commercial) to run Windows Steam/SteamVR on macOS.
- **Steam, SteamVR, Steam Link** and **Half-Life: Alyx** (Valve), installed by you from Steam.
- **Meta Quest** system software on the headset.

`nvenc_shim/` implements the NVENC API (as declared by nv-codec-headers) so that SteamVR's Steam Link driver can use
Apple's VideoToolbox. The notes about the Steam Link driver's (driver_vrlink.dll) behaviour in `docs/` come from
observing and disassembling it for interoperability with this encoder; no Valve code is included.
