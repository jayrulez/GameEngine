# Debug-draw accumulation leaks while the window is minimized

STATUS: root-caused on Windows 2026-10-10, NOT fixed. Instrumentation that found it is in the
tree but UNCOMMITTED (see "What is already in the tree"). Platform-independent bug - the
mechanism is in Foundation/Engine code, not in any Windows path.

## Symptom, as a user sees it

Leave the editor running for a long session (hours), minimize it, restore it. The editor freezes
for seconds to tens of seconds, then recovers on its own. Over several such cycles the process
grows to many GB. It is NOT a crash and NOT a deadlock - it always recovers.

Two earlier reports that are the SAME bug, both previously unexplained:

- `Assertion failed: index < m_size` at `Core/Containers/Array.cppm:207` (`operator[]`) in a
  Release dist build, same trigger (long session, then minimize). Unresolvable at the time
  because a `CMAKE_BUILD_TYPE=Release` dist carries no debug directory at all.
- `[Vulkan ERROR] vkAllocateMemory(): pAllocateInfo->allocationSize (17179869184) is larger than
  maxMemoryAllocationSize (4292870144)`.

## Evidence

Observed sizes, all EXACT powers of two, all the same buffer label:

| run | requested allocation | = |
|---|---|---|
| 1 | 17 179 869 184 B | 2^34 (16 GiB) |
| 2 | 4 294 967 296 B | 2^32 (4 GiB) |
| 3 | 1 073 741 824 B | 2^30 (1 GiB) |

Run 3, with the allocation instrumented to print the label:

```
[Warning] RHI: large buffer allocation: 1024 MB for 'debug.vtx'
          (requested size 1073741824 B, usage bits 4)
```

And the main-loop stall watchdog, same session:

```
[Error] Runtime: main loop has not advanced for ~5000 ms (frame 164348, window minimized=1).
[Error] Runtime: main loop has not advanced for ~10000 ms (frame 164348, window minimized=1).
[Error] Runtime: main loop has not advanced for ~15000 ms (frame 164348, window minimized=1).
[Warning] Runtime: main loop resumed after ~15750 ms stalled (frame 164350)
```

`minimized=1` is recorded by the main thread on the iteration before the stall, so the link to
minimize is measured, not inferred.

## Root cause

`debug.vtx` is the debug-draw vertex buffer (`Render/Debug/DebugDrawPassImpl.cpp:472`). Its
growth is a doubling from a 4096-byte base (`DebugDrawPass::EnsureBuffer`,
`DebugDrawPassImpl.cpp:451`):

```cpp
u64 newCap = cap > 0 ? cap : 4096;
while (newCap < bytes) { newCap *= 2; }
```

which is exactly why every observed size is a power of two. `bytes` comes from the accumulated
debug geometry, so the question is why that grows without bound. The chain:

1. **Minimize makes the frame invalid.** `RenderWindow::BeginFrame`
   (`Foundation/Graphics/Graphics.cppm:531`) returns an empty `FrameContext{}` when
   `IsMinimized() || Width() == 0 || Height() == 0`.
2. **An invalid frame skips the whole render bracket.** The editor guards on
   `if (frame.valid && frame.window == host.MainRenderWindow())`
   (`Editor/Editor.App/ApplicationImpl.cpp:1057`), so neither `BeginRendering` (:1063) nor
   `EndRendering` (:1075) runs. The game player has the same shape: `RenderFrame` early-returns
   when `frame.encoder == nullptr` (`Engine/Engine.DefaultApp/DefaultApplicationImpl.cpp:721`),
   skipping its `BeginRendering` (:735) / `EndRendering` (:774). **Both hosts are affected.**
3. **`EndRendering` is the ONLY drain.** `RenderSubsystem::EndRendering`
   (`Engine/Engine.Render/RenderSubsystemImpl.cpp:614`) is where every debug list is cleared:

   ```cpp
   // Immediate-mode: clear all debug lists AFTER rendering, so next frame's draws start empty
   m_debugGlobal.Clear();
   m_debugScreen.Clear();
   for (auto& kv : m_debugScenes) { kv.value.Clear(); }
   for (auto& kv : m_debugViews)  { kv.value.Clear(); }
   ```

   (`DebugDraw::Clear` itself, `Render/Debug/DebugDrawImpl.cpp:42`, clears `m_lines`,
   `m_overlayLines`, `m_tris`, `m_overlayTris`, `m_2d`, `m_3dText`, `m_textChars`.)
4. **Producers keep producing.** Debug geometry is appended during UPDATE, not during render -
   the editor's grid and gizmos, physics wireframes
   (`Engine.Physics/PhysicsSubsystemImpl.cpp:450`), IK (`Engine.Animation`), navigation
   (`Engine.Navigation`), script `DebugDraw.of(scene)`. None of them know a frame was skipped.
5. **The loop runs unthrottled while minimized, which is the accelerant.** `RunApplication`
   (`Foundation/Runtime.Desktop/DesktopRunner.cppm`) has NO sleep and no frame limiter -
   `host.Settings().maxFrameTime` only clamps the `dt` VALUE handed to `Tick`, it never waits.
   The loop is paced solely by `Present()` blocking on vsync (`PresentMode::Fifo` is the default,
   `Graphics.cppm:154` / `RHI/Descriptors.cppm:118`). A skipped frame never presents, so nothing
   blocks and the loop spins as fast as a core allows - appends happen thousands of times per
   second instead of ~60. This is also why a minimized editor burns 100% of one core.
6. **On restore the bill arrives.** `bytes` is now enormous, `EnsureBuffer` doubles up to it,
   destroys the old buffer and creates a multi-GB `CpuToGpu` buffer, then `MemCopy`s into it.
   That is the multi-second stall, and it recovers once the allocation completes.

