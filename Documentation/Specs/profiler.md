# Profiler - CPU and GPU on one timeline, in the editor and in a capture file

> STATUS: BUILT 2026-10-13, P0 to P5: the capture store, GPU timings and statistics, the RHI's
> counters, the stream with capture files and `profiler_capture`, `Profiler.UI` with the overlay,
> and the editor's Profiler tab. Decisions taken and revised the same day (below). Since
> 2026-10-28 the overlay and the editor's views are Profiler.GUI's; `Profiler.UI` is deleted. Backlog entry it closes:
> `Plans/backlog-0.1.md` (Seeded: GPU profiler + profiler visualization). Read CONVENTIONS.md first.

## What it is for

The user asked (2026-08-26) for two things: GPU timings past what the render graph prints today,
and a visual profiler that puts the CPU's scopes and the GPU's passes on ONE per-frame timeline
instead of two text dumps. It is a 0.1 item: a user making a game needs to see where a frame goes,
and agents driving the engine over MCP need the same numbers.

## Where things stand (read in the code, 2026-10-13)

The backlog item is out of date in three places: the CPU profiler is `foundation.profiler`
(not `draconic.profiler`), GPU timestamps work on DX12 and native WebGPU as well as Vulkan, and
the `profiler-system` and `imgui-integration` docs it links do not exist.

**CPU: `foundation.profiler`** (`Code/Foundation/Profiler/Profiler.cppm`, macros in `Profiler.h`).
- `PROFILE_SCOPE(name)` (RAII), `PROFILE_FRAME_BEGIN/END`, compiled in where `BUILD_PROFILING`
  is 1: Debug and RelWithDebInfo (`CMakeLists.txt:220`); out in Release and Shipping. A runtime
  `SetEnabled`, on by default.
- Each thread appends `ProfileSample{name, startTick, durationTicks, depth, threadIndex}` to its
  own list (thread_local, registered under a mutex); `EndFrame` drains every thread into ONE
  completed frame, overwritten each frame. No history beyond a 64-entry ring of frame times, no
  thread names, nothing in the job system.
- Frames are bracketed in `ApplicationHost::Tick` (Update, Render, Acquire, Present, Advance);
  about 40 scopes across render, animation, audio, navigation, particles, physics, script,
  terrain, UI and vegetation.
- Read out only as text: the P key (`DefaultApplicationImpl.cpp:130`) prints `BuildReport()`.

**GPU: `GraphProfiler`** (`Code/Foundation/RenderGraph/GraphProfiler.cppm`).
- One timestamp query set (64 passes x 2) and one readback buffer, no ring per frame in flight;
  a timestamp before each executed pass's barriers and after its last transition.
- Turned on unconditionally (`RenderSubsystemImpl.cpp:977`), in every build, Shipping included.
- Read only on demand, by `WaitIdle()` and a read of the last frame: no frame tag, no history,
  no capability check (`Features.timestampQueries` is never asked).
- The graph also times each pass's CPU recording (`m_passCpu`), printed beside it.

**The RHI** has what a profiler needs: `QueryType::Timestamp` / `PipelineStatistics`,
`WriteTimestamp` on encoders, `ResolveQuerySet`, `Queue::TimestampPeriod()`, and render-pass
boundary timestamps in `RenderPassDesc`. Vulkan: full. DX12: full. WebGPU native: encoder
timestamps when wgpu-native grants `TimestampQueryInsideEncoders`, pass-boundary timestamps
always; in a browser, none (a no-op). Null: stubs. Pipeline statistics pools exist on Vulkan and
DX12; nothing reads them.

**UI**: nothing draws a timeline. The editor is built on `foundation.ui` docking (no ImGui);
`LogView` is the model of a panel fed every frame. ImGui (`extensions.imgui`) is in the samples
only. No MCP tool returns timings.

**Tests**: `Profiler.Tests` (4 cases), one GraphProfiler case (uninitialized is safe). Nothing
tests a timestamp on any backend.

