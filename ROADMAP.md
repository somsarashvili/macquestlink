# Roadmap

Open items, roughly by value.

## Streaming
- Measure the baseline in the headset (stream fps, frame path, Bad link per minute, ping, input lag) on M4 Max and M4.
- vrlink's send path: under Wine every UDP `sendto` is a wineserver round trip, and vrlink sends a frame as ~500
  packets from the thread that also reads the headset's acks. Sending directly on the socket's unix fd, connecting the
  socket to the headset, and waking the receive thread without wineserver were each measured to cut CPU and ping in
  development builds; candidates for the baseline once measured on their own.
- Oversized frames after bitrate changes: ignore changes below ~2% and coalesce changes within ~10 ms.
- Slow bitrate ramp after connecting (~90 s): check whether vrlink's starting estimate can be raised.
- Zero-copy encode (DXMT renders vrlink's NV12 target into memory the helper wraps as an IOSurface): saves ~0.3 ms
  per frame but cost more vrserver CPU in a first version; needs an event-based GPU wait.
- Persistent corruption until the next keyframe was seen once; VideoToolbox cannot constrain motion vectors to a slice.

## Rendering / CPU
- Source 2's frame timer calls `QueryPerformanceCounter` ~600k times/s, a Wine syscall each; serving it from the TSC
  shortened Alyx's render-thread recording by ~15% in a test.
- GPU-bound Macs (M4 Air): Alyx's main pass at 4x MSAA dominates; options are `vr_msaa 2` and limiting the render area
  to the viewport.

## Audio
- Choppy audio: check capture buffer size, sample format/rate and clock drift between the loopback aggregate and the
  capture.
- Microphone: verify speech in a game.

## Integration
- Games installed in other Wine prefixes (e.g. one prefix per game): shared textures and keyed mutexes only resolve
  within one wineserver, so the game has to run in the SteamVR bottle.
- Drop the NV12 and swapchain patches when DXMT upstream covers them (DXMT issues #229, #141).

## Stability
- SteamVR's watchdog once aborted when Alyx quit from its menu (not seen since).
- `vrwebhelper.exe` crashes now and then.
