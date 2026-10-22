# Steam Link driver (vrlink) notes

These notes describe how SteamVR's Steam Link driver (`driver_vrlink.dll`, SteamVR 2.x) behaves when it runs under
Wine with this encoder. They come from its logs and from disassembly, for interoperability only.

## Getting vrlink to stream

- **HEVC gate.** Steam only offers HEVC streaming to SteamVR when the D3D adapter reports NVIDIA's vendor ID
  (`0x10DE`). `config/dxmt.conf` makes DXMT report it for `steam.exe` and `vrserver.exe` only.
- **NVENC.** vrlink loads `nvEncodeAPI64.dll` from Steam's directory and SteamVR's `bin\win64`. It drives NVENC in
  async mode: `EncodePicture` queues, a completion event signals the output, `LockBitstream` reads it. The shim keeps a
  worker thread that encodes in submission order and sets the event the way the hardware encoder would.
- **Input.** vrlink renders its encoder input itself: four 1216x1216-ish views stacked vertically (a sharp foveal inset
  and a low-resolution full view per eye), as NV12 with `driver_vrlink.10bit = false`. It writes the luma and chroma
  planes through plane render-target views (DXMT patch 0002). The shim copies the texture to a staging texture, maps it,
  and hands the planes to the helper through `C:\macquestlink\frames.bin` (a file both sides map).
- **Slices.** vrlink asks for 4 slices (`sliceMode` 3, one per band) with `constrainedFrame`; the helper sets
  VideoToolbox's slice count and the shim reports each slice's offset.
- **Rate control.** vrlink retunes the bitrate almost every frame, often by less than 1%. Every VideoToolbox bitrate
  change restarts its rate control and the next frame overshoots, so the helper applies cuts below 85% after they hold
  for 40 ms and otherwise follows a ~0.25 s average at most once a second. The helper uses constant bit rate with a
  one-frame VBV and drops CBR filler NAL units (they only cost air time). Timestamps come from the host clock;
  vrlink's `inputTimeStamp` is not in real time units.
- **IDR.** vrlink requests an IDR at session start only; it never calls `InvalidateRefFrames`.

## Wine gaps worked around in the shim (vrserver.exe)

- `NotifyRouteChange` is not implemented in Wine. vrlink's network check on vrserver's main thread then re-arms it on
  every call (a new, leaked event, plus `GetAdaptersAddresses` and `GetIpNetTable` twice: ~14 ms of round trips to
  Wine's nsiproxy). The shim reports the notification as pending so the check becomes a no-op.
- Audio (`nvenc_shim/audio_hook.c`): Wine's `IConnector::GetDeviceIdConnectedTo` and
  `IAudioEndpointVolume::QueryHardwareSupport` return `E_NOTIMPL`, which made SteamVR skip every endpoint and vrlink
  fail; Wine has no WASAPI loopback capture (redirected to the capture side of a loopback device) and no
  `IPolicyConfig` (supplied, so SteamVR can switch Wine's default devices to the Steam Streaming ones and back).

## "Bad link" events

vrlink's bitrate controller runs once per streamed frame. The "ping" in its log is the headset's own round-trip
estimate (sent back in its stats), not a host measurement. A tick counts as bad when

    ping > clamp(5 - frames in flight, 1, 4) x (1000 / display Hz)

where frames in flight = frames sent but not yet acknowledged + the send queue. At 72 Hz with 2 frames in flight that
is 41.7 ms, with 3 it is 27.8 ms. Each bad tick cuts the rate by 2.5% x (1 - budget / ping); the
`Bad link event` line is logged when a bad stretch ends (`d` = its length, `ping` = its maximum). Good ticks move the
rate toward the link estimate slowly (the ~90 s ramp after connecting).

What makes the ping high on a Mac:
- vrlink's receive thread is also its sender: it sends each video frame as ~400-500 UDP packets of ~1 KB and reads
  nothing meanwhile, so the headset's acks wait in the socket.
- Under Wine every `sendto` is a wineserver round trip (~40 us of CPU), and unconnected UDP sends on macOS look up the
  route per packet. At ~280 Mbit/s a frame's send burst took most of the 13.9 ms frame period.
- Wake-ups of the receive thread go through wineserver too (1-10 ms on some reads).
- Host stalls (frame path spikes) and Wi-Fi (Mac on Wi-Fi: ping 30-50 ms and frequent events; wired Mac: rare).

`driver_vrlink.txt` in Steam's `logs` folder has the events; `scripts/measure.sh` counts them per minute.
