# Orthographic views - every effect under an orthographic camera, then the editor's

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: BUILT 2026-10-12/13, every phase (P1 also took the sky's zero velocity under
> orthographic, so TAA's history stays finite; P2 revised how distances from the camera read,
> below; P3 reads the projection's kind off the inverse projection, the SSR push being full). Backlog
> entries it closes: `Plans/backlog-0.1.md` (Editor: an orthographic editor camera) and
> `Plans/weekly_backlog.md` (Post effects for an orthographic camera). Read CONVENTIONS.md first.

## What it is for

The scene view's camera switches between perspective and orthographic: a toggle and a key, and
the orientation gizmo's faces giving the top, front and side views that orthographic is mostly
used for. The user ruled (2026-10-11) that the renderer comes first: several effects are switched
off for any orthographic view today, so an orthographic editor view, and a game's orthographic
main camera (isometric, 2.5D, a top-down map), look worse than the same scene in perspective.
Both get the whole renderer.

## Where things stand (read in the code, 2026-10-12)

What already works under an orthographic projection:

- `CameraComponent` has `projection` (Perspective / Orthographic) and `orthoHeight`, in the
  inspector; `MakeCameraProjection` (`Engine.Render/RenderComponents.cppm:274`) builds
  `OrthographicRH` from them. The game's camera, its pointer ray, picking, decals, LOD
  (`LodMath.cppm:40`) and the cluster build (`ClusterSystemImpl.cpp:202`) handle it.
- `projection::IsOrthographic(m)` (`Core/Math/Float4x4.cppm:182`) tells the two apart.
- Shaders that rebuild a view position through `InvProj` / `InvViewProj` are convention-free.

What is switched off: `LimitPostForOrthographic` (`RenderComponents.cppm:1118`, applied at
`RenderSubsystemImpl.cpp:500`) turns off AO, SSR, SSGI and TAA for an orthographic view.

What assumes a perspective eye, and so is wrong (or NaN) under an orthographic one:

| Where | What it assumes |
|---|---|
| `ao_ssao.ps.hlsl:35` `ViewToUv` | NDC = view xy * ProjXX/YY / -z |
| `ao_gtao.ps.hlsl:42,46` | V = normalize(-P); the march radius / -P.z |
| `ssr.ps.hlsl:86`, `ssgi.ps.hlsl:56` | V = normalize(-P); `ViewToLocal` divides by -z; SSGI clamps rays at the eye |
| `taa.ps.hlsl:72,105`, `depth.hlsli` `LinearizeDepth` | perspective depth linearization for the disocclusion test |
| `Views.cppm` `ApplyProjectionJitter` | jitter in proj(2,0)/(2,1), which scales with clip.w; under orthographic w is 1, so the offset varies with depth (it belongs in proj(3,0)/(3,1)) |
| `particle.ps.hlsl:19` (`DepthParams`, `ParticleRenderer.cppm`) | soft particles linearize the scene depth as perspective |
| `sky.vs.hlsl:21` | the sky ray is the far-plane point minus the eye: a fan |
| `sky.ps.hlsl:28` | velocity from a direction (w = 0) through PrevViewProj: w is 0 under orthographic, a NaN |
| `forward.ps.hlsl:79`, `surface/forward.hlsli:45` | V = normalize(CameraPos - worldPos) for specular |
| `instance_fade.hlsli:15`, `debug_grid.ps.hlsl:63` | fade by distance to the eye |
| `ViewCamera` (`Render.Api/RenderApi.cppm:43`) | carries `farZ` but no near plane; 0.1 is hard-coded at `ClusterSystemImpl.cpp:154`, `ShadowSystem.cppm:54` (cascade splits) and `PipelineImpl.cpp:2198` (TAA) |
| `PipelineImpl.cpp:1595` | the shadow reach is measured from the eye, a distance an orthographic camera's placement makes arbitrary |

The editor: the scene view, its gizmo camera and the preview viewport build
`PerspectiveFovRH(1.0472, ...)` directly (`ScenePageImpl.cpp:266,1229`,
`PreviewViewportImpl.cpp:189`). `EditorCamera` (`Editor.Camera/EditorCamera.cppm`) zooms, frames
and pans by distance and field of view; `MakeMouseRay` (`ScenePageImpl.cpp:876`), the gizmo's
`ScreenScale` (`Gizmo.cppm:106`), the marquee, the grid's spacing and the scale bar assume a
perspective eye. `ViewGizmo.cppm` (the orientation gizmo) already snaps to the six faces.

## Design

### One description of a view's projection

`ViewCamera` answers `IsOrthographic()`, `NearZ()`, `FarZ()` and `OrthoHeight()` from its
projection matrix (`projection::NearPlane` / `FarPlane` read the planes back from the z column,
which neither projection's jitter touches), so none of the thirty-odd places that build a view
camera has to repeat them, and none can disagree with the matrix. The three hard-coded 0.1s read
`NearZ()`. Every shader that needs to know reads one flag and two numbers from the view
constants it already has (a `ViewParams` block where there is none): orthographic or not, near,
far. A shared `projection.hlsli` (`view.hlsli` is taken: the view constants) holds the two-way
helpers so each effect does not grow its own branch:

- `ViewPosFromDepth(uv, depth)`: through `InvProj`, as today (already correct for both).
- `ViewToNdc(vp)`: perspective divides by -z; orthographic does not.
- `ViewDir(viewPos)`: towards the eye; under orthographic, the constant (0, 0, 1) in view space.
- `LinearDepth(depth)`: perspective or orthographic linearization (`depth.hlsli` gains the
  orthographic one; reverse-Z either way).
- `WorldViewDir(worldPos)`: `CameraPos - worldPos` normalized, or minus the camera's forward.

### The effects

- **AO (SSAO, GTAO)**: `ViewToNdc` and `ViewDir`; GTAO's march radius in screen space is the world
  radius over the view's height in world units (orthoHeight) instead of over -z.
- **TAA**: the jitter goes in proj(3,0)/(3,1) for an orthographic projection (a constant NDC
  shift either way); the disocclusion test uses `LinearDepth`. Reprojection is matrix-based and
  already right.
- **Sky**: under orthographic the sky is drawn as a perspective camera with the same rotation
  and a fixed 60 degree field of view would see it, so the background still reads as sky; its
  velocity comes from the rotation change alone (no w = 0 divide). This is a choice (see
  Decisions).
- **Soft particles**: the scene depth linearized for the projection it was drawn with.
- **Specular and other view vectors** in the forward and surface shaders: `ViewDirToCamera` in
  `view.hlsli`, which tells an orthographic view by its view-projection's zero w column (a
  surface's `viewDir` comes from `ViewEyeFor`).
- **Distances from the camera** (the instance fade, the grid's fade, vegetation chunks, the local
  shadows' priority, particle sorting) keep measuring from the camera's position in both
  projections (revised 2026-10-12, building P2). Under orthographic every point is the same size
  on screen, so no distance is the right one; measuring from the near plane faded everything out
  for a camera placed far back. An orthographic camera's position is where its author puts it,
  and the editor keeps its eye where the matching perspective camera stands (P4), so these read
  as they would in perspective.
- **SSR and SSGI**: parallel rays. The ray's origin is the pixel's view position and its eye
  direction is `ViewDir`; the march projects with `ViewToNdc`; SSGI's near-plane clamp uses
  `nearZ` rather than the eye. The larger of the effects, last.
- **Shadows**: the cascade splits start at `nearZ` (P0) and cover [nearZ, nearZ + reach] of the
  view box, measured along the view from the camera as in perspective; the box fits an
  orthographic frustum as it does a perspective one.

When the last of these lands, `LimitPostForOrthographic` and its test go.

### The editor camera

- `EditorCamera` gains `orthographic` and `orthoHeight`. One function builds the scene view's
  projection from it (the view, the gizmo camera and the preview all call it), so the three
  sites stop hard-coding the field of view.
- **Placement**: switching does not move the camera. An orthographic editor camera keeps its eye
  where the perspective one stood, its pivot (`position + Forward() * focusDistance`) and its
  near and far planes, so orbiting turns about the same point and what measures from the camera
  (fades, sorts, the shadow reach) reads as it did.
- **Switching keeps the framing**: perspective to orthographic sets `orthoHeight` to what the
  field of view spans at the pivot (2 * focusDistance * tan(fovY / 2)); back the other way sets
  the focus distance from the height.
- **Zoom** scales `orthoHeight` (the wheel's fraction, as now); **pan** moves by pixels times
  orthoHeight over the viewport's height; **frame** (F) fits the height to the bounds; fly (WASD)
  moves as now, and forward/back move the pivot.
- **Picking and tools**: `MakeMouseRay` gives each pixel its own origin on the near plane and the
  forward direction; the gizmo's `ScreenScale` is orthoHeight times its fraction;
  `ViewportToolInput` carries the projection so tools ask it rather than assume; the marquee
  projects through the matrix; the grid picks its spacing and the scale bar its length from
  orthoHeight.
- **The toggle**: the orientation gizmo's centre (a click) and the View menu's "Orthographic"
  (an editor action, so MCP and the command palette have it), keys Numpad 5 as in Blender and
  O. The gizmo shows which is on.
- **MCP**: `viewport_camera_get` / `viewport_camera_set` gain `orthographic` and `orthoHeight`;
  `McpGuide.md` says so.

## Phases

Each phase is its own commit (or two), with its tests, both compilers green. The renderer phases
are checked by the user in an orthographic scene (a test scene in a sample, or the editor once
P4 lands; until then a game camera set to Orthographic in the inspector).

- **P0 - one description of the view.** `ViewCamera`'s answers from its projection; the three
  0.1s read them. Tests: the planes read back from both projections, jittered or not; the cascade
  splits start at a camera's own near plane. (`projection.hlsli` lands with its first users, P1.)
- **P1 - AO and TAA.** As above, lifted out of `LimitPostForOrthographic`. Tests: a headless
  render of a box on a floor through an orthographic camera darkens the inner corner and leaves
  the open floor at 1 (AO); the orthographic jitter shifts a point at two depths by the same
  pixel offset, and a still orthographic scene under TAA converges to its unjittered image within
  a tolerance.
- **P2 - the sky, soft particles, view vectors, shadows.** Tests: the sky's stand-in is a
  perspective with the view's aspect looking where the view looks, and a direction through it
  stays put for a still camera and moves for a turning one; an orthographic view's cascades cover
  its frustum from its near plane to the reach. The soft particles' and the view vectors' shader
  changes have no pixel probe (none exists for lit particles or specular); the user checks them.
- **P3 - SSR and SSGI.** As above; `LimitPostForOrthographic` removed. Tests: an orthographic view
  of a mirror floor reflects a box above it where the CPU ray says it should (a few pixels'
  tolerance); SSGI's result is finite everywhere.
- **P4 - the editor camera.** As above. Tests (Editor.Camera, Editor.Scene): switching keeps the
  pivot and the framing; zoom, pan and frame in orthographic; the mouse ray's origin moves with
  the pixel and its direction does not; the gizmo keeps its screen size as the height changes;
  the MCP tools round-trip the new fields.
- **P5 - docs.** `Systems/renderer.md` (orthographic views, `projection.hlsli`), the editor's docs and
  `McpGuide.md`, the weekly, the two backlog entries closed; port notes to Sedulous for each
  phase as it lands.

## Decisions

1. **The orthographic sky** (user: yes): drawn as a 60 degree perspective camera with the same
   rotation would see it, so it still reads as sky behind an isometric scene (not one flat colour
   along the view direction).
2. **Auto-orthographic on the gizmo's faces** (user: yes): snapping to a face switches to
   orthographic and orbiting away switches back (Blender's "auto perspective"), with an editor
   setting to turn it off.
3. **The key**: Numpad 5 (Keypad5) as in Blender, and O for keyboards without a pad. O is free in
   the scene view (W/E/R pick the gizmo, Q and E sink and rise while flying, X switches its space,
   V snaps, F frames) and reads as "orthographic" (user: committed with it).
4. **Order** (user: yes): P0, P1, P2, then the editor camera (P4); SSR and SSGI (P3) when ready.

## Gotchas

- Reverse-Z: the near plane is depth 1 and the far 0 in both projections (`depth.hlsli`); the
  orthographic linearization is linear in depth, not hyperbolic.
- A camera placed far back is normal under orthographic, and what measures from its position
  (fades, the shadow reach, sorts) measures from there; the editor keeps its eye where the
  perspective camera stood.
- The jitter row differs by projection; get it wrong and TAA smears by depth.
- Every new shader branch reads a uniform, not a specialization, unless the cost shows; check the
  shader corpus count is unchanged (no new variants).
- Sedulous mirrors the renderer: send each phase's port notes as it lands.
