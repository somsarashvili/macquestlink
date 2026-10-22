# Keyed mutexes across processes (DXMT patch 0005)

SteamVR moves every frame between processes through shared D3D11 textures guarded by keyed mutexes
(`IDXGIKeyedMutex`): the game submits to vrcompositor, vrcompositor hands the composited frame to vrserver, where the
Steam Link driver (vrlink) converts it to NV12 and calls NVENC. Two problems showed up under Wine + DXMT.

## Acquire timeouts were not honored

Wine's `D3DKMTAcquireKeyedMutex` returns `STATUS_TIMEOUT` immediately while another process holds the key, whatever
timeout the caller asked for. DXMT passed that straight through, so SteamVR's waits failed at once and it skipped or
repeated frames. The patch retries until the caller's deadline. Between attempts it sleeps on the mutex's shared Metal
event (signalled by the holder's release) for up to 2 ms, then yields briefly after a signal, because the CPU-side
release can trail the event. A plain busy loop worked but cost vrserver ~100% of a core.

## Stale frames on the consumer side

DXMT's acquire makes the acquiring *device* wait on the GPU for the releaser's fence. SteamVR often acquires a mutex on
one device only to synchronise (a "sync texture") and then reads the shared textures on another device or thread. On
Windows the driver orders that implicitly; under DXMT the reader could sample a texture whose producer GPU work had
not finished, which showed up as judder ("multiple horizons" when turning: frames up to 6 frames old).

The patch makes a process that *opened* (imported) the mutex wait on the CPU, on acquire, until the releasing device's
GPU work has completed. The process that *created* the mutex (typically the game, which owns its sync texture) keeps
the GPU-only wait and with it its CPU/GPU overlap. `dxgi.keyedMutexWaitOnAcquire = false` in `dxmt.conf` turns the
wait off for a process.

Waiting on the release side instead (each release blocks until the GPU finished) also fixed the stale frames, but in
the game it serialised the render and encode threads and caused bursts of acquire timeouts in the compositor, so it was
dropped.

## Process exit

Metal drops a dying process's in-flight command buffers. A frame that was released just before the process exited
could therefore never reach the GPU, and the consumer waited on a fence that never came. The patch waits (up to 1 s)
for each command queue's committed work in `DLL_PROCESS_DETACH`.

## Tests

- `tools/kmstress.c`: cross-process ping-pong through a keyed-mutex texture; counts stale and half-written reads, with
  flags for SteamVR's sync-texture pattern (bit 4) and a second reader device (bit 6).
- `tools/d3dshare.c`: sharing (NT handle, legacy KMT handle), keyed mutex and NV12 checks with a child process.
