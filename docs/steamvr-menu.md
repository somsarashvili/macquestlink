# SteamVR dashboard and overlays

The SteamVR dashboard (menu button), the app-launch interstitials (`steam.app.<appid>_interstitials`, e.g. Alyx's
loading/"press trigger" screens) and every other SteamVR web overlay are drawn by `vrwebhelper.exe` (CEF 126) and
handed to the compositor with `IVROverlay::SetOverlayTexture`. Under DXMT none of them rendered. Three DXMT problems
stacked up; all are fixed in the patch series (`patches/dxmt/0006`-`0008`).

## 1. D3D11 overlay textures showed uninitialized memory (0006)

`SetOverlayTexture` with a D3D11 texture: vrclient (in the overlay process) creates its own `R8G8B8A8_UNORM_SRGB`
texture with `D3D11_RESOURCE_MISC_SHARED` (no keyed mutex), `CopySubresourceRegion`s the app texture into it and sends
the legacy handle to vrcompositor. It never calls `Flush`. DXMT only submits on Flush/Present/Map, so the copy never
reached the GPU and the compositor sampled an uninitialized texture (magenta, a flat grey panel in the luma dump).
`SetOverlayRaw` worked, which pointed at the D3D path.

Fix: copies (`CopyResource`/`CopySubresourceRegion`) into a shared texture without keyed mutex commit right away
(`CommitIfShared`). Copies into keyed-mutex textures are unchanged (their release already syncs).

## 2. CEF's GPU process could not use its shared textures (0007)

vrwebhelper renders offscreen. With GPU compositing, Chromium's viz captures each frame into a D3D11 shared texture
(B8G8R8A8, `SHARED|SHARED_NTHANDLE`) and calls `OnAcceleratedPaint`; vrwebhelper opens the NT handle and calls
`SetOverlayTexture` (DirectX). Inside the GPU process ANGLE wraps that texture with `EGL_D3D11_TEXTURE_ANGLE`, and
`ExternalImageSiblingImpl11` only marks it texturable/renderable when `IDXGIResource::GetUsage` reports
`DXGI_USAGE_SHADER_INPUT`/`RENDER_TARGET_OUTPUT`. DXMT returned 0, so `glEGLImageTargetTexture2DOES` failed ("EGL
image internal format is not supported as a texture"), Skia logged "Unable to initialize SkSurface" for every frame and
no frame ever reached vrwebhelper (no `SetOverlayTexture` at all).

Fix: `GetUsage` derives the usage from bind flags and CPU access.

## 3. CEF's GPU process fell back to software (0008)

The GPU process creates a swapchain for a browser-process window; DXMT refused cross-process swapchains (DXMT issue
#141), Chromium disabled GPU compositing and CEF used `OnPaint`. vrwebhelper's `OnPaint` path uploads into an OpenGL
texture and submits `TextureType_OpenGL`, which vrclient shares through `WGL_NV_DX_interop` + `glCopyImageSubData`
(not available in Wine on macOS): "wglGetCurrentContext() returned NULL" in `vrclient_vrwebhelper_main.txt` and
`VROverlayError_InvalidTexture` in `vrwebhelper_main.txt`.

Fix: render such swapchains into a hidden local window (ported from an earlier experimental branch). Without
it the dashboard stays invisible even with 0006 and 0007.

`steamvr.forceCEFMode` ("cpu"/"gpu", read by vrclient and passed as `-forceOnPaint`) is no way out: "cpu" makes
vrwebhelper create an SDL OpenGL context, which fails in Wine ("Could not create GL context: Invalid parameter"), and
vrwebhelper exits. Leave it unset.

## Tools

- `vroverlay.exe panel <d3d|d3dclear|raw|file> <s>`: head-locked test panel 1 m in front of the HMD.
  `vroverlay.exe panel dash <s>` opens the dashboard (`IVROverlay::ShowDashboard`) and logs `IsDashboardVisible`.
- `sharefill.exe [fmt] [size] [hold]`: fills shared textures by clear/initial data/copy/update and reads them back in
  a child process.
- `angletex.exe [fmt]`: what Chromium does with a D3D11 texture through the CEF dir's ANGLE (EGL image, FBO, clear,
  read back), with GL debug messages.
- `cefclient.exe` in the vrwebhelper dir reproduces the CEF side without SteamVR:
  `--off-screen-rendering-enabled --shared-texture-enabled --no-sandbox --enable-logging --log-file=C:\...`;
  `--remote-debugging-port=N` plus CDP gives `chrome://gpu` as text.
- `vrsetting.exe <section> <key> s:<string>` sets string settings.

## Verified

- A D3D11 head-locked panel shows its checkerboard in the encoder input (it was flat grey before).
- `ShowDashboard` shows the full dashboard (clock, library, bottom bar) in the stream.
- The Alyx launch interstitial and "LOADING..." screens are visible in the headset.

## Open

- SteamVR auto-opens the dashboard on start ("InitialOverlayToAutoShow", quicklaunch); before the fix it was open but
  invisible. Close it with the menu button.
- Steam summons Alyx's game overlay into the dashboard after launch and Alyx pauses while it is open;
  `scripts/alyxsave.sh` closes it through SteamVR's web mailbox (`tools/vrmailbox.py`).
