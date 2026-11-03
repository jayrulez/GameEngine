# Color pipeline - what is entered is what is seen

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

## The problem

PaperKid's grass and houses look pale. Their colours were typed as the colours wanted, and the
renderer draws something lighter and greyer.

- A colour has two readers that disagree about what its numbers mean:
  - The UI draws every colour as sRGB ("Takes the theme's color as authored (sRGB, like every
    UI color)", `UIRuntime.cppm:227`). The colour picker's swatch, its HSV square and its hex
    field therefore show a stored 0.5 as sRGB 0.5, mid grey (`#808080`).
  - The renderer uses the same 0.5 as LINEAR light: a mesh's `color` goes to the GPU untouched
    (`ExtractImpl.cpp:87`, `rd.color = mc.color`), and so do a light's colour, the ambient and
    sky colours, a sprite's tint and a material's `BaseColor`. Linear 0.5 displays as sRGB
    0.735 (`#BCBCBC`).
- Images are already right: a colour image is uploaded in an sRGB GPU format and the hardware
  decodes it to linear on sample (`FormatUtils.cppm:28`). So a texture pixel of 128 and a picked
  tint of `#808080` do not match today: the tint renders far lighter. Images and colours follow
  different rules.
- The pickers: `ColorPicker` (HSV, RGB fields and hex, 0 to 1), shown by `ColorEditor`, the
  property-grid swatch the inspector (`InspectorViewImpl.cpp:782`, :1547, :2146), the material
  page (`MaterialPageImpl.cpp:602`) and the particle page (`ParticleEffectPageImpl.cpp:179`) use;
  and `HDRColorPicker` (an LDR colour times an intensity, giving a `Float4` above 1), which
  nothing in the editor uses yet. Neither converts anything.
- A material has no colour type: a colour parameter is a `Float4` (`MaterialBuilder::Color`
  forwards to `Float4`), and the material page shows EVERY `Float4` with a colour editor, whether
  it is a colour or not.
- Imports: glTF's `baseColorFactor` and `emissiveFactor` are linear by the glTF specification and
  are stored as read (`GltfLoader.cppm:202`, :251); FBX's colours are the DCC's display values,
  also stored as read (`FbxLoader.cppm:256`).

## Proposal

One rule, the one images already follow: **an authored colour is sRGB; the renderer decodes it to
linear once, where it hands the colour to the GPU.** Shading, lighting and blending stay linear.

### 1. Authored colours store sRGB

Every colour field that means "the colour you see" stores the sRGB value: a mesh's or instance
set's colour and tints, a sprite's and decal's tint, a light's colour, the ambient colour, the
sky's horizon, zenith and ground, a material's colour parameters, a particle's colours, UI colours
(already so). The render extraction decodes them (`SrgbToLinear`, `Color.cppm:165`) as it copies
them into render data; nothing upstream of it changes meaning.

Why sRGB in the data and not linear with a converting picker (both look the same on screen):

- The numbers mean the same everywhere: a scene file, the inspector, a hex code, a script's
  `Color(1, 0.5, 0)`, an MCP call and an image editor all agree with what is seen. With linear
  storage every script and tool that writes a colour would have to convert, and the ones that
  forget write PaperKid's pale colours again.