## What other engines do (surveyed 2026-10-13, current code)

| | CPU record | GPU timing | CPU/GPU clock | Viewer | Gate |
|---|---|---|---|---|---|
| **Lumix** | begin/end events into 4 KB pages per thread, a ring of 500 pages (2 MB); fibers, jobs, mutex waits, counters, context switches (Windows) | timestamp per begin/end, nested; 2 frames late; NVML clock and memory counters | once at start, an offset, no drift fix | in-editor ImGui timeline: a lane per thread, a GPU lane, counters, CPU block linked to its GPU block; frozen snapshots; own binary save | always on |
| **ezEngine** | one record per scope at its end, a ring per thread (4 MB main, 1 MB others); scopes under 0.1 ms dropped | nested scopes, frame-tagged pools, read when the frame's fence passed, kept 4 frames | once (a timeline semaphore and a sample), no drift fix | no viewer: writes Chrome trace JSON (frames lane, GPU lanes, thread lanes) for chrome://tracing or Perfetto; Tracy optional | off in Shipping |
| **Traktor** | begin/end events, names interned, flushed 64 at a time to a live listener | per render-graph pass, flat; read when the swapchain image comes back | none: GPU bars placed at the CPU time of the graph build | editor dialog over the editor-to-game connection: perf counters and a lane chart; live only, no files | listener present |
| **LunarSong** | aggregates per name (a call tree for the main thread), 600 frames of history | per render-graph pass, flat; 3 frames late; Vulkan, Metal, WebGPU | none | editor panels: a CPU tree with a frame-time chart and scrubbing, a CPU and GPU per-pass table, counter charts; no timeline; extensive MCP tools (`get_cpu_profiler`, `get_gpu_profiler`, ...) | compiled in, armed by a panel |
| **ours** | per-thread samples, one frame kept | per pass, flat, read by stalling | none | P key text | Debug/RelWithDebInfo (CPU); GPU always |

None of them uses calibrated timestamps (`VK_KHR_calibrated_timestamps`, D3D12
`GetClockCalibration`); the two that align clocks do it once, with no drift correction. Only
Lumix has a real timeline viewer in its editor; ez leans on Chrome/Perfetto, which costs a file
writer and gives a full viewer for free.

## Design

### One capture layer, UI-free

`foundation.profiler` becomes the one store both halves write to, and every viewer reads:

- **Per thread**: a fixed ring of the scopes it finished (name id, depth, begin and end ticks),
  sized per thread (the frame thread 64K, others 16K; `SetRingCapacity`), written without a lock
  by its own thread and read by the frame thread at frame end; a full ring drops new scopes and
  counts them. Names are interned to ids: a literal once per thread (cached by address), a name
  made at run time (a pass's) each call. Threads carry names (`core::SetCurrentThreadName`; the
  host names its frame thread "Main", the job system its workers "Worker N"), and every job a
  job system runs is a "Job" scope on its worker's lane (`core::JobHooks`, which Core calls
  around each job without knowing the profiler).
- **Frames**: `EndFrame` copies what every thread finished into the frame's record; the store
  keeps the last 300 records (about five seconds; `SetHistoryLength`), found by frame number.
- **GPU lane**: GPU passes arrive late (below) and are written into the frame they belong to,
  by frame number (`SubmitGpuPasses`), as begin/end ticks already on the CPU clock.
- **Counters**: registered once by name and unit, set per frame (below), the lanes under the
  timeline.
- **Armed or not**: recording costs a branch when nothing is watching. Arms are counted: a
  viewer (the panel, a capture, an MCP call) arms it and disarms when done; the P key's first
  press arms it for the rest of the run.

### Built to be streamed: remote profiling in 0.2 is a transport, not surgery

The user asked (2026-10-13) that a live connection to a running player, a 0.2 item, need no deep
changes then. So from P0 the store's unit is a **frame record** that means the same thing in
another process:

- A frame record holds its frame number, its CPU begin/end ticks, every thread's events in that
  frame, its GPU passes (already on the CPU clock), its pipeline statistics and its counters,
  with names as ids. It refers to nothing in the process: no pointers, no `StringView`s.
- The tables a record leans on travel beside it as deltas: the name table (an id per string,
  sent when new), the thread table (index, name), the counter table (id, name, unit), and once
  per session a header: application, platform, build, the tick frequency. A viewer never
  compares another process's ticks with its own clock.
- **One encoding** of that stream (`ProfileStream`, a compact binary) is written by the store
  and read by every viewer. A capture file is that stream saved (`.aeprofile`); the Chrome trace
  JSON is an export from it, for Perfetto. In 0.2 a socket carries the same bytes.
- **Viewers read through a source** (`IProfileSource`: the frames it has, the tables, a newest
  frame), with two implementations in 0.1: the live store in this process and a capture file.
  The panel and the overlay never touch the store directly, so 0.2 adds a third, the network.
- **Control is a small message set**: arm, disarm, capture N frames, set the overlay, which the
  local store answers by function call now and a player answers over the wire in 0.2.

That costs 0.1 little (a capture file is the stream either way) and buys a panel that opens a
capture from a Steam Deck or a phone today, the way it will watch one live tomorrow.

### GPU timings: frame-tagged, read without stalling, on the CPU clock

`GraphProfiler` keeps one query set per frame in flight, tagged with the CPU frame number, and
reads a frame's results when that frame's fence has passed (the slot comes round), never by
`WaitIdle`. Each executed pass writes begin/end; render-graph markers inside a pass (a named
scope) may nest. It asks `Features.timestampQueries` and turns itself off where there are none
(a browser), so the GPU lane is simply absent there.

Aligning the clocks: where the device offers calibrated timestamps (Vulkan
`VK_KHR/EXT_calibrated_timestamps`, D3D12 `GetClockCalibration`), sample a CPU/GPU pair every
second or so and map each GPU tick through the latest pair, which also corrects drift; elsewhere
(WebGPU native), a sync point once at arm time (a timestamp written and waited for, the CPU tick
taken then), as ez does. A test checks a frame's GPU passes land inside or after their CPU frame
and in order.

The GPU profiler is compiled under the same gate as the CPU one and armed with it, instead of
running in every build.

### The RHI stays a pure interface

The user's rule (2026-10-13): the RHI is a pure interface. Whatever this adds to it (the counters'
read-out, the driver's memory budget, a calibrated clock pair) is pure virtual, implemented in
every backend: Vulkan, DX12, WebGPU, Null, and the Validation wrapper passing through. A backend
without the capability says so in its `Features` and still implements the method (an empty
answer, an explicit "unsupported"), it does not lean on a default body. The few virtuals that
have one today (`AsMeshShaderExt`, `AsRayTracingExt`, `MaxColorDepthSampleCount`,
`SupportsSampleCount`, `DestroyMeshPipeline`, `DestroyAccelStruct`,
`DestroyRayTracingPipeline`) are not the pattern to follow.