This also explains the `Array::operator[]` assert: at these element counts an index or count
derived from the debug lists exceeds what the consuming code expects.

## Defect 1 (the actual bug): the drain is coupled to rendering

Debug accumulation must be bounded per host iteration whether or not a frame renders. Three
options, in rough order of preference - **this is a design call for whoever fixes it**:

- **(a) Clear at the START of the accumulate window instead of the end.** Move the clears from
  `EndRendering` to `BeginRendering`, or to wherever the host begins an update. Smallest change,
  but still skipped if a future path skips the whole bracket.
- **(b) Clear unconditionally in the host's frame bracket**, outside the `frame.valid` guard, in
  BOTH `ApplicationImpl.cpp` and `DefaultApplicationImpl.cpp`. Holds no matter why rendering was
  skipped; costs a second call site to keep in sync.
- **(c) Make the producers no-op when there is no consumer** (e.g. `DebugDraw` stops appending
  when the frame it belongs to was never begun). Most robust, largest change.

Note the comment at `RenderSubsystemImpl.cpp:624` states the intended invariant - "clear all
debug lists AFTER rendering, so next frame's draws start empty (the app accumulates during its
update, before the next BeginRendering)". The invariant is right; it just isn't enforced when
rendering does not happen.

A defensive cap on `EnsureBuffer` would ALSO be reasonable on its own merits (a debug vertex
buffer has no business reaching 16 GiB), but it is a backstop, not the fix - it would turn the
leak into dropped debug geometry rather than bounding the CPU-side lists, which are what grow.

## Defect 2 (independent): the main loop busy-spins when nothing is presented

Worth fixing regardless of Defect 1: a minimized app should not peg a core, and this is the
~100x accelerant that turned a slow leak into a visible hang. When an iteration presented
nothing, sleep a few ms before the next one. Beware of two things:

- Do not sleep when a frame DID render; the vsync wait is the pacing and adding to it drops fps.
- The web runner (`Runtime.Web`) is a `requestAnimationFrame` callback, not a loop - it is paced
  by the browser and must not be given a sleep.

**Suggest fixing Defect 1 FIRST and verifying the repro still happens.** Fixing Defect 2 first
slows accumulation by roughly a hundredfold and would make the remaining bug take days rather
than minutes to show, which is how it stayed unexplained for two sessions.

## What is already in the tree (uncommitted)

Three instruments, added while chasing this. Keep or drop as you see fit; the first is how the
bug was found and is worth keeping.

1. **`Foundation/Runtime.Desktop/DesktopRunner.cppm`** - `detail::StallWatchdog`. A heartbeat the
   loop bumps plus a watcher thread (250 ms poll, 5 s threshold) that reports WHILE the main
   thread is stuck, with frame number and the minimized flag. A per-frame timer cannot do this:
   it only reports after the frame ends, and the frames that matter never end. This is the
   instrument that produced the `minimized=1` evidence above.
2. **`Foundation/RHI.Vulkan/VkBuffer.cppm` + `VkTexture.cppm`** - log label + size for any
   allocation >= 256 MB, before allocating. This is what printed `debug.vtx` and named the
   culprit; the validation layer only ever reported a size with no owner. Worth keeping, and
   worth porting to the DX12/WebGPU backends.
3. **`Foundation/UI/Core/View.cppm` + `UIClusterImpl.cpp`** - a 256-deep cap on the style
   inheritance walk (`ComputeStyle` -> `Parent->ResolveStyle`, and the `inherit` hop in
   `ResolveKeywords`; `var()` chains were already capped at 8 but these two were not), plus
   `LiveViewCount()` / `PeakViewCount()` with a doubling-threshold warning. **These were chasing a
   wrong hypothesis** (a view leak) and never fired. The depth cap is defensible on its own - an
   unbounded recursion up a parent chain is a real hazard - but it is unrelated to this bug.
   Their silence is itself useful evidence: fewer than 65 536 live views, under 256 levels of
   style inheritance.

## How to verify a fix

1. Build the editor with symbols - `build-msvc.cmd --preset msvc-reldbg --target Tools.Editor`,
   or the `clang-reldbg` preset. Do NOT use a `Release` dist: asserts are live there (they are
   gated on `BUILD_SHIPPING`, not `NDEBUG`) but it emits no PDB, which is why the original
   `Array` assert was unresolvable.
2. Open a project, leave it running, minimize, wait, restore. Repeat a few times.
3. **Pass:** no `large buffer allocation ... for 'debug.vtx'` line, no stall-watchdog error, and
   the process does not grow across minimize/restore cycles.
4. A direct regression test is possible without a window: drive the host's frame bracket with an
   invalid `FrameContext` N times and assert the debug lists are still empty. `Shell.Null`
   (`IsMinimized()` is settable there) is the obvious substrate.

## Not established

- Whether anything OTHER than debug draw accumulates across skipped frames. The same "only
  drained by the render path" shape could exist elsewhere - the GPU retire queues
  (`GpuRetireQueue`, `Render/Resources.cppm:71`) are drained on frame completion and are worth
  an audit under the same lens.
- Whether the earlier `Array::operator[]` assert is the debug lists specifically or a consumer of
  them. Plausible but not traced; nobody has symbolized that trace.
- An unrelated latent bug noticed in passing: `DynamicUniformRing::AllocateRange`
  (`Render/Resources.cppm:376`) bounds-checks with `m_cursor + count > m_slotsPerFrame` in u32,
  which WRAPS for a large `count` and lets the check pass. Not implicated here, but it would
  convert a bad count into an out-of-bounds write into mapped GPU memory.
- The `vkCreateGraphicsPipelines(): Vertex attribute at location 2 not consumed by vertex shader`
  warning that appears alongside this is a separate, harmless pipeline/shader layout drift.
