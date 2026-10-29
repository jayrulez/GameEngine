# Skinning Benchmark & Baseline

> Status: CURRENT
> Verified: 2026-08-12 @ 7e4ebae7
>
> Since 2026-10-29 (shader-blocks-cleanup phase 3) the per-instance stream called `DataOffsets`
> below is `drawOffsets` in the forward and depth passes (`render::GpuDrawOffsets`: instance index,
> bone base, last frame's bone base, fade) and `pickOffsets` in the pick pass
> (`render::GpuPickDrawOffsets`: instance index, bone base, entity generation, entity index + 1).
> Track: [[skinning-baseline]] / [[instanced-mesh]]

`Code/Samples/AnimStressTest` is the skeletal-animation throughput benchmark - a port of
Sedulous's `EngineAnimationSandbox`. It spawns a grid of the Quaternius `Char` humanoid; every
instance gets its own `AnimationPlayer` (random clip + small speed jitter + desynced start) while
sharing one cooked model (mesh / skeleton / clips / materials).

It exists to measure the GPU-skinning architecture so we can compare **before vs after** the
Flax/Sedulous persistent-shared-bone-buffer + instanced-skinning rewrite.

## Running

```
# Debug (validation ON - correctness, not throughput)
cmake --build build/clang --target AnimStressTest
./build/clang/Code/Samples/AnimStressTest/AnimStressTest

# Release (RelWithDebInfo, validation OFF - true perf)
cmake --build build/relwithdebinfo --target AnimStressTest
./build/relwithdebinfo/Code/Samples/AnimStressTest/AnimStressTest
```

Controls: **Space** = +25 characters, **Backspace** = −25 (camera auto-reframes to keep the whole
grid in view), **WASD/QE + RMB** fly, **P** = profiler dump (CPU scope tree incl. `Anim.Drive`, plus
GPU pass timings), **Esc** = exit. vsync is OFF (`PresentMode::Immediate`) so the frame time is real
CPU+GPU work, not the display refresh.

`kAutoRamp` (a `static constexpr bool` in `main.cpp`, **false** by default) is a measurement aid: flip
it to `true` for a headless run that ramps the character count until FPS ≤ 50, prints the threshold,
and exits. The backend prints `[Vulkan] validation layers: ENABLED/DISABLED` at startup so a perf run
can confirm validation is off.

## BEFORE baseline - current non-instanced skinning (captured 2026-06-29)

| Build | Validation | Result |
|-------|-----------|--------|
| **RelWithDebInfo** | off | **~1,683 fully-rendered animated characters @ 50 fps (≈20 ms)** |
| Debug | on | ~100 chars @ 31 fps (validation-dominated - not a throughput measure) |

**~1,683 chars @ 50 fps is the number the rewrite must beat.**

### Frame breakdown @ 1,000 chars (RelWithDebInfo, validation off) - 86 fps / 11.66 ms

```
Update:            4.61 ms   (CPU)
  Anim.Drive:      4.32 ms     per-instance player eval (sample clip + compute skinning matrices)
Render:            7.15 ms   (CPU)
  Render.Extract:  0.35 ms
  Render.Compose:  6.46 ms
    Compose.Execute: 6.26 ms   record + submit + wait on GPU

GPU passes (total 4.44 ms):
  shadow.cascade ×4   ~3.16 ms   (CSM - the dominant GPU cost)
  forward             1.03 ms
  shadow.atlas ×2     ~0.14 ms
  cluster.build       0.08 ms
  tonemap             0.02 ms
```

Read: the frame is **CPU-bound** (Update 4.6 + Render 7.1 ≈ 11.7 ms serial, while the GPU finishes in
4.4 ms). The two CPU costs the rewrite targets are `Anim.Drive` (per-player skinning matrix
computation, redone every frame) and the per-pass bone uploads inside `Compose.Execute`. GPU-side, a
persistent shared bone buffer + instanced skinned draws should cut the forward + cascade draw counts.

### Frame breakdown @ 1,000 chars (Debug, validation ON) - 5 fps / 221 ms

```
Update:           24.3 ms   (Anim.Drive 23.8 ms)
Render:          238.0 ms   (Compose.Execute 235.8 ms  <- Vulkan validation-layer CPU overhead)
GPU passes:       23.0 ms   (forward 6.1, cascades ~12.9, cluster 3.2)
```

Debug is **validation-bound**, not skinning-bound: `Compose.Execute` (command record + submit) is
~236 ms because the validation layers inspect every command - ~95% of the frame, vs 6.3 ms in release.
Develop/correctness-check in Debug, but treat **release** as the perf baseline. (Both `Anim.Drive` and
the GPU pass times are ~5× the release figures, as expected for an unoptimized build.)

## AFTER - Sedulous skinning (persistent shared bone buffer + instanced skinned draws)

The rewrite (1) writes each distinct skeleton's matrices ONCE per frame into a CpuToGpu staging pool
([current][prev] slab), mirrors the populated range to a GpuOnly device buffer with one copy, and binds
that device buffer for forward + all shadow passes (no per-pass re-upload); (2) batches identical
(mesh, material) skinned instances into one instanced draw, flowing each instance's bone base through
`DataOffsets.y` (current) / `.z` (prev) so one draw skins N characters, drawn per-submesh for correct
multi-material rendering.

| Build | Validation | BEFORE | AFTER | Gain |
|-------|-----------|--------|-------|------|
| **RelWithDebInfo** | off | 1,683 | **~3,100 chars @ 50 fps** | **+85% (1.85×)** |

(Baseline + before/after measured on the original heavy scene - 18 clustered point lights + spot +
point-cube shadows. The scene was then **lightened to one directional shadow light + floor +
characters** to isolate skinning, matching Flax's "basic characters" reference. The threshold barely
moved - ~3,045 lightened vs ~3,120 heavy - which proves the scene was never lighting-bound.)

### Frame breakdown @ 1,000 chars (RelWithDebInfo, validation off) - 86 → 143 fps (11.66 → 7.0 ms)

```
                 BEFORE      AFTER
Update:          4.61 ms     3.27 ms    (Anim.Drive 4.32 -> 3.13)
Render:          7.15 ms     1.73 ms    <- Compose.Execute 6.26 -> 0.67 ms
  (per-pass bone re-upload + per-character draws gone)
GPU passes:      4.44 ms     5.41 ms    (forward 1.03 -> 1.34; cascades similar)
```

The render-CPU cost collapsed (~4× on `Compose.Execute`): bones upload once, and skinned characters
draw instanced. The frame is now **`Anim.Drive`-bound** (the per-player skeleton evaluation, recomputed
on the CPU every frame) plus the scene transform-update over the ~34-entities-per-character hierarchy -
the next two frontiers (CPU-side animation eval / collapsing the per-character entity count).

### Lightened scene (1 directional light) - confirms NOT lighting-bound

Frame breakdown @ 1,000 chars (RelWithDebInfo) - 151 fps / 6.6 ms:
```
Update:   3.34 ms  (Anim.Drive 3.15 ms - per-character, light-independent)
Render:   2.39 ms  (Compose.Execute 0.52 ms; Render.Acquire 1.17 ms = GPU backpressure)
GPU:      5.29 ms  (shadow.cascade ×4 ~3.81 ms  <- dominant; forward 1.39; cluster.build 0.06)
```
GPU is essentially unchanged from the heavy scene (5.29 vs 5.41 ms) - the 18 point lights + spot +
point-cube shadows cost ~0.1 ms. The real GPU cost is the **4-cascade CSM**, which re-draws every
character's depth four times (a char-scaled cost). So beyond the CPU frontiers, fewer cascades / tighter
per-cascade caster culling is the GPU lever - not lighting.

Prev-frame bone matrices are stored + flowed (`DataOffsets.z`) but not yet consumed by a velocity
target; when a motion-vector pass lands it's a shader-only change.

## Limitations this benchmark exposed

1. ✅ **FIXED - bones re-uploaded per pass.** Each caster's bone matrices were uploaded separately for
   the forward pass *and* every CSM cascade (and local-shadow passes) - roughly `N × bones × (5+)`
   matrices/frame, which overflowed the old 262,144-slot ring at ~1,800 chars and silently dropped
   *forward* draws (characters vanished from the main view while their shadow casters kept rendering in
   the light pools). The rewrite uploads each skeleton's matrices **once** into a staging pool +
   GpuOnly device mirror, shared across all passes. (`kMaxBoneMatrices` stays `1<<20` as headroom.)
2. ✅ **FIXED - skinned meshes can't be instanced.** The instanced path had no per-instance bones, so
   identical skinned meshes collapsed to a single bind pose. The rewrite flows each instance's bone
   base via `DataOffsets.y`, so skinned meshes batch into instanced draws like static ones.
3. **Full skeleton spawned as scene entities (still open).** Each character spawns its entire node
   hierarchy (~34 entities including skeleton joints) → 13,600 entities at 400 chars, inflating the
   scene transform-update cost. Sedulous uses ~1 entity per character (the skeleton lives inside the
   player). With the GPU side now cheap, this + the CPU `Anim.Drive` eval are the next frontiers.
