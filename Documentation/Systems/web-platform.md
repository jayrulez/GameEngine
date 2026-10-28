# Web platform - Emscripten + WebGPU

> Status: CURRENT
> Verified: 2026-08-12 @ 12114827
> Track: [[web-platform-track]]

The engine runs in the browser on WebGPU: a WebGPU RHI backend, an Emscripten web runner, single-
threaded execution, and cook-time WGSL. Shipped - a full 3D scene + web player + AngelScript-on-web run
in-browser. Backend and clip-space fixes: [[web-render-fixes-and-flip]].

## The approach that shipped

- **Native-first bring-up via wgpu-native.** The WebGPU RHI backend (`foundation.rhi.webgpu`,
  `Code/Foundation/RHI.WebGPU`) is written against the standard `webgpu.h` C header and validated ON
  DESKTOP by loading wgpu-native (a prebuilt shared library, the DXC runtime-sidecar pattern - see
  [[dxc-runtime-sidecar]]), giving a full gdb/RenderDoc dev loop; it doubles as a bonus desktop backend.
  The Emscripten build is a relink against the browser's WebGPU, not a port. The backend's
  bring-up order was: device/queue/fence lifecycle -> resources -> pipelines/bind groups ->
  encoders + swapchain (first triangle) -> transfer/queries/bundles (full renderer). Every stage
  has shipped; a NotSupported in the backend today is a genuine WebGPU API limit, never a
  missing stage.
- **Single-threaded web v1.** wasm pthreads need SharedArrayBuffer, which needs COOP/COEP host headers
  (rules out plain static hosts); the JobSystem has an inline mode (0 workers, jobs execute at submit).
- **WGSL from the shaders track.** The browser accepts WGSL only (no dlopen, no DXC), supplied by the
  shaders track's cook-time HLSL -> SPIR-V -> naga -> WGSL pack (`shaders.md`).

The engine was web-shaped by construction: `ApplicationHost` / the runners were designed for the
desktop-blocks / web-yields split (`foundation.runtime.web`'s runner is a thin
`emscripten_set_main_loop`, not a refactor); the RHI is the WebGPU-safe subset (the bundle-safe command
subset, WebGPU texture-format conventions, no bind group past set 3 = WebGPU's 4/4 budget on purpose).

## Modules

`foundation.rhi.webgpu` (the backend), `foundation.runtime.web` (the web runner), `foundation.shell.web`
(the web shell), and the Web player entry (`Engine.Player/WebMain.cpp`). Web export ships the wasm + js +
html shell + `Content.pak` + the WGSL `Data/Engine/Shaders/shaders.dpak` (+ `Data/.dataroot`) through the
standard export path (the "Web" export template - see `Documentation/Systems/export.md`); the web
player fetches the `Data/` tree into MEMFS so the one data-root discovery finds it at `/Data`.

## Saves and settings

The user data directory (a game's save, the user's settings) is mounted over the browser's IndexedDB
by the web player before the game starts (`MountUserData` in `WebMain.cpp`, IDBFS, linked with
`-lidbfs.js`), and what the page stored there before is loaded into it. A writer pushes what it wrote
back with `core::PersistUserData()` (a run's save on flush, the player's settings), a background
`FS.syncfs` on the web and a no-op on desktop. A browser that refuses storage (some private windows)
keeps the directory in memory for the page.

## Testing from another device

WebGPU exists only in a secure context (localhost, or HTTPS). `serve.py` (staged beside every web
export) serves the folder; `python3 serve.py 8443 --https` serves it over HTTPS with a self-signed
certificate, so a phone or another computer on the network can open it by address after accepting
the certificate once. Plain HTTP from another machine loads the page without WebGPU.

## Deferred (none block a playing game)

- **wasm pthreads + SharedArrayBuffer** opt-in (hosting-header-gated).
- **Networking on web**: no UDP in the browser - a WebSocket (later WebRTC) transport behind the
  existing `INetTransport` seam; desktop keeps reliable-UDP.
- **Script-debugger remote transport, native file dialogs, Memory64.**
- **`GenerateMipmaps` / `ResolveTexture`** RHI ops (Blit is copy-when-extents/formats-match today).
