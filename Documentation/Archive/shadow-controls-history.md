# Shadow controls - the controls to tune a light's shadow, and what the mottling was

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

## The problem

PaperKid's house walls show a blotchy, chevron mottling under the roof. Found while tuning its
look (2026-10-03): it is there with ambient occlusion off and gone with the sun's shadows off,
on the walls the sun meets at a shallow angle (N.L ~ 0.17).

- A light offers only `castsShadows` and `shadowUpdate`; nothing a scene author can tune. The sun's
  biases are constants (`MeshRendererImpl.cpp`, `TerrainRenderer.cppm`; Sedulous's
  `MeshRenderer.bf:1345` and `TerrainRenderer.bf:30` the same).
- First read as shadow acne from the near-zero normal offset (0.02 texels). Measured, it is not:
  a real-device probe of PaperKid's wall, sun and camera (Render.Backend.Tests,
  ShadowControlProbeTests) reads clean across a sweep of offsets from 0.02 to 2 texels; the
  casters' slope-scaled hardware bias carries acne. Raising the offset to 1 texel leaves the
  mottling in the game unchanged.
- What it is: the roof overhang's shadow, under-resolved. The cascades fit the camera's far plane
  (PaperKid's 400 m, clamped to the 300 m shadow reach), so a near cascade's texel is large, and
  the grazing sun stretches it about six times across the wall: the shadow edge breaks into
  chevrons. With the camera's far plane at 60 m the same frame shows the roof's shadow as a clean
  band (2026-10-03, look.py crops).

## Built (shadow-controls branch)

### 1. Shadow controls on the light

On the light component (data version 1), shown only while `castsShadows`:

- `shadowStrength` (0 to 1): how dark the light's shadow gets, 1 = full. The forward and terrain
  shaders lerp the shadow toward lit by it.
- `shadowNormalBias`: the normal offset in shadow texels, default 0.02 (the old constant, kept: see
  above). Local lights get it too, as world units per unit of distance from the light (their tile's
  texel size), so it means the same thing for every light.
- `shadowDepthBiasScale`: the depth-compare bias as a scale of the light type's default (1 =
  default). A scale, not a value, because the sun's and a local light's depths are in different
  spaces (0.0009 and 0.0015 NDC); Unreal's Shadow Bias is relative the same way.

One source for the defaults (`render::ShadowBiasDefaults`); the terrain renderer reads the light's
values from the cascades instead of its own copy. The stored scenes with a light (PaperKid's six,
Sky Hopper's three) are re-stamped at version 1 with the defaults.

### 2. Per-scene shadow reach (what fixes the mottling)

The scene's environment settings (data version 5; a v4 scene reads the defaults) gain:

- `shadowDistance` (default 300): how far from the camera the sun's cascades reach, clamped to the
  camera's far plane but independent of it.
- `shadowCascadeSplit` (default 0.5): how the splits blend, 0 = even, 1 = most of the map near
  the camera.
- `shadowFadeDistance` (default 40): the width shadows fade out over at the reach.

They ride the snapshot (`SceneShadowSettings`) to the view's cascades, so two scenes drawn in one
frame (the editor) keep their own; the render subsystem's global shadow distance and far-fade
setters are gone. Resolution stays global: every view's cascades share one array.

PaperKid sets 60 / 0.7 / 10: the roof's shadow on a wall resolves into a band (2026-10-03). Sky
Hopper keeps the defaults.

## Tests

- Real device: a wall the sun grazes reads clean across the offset sweep and at the default; a
  cube's shadow darkens fully at strength 1, half as much at 0.5, and the open plane is untouched.
- Extraction hands each control to the shadow it casts (the sun's cascades, a local light's atlas
  entry with its normal offset per distance, the per-light strength).
- The controls round-trip with the scene at version 1; the inspector shows them only while the
  light casts shadows.
- The reach round-trips at environment v5 and a v4 payload reads its defaults; extraction carries
  it to the snapshot; a shorter reach (and a split nearer 1) gives the near cascade smaller texels.

## Not in scope

Contact-hardening or ray-traced shadows; shadow filtering changes.