- Colours and images follow one rule, so a tint of `#808080` over white matches a texture of 128.
- A fade between two colours (property animation, a script's lerp) moves evenly to the eye; a
  fade done in linear rushes through the darks.

### 2. Brightness above 1 is an intensity, never a colour

sRGB has no meaning above 1. A colour that has to be brighter than white is an sRGB colour plus a
separate linear intensity, as lights already are (`color` plus `intensity`).

- A material's `EmissiveColor` is a `ColorHdr`: the sRGB colour in rgb and the intensity in w
  (the forward cbuffer has no spare lane for a separate field). The GPU receives linear rgb times
  the intensity. glTF's `KHR_materials_emissive_strength` becomes that intensity.
- Particle colours have no intensity of their own yet. A value above 1 decodes along the same
  curve and stays over-bright; a colour-plus-intensity split comes when an effect needs one.

### 3. Every `Color` is sRGB; materials say which uniforms are colours

- No per-field marker: every reflected `Color` field is an authored sRGB colour, and nothing in
  the engine today is a linear colour stored as a `Color`. A field that ever needs another
  meaning gets a different type, not a flag.
- A material property gains two colour types, `MaterialPropertyType::Color` (sRGB rgba) and
  `ColorHdr` (sRGB rgb, linear intensity in w). `MaterialBuilder::Color` and `::ColorHdr` declare
  them; the one uniform upload decodes them (`EncodeUniformsForGpu`), so shaders keep receiving a
  linear `float4`. The material page shows a colour picker only for these (a `ColorHdr` adds an
  intensity row) and plain number fields for any other `Float4`.
- A material stored before these types declared its colours as `Float4`. As it is read, a `Float4`
  that the builtin template of its shader (`CreatePBR` for `forward`, `CreateUnlit` for `unlit`)
  declares as a colour takes the template's type; a custom shader's `Float4`s are left alone.

### 4. Where the decode happens

Once per path, where the colour is handed to the GPU:

- Render extraction (`ExtractImpl.cpp`): mesh and instance colours and tints, sprite and decal
  tints, light colours, the ambient and sky colours, the camera clear.
- The material uniform upload (`EncodeUniformsForGpu`, `MaterialSystem`).
- Particles where they leave the simulation (`ParticleColorToLinear`): billboards, trails,
  mesh-particle tints, particle lights. They are simulated as entered, so a gradient moves evenly
  to the eye.
- Vertex colours that arrive as bytes, in the shader: debug draw (`debug_geom`, `debug_screen`)
  and VG (already so), through the shared `color.hlsli`.
- Viewport backdrops (`ViewportView::LinearClearColor`): a viewport's clear colour is a UI colour.

The pickers need no conversion of their own: they show and edit the stored sRGB value.

### 5. Imports keep their look

- glTF: `baseColorFactor` and `emissiveFactor` are encoded linear to sRGB as they are read
  (`GltfLoader.cppm`, `SrgbFactor`).
- FBX (and OBJ, read by the same loader): encoded too. Its colours are the DCC's scene-linear
  values: Platformer Game Kit's character, exported from Blender as both FBX and glTF, carries
  the same numbers in each. A legacy material's DiffuseFactor is ignored, as glTF has none
  (Blender writes 0.8 there, which made the FBX a fifth darker than its glTF).
- Images: unchanged. The texture import's colour space setting stays the switch between colour
  imagery (sRGB) and data (normal, roughness, masks: linear).

### 6. Existing content

Content authored so far renders darker and richer, because its colours finally mean what was
typed. The engine's own values picked under the raw reading (the default ambient and sky colours,
the editor's viewport backdrops) are re-expressed in sRGB so they look as before. The samples:
Sky Hopper's colours were tuned under the old reading and its imported materials hold linear glTF
values, so they are converted to keep its approved look; PaperKid's colours were typed as the
colours wanted, so they stay, and its lighting is retuned.

## Known gaps

- Post-tonemap passes (debug draw, post-tonemap sprites and world UI, the VG scene overlay) output
  linear values, right for an sRGB or float target. A plain UNORM target (a browser canvas, a
  Vulkan surface without an sRGB format) stores what it is given, so they come out dark there;
  the tonemap and FXAA passes already encode for it (`EncodeOutput`). Older than this change.
- The ImGui extension passes its byte colours raw.
- The terrain shader's fallback ramp (no layers painted) is a pair of constants in the shader.

## Tests

- Real device (`Render.Backend.Tests`, ColorProbeTests): a debug colour reads back from an sRGB
  target as the bytes entered; an unlit quad whose `BaseColor` is sRGB 0.5 over white renders the
  same pixel as one whose texture is 128 under white.
- Extraction: every component colour reaches render data as `ToLinear` of the stored value.
- Materials: `Color` and `ColorHdr` decode at upload and a `Float4` does not; the builtin
  templates declare their colours; a stored `Float4` colour takes the template's type, idempotent,
  and a custom shader's does not; the material page's row follows the type.
- Particles: a simulated colour reaches billboards and particle lights decoded.
- glTF: a `baseColorFactor` of linear 0.2140 reads as sRGB 0.5; emissive strength becomes the
  intensity.
- Viewports: the clear colour is cleared with its linear value.

## Not in scope

Wide-gamut or HDR display output; a colour-managed (ACES/OCIO) authoring pipeline; changes to the
tonemapper.