What P1 added, all pure: `Queue::CalibrateTimestamps` (a GPU timestamp and core::GetTicks read
together; Vulkan's calibrated timestamps in the host clock core::GetTicks reads, D3D12's
GetClockCalibration, false on WebGPU and Null) with `Features::timestampCalibration`;
`CommandEncoder::Begin/EndPipelineStatisticsQuery`; and `Device::QueryResultSize(type)` with
`Device::DecodePipelineStatistics`, since a statistics result is a backend's own record (Vulkan
writes its enabled counters in bit order, D3D12 an eleven-counter struct) and Vulkan's resolve had
written every query eight bytes apart. WebGPU's `timestampQueries` now means what
`WriteTimestamp` can do there (wgpu-native's in-encoder feature; never in a browser).

What P2 added, all pure: `Device::Counters()` (a `DeviceCounters` snapshot: totals of draws,
dispatches, render and compute passes, pipeline and bind group binds, bytes through transfer
batches, and the bytes and count of the buffers, textures and render targets held now) and
`Device::MemoryBudget` with `Features::memoryBudget` (VK_EXT_memory_budget's device-local heaps,
DXGI QueryVideoMemoryInfo). Each backend keeps a `DeviceCounterBlock` (atomics; bundles are built
on workers) that its encoders, transfer batches and resource creation bump; a render bundle keeps
`RecordedCounts` and adds them each time it is executed. Vulkan counts its allocations' real
sizes; WebGPU, D3D12 and Null a texture's size from its description (`EstimateTextureBytes`).
`runtime::DeviceCounterPublisher` turns the totals into the frame's counters at the host's frame
end, the budget read once a second.

### Pipeline statistics and counters

The user asked (2026-10-13) for these in 0.1 too.

- **Pipeline statistics** per pass, beside its timestamps, where the device has them (Vulkan,
  DX12; `Features.pipelineStatisticsQueries`): input-assembly vertices and primitives,
  vertex, fragment and compute shader invocations, clipping primitives. A query set per frame in
  flight, read when the frame's fence passes, like the timestamps. Not nested (a statistics query
  cannot be), so per pass only, the frame's totals summed from them. WebGPU has none: the
  columns read "n/a" there.
- **What the RHI counts itself, every backend** (web included): per frame, draws, indexed draws,
  dispatches, render passes, pipeline and bind group changes, bytes uploaded; and live GPU memory
  the RHI allocated, split into buffers, textures and render targets (the backends allocate their
  own memory, one allocation per resource, so the counts are exact).
- **What the driver says**, where it says it: heap budget and usage (Vulkan
  `VK_EXT_memory_budget`, DX12 `IDXGIAdapter3::QueryVideoMemoryInfo`), read once a second.
- All of it lands in the frame record as counters, so every viewer shows it, the stream carries
  it, and the overlay draws it.

### The views: one library, embedded in the game and the editor

The user's shape (2026-10-13): a library of profiler views, `Profiler.UI` (`foundation.profiler.
ui`), on `foundation.ui` and drawn with VG, whose pieces the game's overlay and the editor's
panel each put together. The editor and the game UI share `foundation.ui`, so one view serves
both; the capture store stays UI-free (`Profiler.UI` depends on it, never the reverse). Every view
reads an `IProfileSource`, so each works on the live store, a capture file, and in 0.2 a player
over the network.

The pieces, each a `View` that draws itself with VG (antialiased lines, filled paths, rounded
rects, text) and measures like any other:

| View | What it draws |
|---|---|
| `FrameTimeGraph` | the frame times of the last N frames as a line over a filled area, CPU and GPU, a budget line (16.7 / 33.3 ms); in the panel, click selects a frame and a drag a range |
| `FrameStats` | the current (or selected) frame's CPU and GPU times, fps, min / median / max over the window |
| `TopList` | the top scopes or GPU passes by time over a window (half a second, or the selection), as labelled bars |
| `CounterStrip` | counters as compact values or sparklines: draws, primitives, shader invocations, GPU memory by kind and against the driver's budget |
| `Timeline` | the selected frames: a lane per thread with nested scopes as stacked bars, the GPU lane, frame boundaries; wheel zoom about the pointer, drag pan, hover for name, duration and depth |
| `ScopeTable` | scopes and passes of the selection by total, self and count, sortable; a row highlights its bars in a `Timeline` beside it |
| `ProfilerToolbar` | pause, the frame window, open and save a capture, export JSON (panel only) |

The two assemblies:

- **The in-game overlay** (`ProfilerOverlay`, compact): `FrameTimeGraph`, `FrameStats`, a
  `TopList` for scopes and one for GPU passes, a `CounterStrip`, on a translucent rounded panel in
  a corner. Whether an application has one, and what turns it on, is the application's decision
  (user 2026-10-13): Engine.UI only offers the place to draw, its screen tier's global overlay
  layer (`UISubsystem::PushScreenOverlay` / `RemoveScreenOverlay`), above every scene, surviving
  scene changes. A `ProfilerOverlayHost` (Engine.DefaultApp) shows it there, arms the profiler
  while it shows and refreshes it each frame. The default application owns one: off by default;
  F3 toggles it, the player's `--profile-overlay` and the web player's `?profile-overlay` show it
  from the start, in any build the profiler is compiled into (all but Shipping), exported players
  and the web included. A script asks the app: `run.setProfilerOverlay(shown)` and
  `run.profilerAvailable()`, bound in every build so a script compiles the same in Shipping,
  where nothing shows and the profiler reads unavailable; an app that wires nothing leaves both
  harmless. The editor embeds the default application, so Play in editor gets the same overlay
  without a special case (user 2026-10-13); it shows the whole editor process, as the panel does.
  Two viewers at once (a PIE game's overlay, the editor's panel) leave each other alone: arms are
  counted, and reading the store's history is passive.
- **The editor's Profiler panel** (a tab docked with the Console, in the View menu; user
  2026-10-13: start and stop in its toolbar, the views embedded): `ProfilerToolbar`,
  `FrameTimeGraph` as the frame strip (click a frame, drag a range, Pause freezes), the
  `Timeline` of the selection with a `ScopeTable` beside it, `CounterStrip` lanes under it. It
  shows the editor process as a whole, and says so: every page, preview and Play-in-editor run
  in one frame, never presented as one game's. A game's own numbers come from its player (the
  overlay, or a capture from it opened in the panel). So it never disturbs another viewer: its
  Stop freezes its own recording, never the store; it does not change the store's history length
  while anything watches (that clears the history), copying frames out instead when it wants
  more; and it saves its recording with the same writer as the process's recorder, which a
  capture already running does not block.

A game can embed any piece itself (a debug menu of its own showing `FrameTimeGraph`), since they
are ordinary views.

### Capture files and MCP

- **Capture files**: Save Capture in the panel, a key and a command-line flag in any player
  (`--profile-capture <seconds>`), and the MCP tool write the stream (`.aeprofile`); the panel
  opens one (Open Capture), and Export writes Chrome trace JSON beside it, for Perfetto or
  chrome://tracing (a frames lane, a lane per thread, the GPU lane, counters). So an exported game
  on a Steam Deck or a phone is profiled without the editor: capture there, open here.
  As built (P3), every capture writes both files at once, the JSON beside the stream; the key is
  F9 (five seconds) in any game on the default application, and captures land in
  `<user-data>/profiles` as `<application>-<ticks>.aeprofile` and `.json`.
- **MCP**: `profiler_capture` arms the profiler for N seconds (of the editor or a PIE instance;
  seconds, as built, since a frame count means different lengths in a debug and a release build),
  writes the capture and answers a summary: frame times (min, median, max), the top scopes and
  GPU passes by total time, the counters, and the path. Documented in the McpGuide; the tool
  count bumps.

## Decisions (user 2026-10-13)

1. **Viewers**: the editor's Profiler panel, capture files (our stream, exported to Chrome trace
   JSON), and an in-game overlay, off by default (revised: the overlay first ruled out, then
   asked for), both assembled from one library of views, `Profiler.UI`, drawn with VG.
2. **Profiling an exported game**: capture files for 0.1. A live connection from the editor to a
   running player is in `Plans/backlog-0.2.md`, and 0.1 is built so that it is a transport only
   (the frame record, the stream, the sources, the control messages).
3. **Gate**: compiled into Debug, RelWithDebInfo and Release; out only in Shipping
   (`BUILD_PROFILING`, `CMakeLists.txt:220`), the GPU profiler with it.
4. **Pipeline statistics and GPU memory counters**: in 0.1 (revised from "later").

## Phases

- **P0 capture store**: per-thread event rings, thread names, frame history, frame records with
  their name, thread and counter tables, the armed switch, job-system scopes; the P key reads the
  new store. Tests: nesting across threads, the ring wrapping, a frame selected from history, a
  record holding no pointer into the process, nothing recorded unarmed.
- **P1 GPU**: frame-tagged timestamp and pipeline-statistics query sets per frame in flight,
  read without stalling, capability checks, clock alignment per backend, under the gate. GPU
  probe tests on Vulkan and WebGPU native: passes arrive tagged with their frame, ordered, inside
  the frame's CPU span after alignment; statistics count a known draw's vertices.
- **P2 counters**: the RHI's own counts (draws, dispatches, passes, binds, uploads, live memory
  by kind) on every backend, the driver's budget where offered. Tests: a frame of known work
  counts what it did; memory rises and falls with a texture made and freed.
- **P3 the stream, capture files and MCP**: `ProfileStream` write and read, `.aeprofile`, the
  Chrome export, the player key and flag, `IProfileSource` (live, file), the control messages,
  `profiler_capture`. Tests: a captured stream reads back frame for frame; the JSON holds frames,
  threads, the GPU lane and nesting; the tool answers its summary. As built, the control messages
  are function calls on the store and the recorder (`Arm`, `Disarm`, `CaptureRecorder::Start`);
  their wire form waits for the 0.2 transport that would carry them. The editor host alone serves
  the tool: the headless host runs no frames to capture.
- **P4 `Profiler.UI` and the overlay**: the views `FrameTimeGraph`, `FrameStats`, `TopList`,
  `CounterStrip`, and the overlay assembled from them; off by default; F3, `--profile-overlay`, a
  script call;
  the application's `ProfilerOverlayHost` puts it on Engine.UI's global screen overlay layer;
  players, web included. Tests: each view measures and draws its numbers from a source holding a
  known history (its VG batch holds the graph's points, the bars' lengths); the overlay is off
  unless turned on. The user checks it by eye. As built: each view computes a model from the
  source (the graph's points, the rows), which the draw and the tests read; the CPU top list
  ranks scopes by their own time, so an enclosing scope does not always lead. The script call is
  `run.setProfilerOverlay` / `run.profilerAvailable`, which the default app wires to its host.
- **P5 the Profiler panel**: the views `Timeline`, `ScopeTable`, `ProfilerToolbar`; the panel
  assembled from them with the P4 views; opening a capture; the user docs page. Tests: a
  `Timeline` lays out a known frame's lanes and nesting, zoom keeps the point under the pointer,
  a table row selects its bars. The user checks it by eye.
  As built: the panel records into its own `ProfileRecording` (Foundation.Profiler: frames copied
  from the store once their GPU passes have landed, 3,600 kept), so Stop, the selection and the
  longer history never touch the store; Record and Stop replace a separate Pause. The toolbar is
  the editor's (Record, Clear, Open..., Save, Live), not a `Profiler.UI` view: it is editor UI.
  Save writes the .aeprofile and the Chrome JSON together through `WriteCaptureFiles`
  (Profiler.Capture, shared with the recorder), so there is no separate Export. Open picks from
  `<user-data>/profiles`. The tab's parts keep their natural sizes and scroll in a smaller pane
  (user 2026-10-13); closing the tab stops its recording (user 2026-10-13), and View > Profiler re-docks it beside
  the Console, and a layout saved
  before the tab existed docks it there rather than floating it. `profiler.record` (View >
  Profiler Record) toggles recording, so an agent drives it through `action_execute`. The user
  page is `editor/profiler` in the docs repo.
- **After 0.1**: a live connection to a running player (backlog-0.2): a socket source and a
  player-side server answering the control messages.
