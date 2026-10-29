# Authoring terrain

The end-to-end workflow for building a terrain in the editor: the assets that make one up, the
layer/texture model, sculpting, splat painting, and the gotchas that cost time in practice. For
the engineering side see `Archive/terrain-history.md`, `Archive/terrain-splat-topk-history.md`, and
`Archive/terrain-layer-pbr-history.md`.

## What a terrain is made of

A terrain is a **composition of assets**, referenced by guid:

- **Heightfield asset** - the geometry: a square grid of 16-bit heights over a world-size XZ
  footprint. Shared: physics colliders and navigation reference the SAME heightfield asset.
- **Terrain asset** - the composition: references the heightfield, the splat weights, the BASE
  layer, and the paint palette (textures + tile scales). This is what a `TerrainComponent` in a
  scene points at.
- **Splat weights asset** - the paint data: which palette layer covers which spot, at what
  strength. Written by the paint brush.
- **Texture assets** - albedo (and optionally normal + ORM) maps for the base and each palette
  layer.

Cooking turns these sources into runtime products; the editor re-cooks automatically after edits.

## 1. Get a heightfield

Import a heightmap (16-bit PNG or raw `.r16`) - it becomes a `HeightfieldAsset` and is resampled
onto the asset's grid. Or create the asset directly and sculpt from flat. Grid sizes must be
`64k+1` (65, 129, 257, 513, 1025, ...); the asset also carries the world XZ footprint and the
`[minY, maxY]` height range the 16-bit samples map onto. The heightfield page shows the grid as a
2D grayscale image - the 3D view lives on the terrain page and in the scene.

## 2. Create the terrain asset

Create a Terrain asset and open its page:

- **Heightfield**: pick the heightfield asset.
- **Weights**: pick an existing splat-weights asset, or press one of the **Create weights**
  buttons (512 / 1024 / 2048) to author a blank one and wire it in. A fresh raster is all-base -
  nothing is painted yet. You can also import a PNG as weights (legacy channel layout: R = base
  share, G/B/A = the first three paint layers).
- **Base layer**: the albedo (plus optional normal/ORM/height) that shows wherever nothing is
  painted - and what the eraser reveals. The base is **never painted directly**; it is the canvas.
  With NO base albedo assigned, the built-in height/slope ramp (the greenish fresh-terrain look)
  acts as the base, so painting layers composites over it and erasing returns to it.
- **Paint layers**: add as many as you want (the palette is unbounded; up to 256 layers can be
  referenced by the 8-bit paint index, and up to 4 blend at any single texel). Each layer has an
  albedo picker, optional **normal**, **ORM**, **height**, and **mask** pickers, and a **tile scale**
  (world units per texture repeat, shared by all maps of the layer).
- **Height blend**: a per-terrain slider (0..1, default 0.25) that only bites once a layer has a
  height map assigned. Smaller = crisper, interlocked seams; larger = a wider soft skirt.
- **Palette texture size**: the common resolution every palette texture is resampled to at cook
  (default 1024). Raise it for hero terrains, lower it for cheap ones.

Then add a `TerrainComponent` to a scene entity and pick the terrain asset in the inspector.

### Per-layer textures: albedo, normal, ORM, height, mask

- **Albedo** is the color map (sRGB - the importer's default is correct).
- **Normal** is a tangent-space normal map. Optional; missing = flat.
- **ORM** is one packed texture: **R = ambient occlusion, G = roughness, B = metallic** - the
  standard glTF shared-image layout, so exported ORM textures drop straight in. Optional;
  missing = AO 1 / roughness 1 / metallic 0 (today's default look).
- **Height** is a grayscale displacement map (the `*_disp_*` file from a texture pack); only the red
  channel is read. It does NOT move geometry - it re-biases the blend so, at a boundary, the layer
  whose local height is greater shows through first (gravel in the low spots, grass on the high
  tufts) instead of a uniform cross-fade. Optional; assign it on two or more layers and set the
  **Height blend** slider to taste. NOTE: turning height-blend on trades soft dissolves for crisp,
  interlocked seams across the WHOLE terrain - existing soft gradients will visibly sharpen; raise
  the contrast slider to widen the skirt back out.
- **Mask** is a grayscale coverage/opacity map (the `*_mask_` file that ships with SPARSE sets - sparse
  grass, scattered gravel); only the red channel is read. It cuts the layer's paint weight so the layer
  shows only where the mask is opaque, and the freed coverage reveals the OTHER layers you PAINTED
  underneath it (weighted by how uncut each layer's own mask is there) - so grass gaps show the ground
  layer you painted, falling to the base
  canvas only where nothing else is painted. Palette layers only. Optional; missing = fully opaque.
  IMPORTANT caveats: (1) the reveal only works where the lower layer still has weight - painting the
  sparse layer to FULL strength evicts the layers under it (flat top-K has no stacking), so paint it a
  touch lighter or the gaps show base. (2) Many photo textures (e.g. Poly Haven `sparse_grass`) already
  bake the dirt into the diffuse's gaps - those look right from the diffuse ALONE, and adding the mask
  double-cuts them; only use a mask when the diffuse is a clean overlay. At a distance the mask's mip
  average softens sparse coverage toward a uniform mix (intended; coverage-preserving mips are a later
  refinement).

**Normal handedness**: our terrain uses the glTF / OpenGL green-up convention (verified by probe), so
Poly Haven `_nor_gl_` maps and glTF-exported normals import AS-IS - do NOT flip green.

**Color-space gotcha**: normal and ORM maps are *data*, not color. If you hand-import one as a
texture asset, set its **colorSpace to Linear** (the import default is Srgb, which warps data
maps). Textures brought in by the model importer as material maps are already Linear.

**Pick the right file**: texture packs ship `*_diff_*` (color) next to `*_disp_*` (displacement/
height), `*_nor_*` (normal), `*_rough_*` / ORM, and `*_mask_*` (coverage) maps with near-identical
names. Assign each to its matching slot - a `*_disp_*` map belongs in the **height** picker and a
`*_mask_*` in the **mask** picker, not albedo (as albedo they render as flat gray). Hover a layer
swatch in the paint panel to see the asset NAME and confirm.

**EXR sources**: our importer decodes PNG/JPG/TGA/HDR but NOT OpenEXR. Poly Haven often ships normal +
roughness as EXR - grab the PNG variants (or convert offline). Terrain layer maps cook to RGBA8
regardless, so 8-bit PNG loses nothing here.

**HDR skies cook to BC6H automatically** (2026-09-07): an imported `.hdr` (usage = HDR) cooks to
BC6H on desktop and desktop-browser targets - 1 byte per texel, GPU-native, no runtime decode -
so a 134 MB RGBA32F sky becomes ~9-11 MB. Compression = None on the asset keeps the raw float
data; mobile-browser targets stay uncompressed until the ASTC-HDR variant lands.

## 3. Sculpt

Sculpting happens in the **scene viewport** (never on the asset pages). Select the **Sculpt
Terrain** tool; the tool panel shows mode, radius, and strength.

- **Modes** (hotkeys `1`-`4`): Raise, Lower, Smooth, Flatten. **Ctrl+click** picks the flatten
  target height from the terrain under the cursor.
- **Radius**: Shift + mouse wheel over the viewport (the panel field tracks it live); the bare
  wheel zooms the camera as usual, brush or no brush.
- **Strength** is in **world meters per second** - it is unbounded because it is measured against
  your scene's height range; a 30 m hill and a 600 m range want different values.
- One undo entry per stroke. Sculpting is disabled during Simulate (the physics collider shares
  the heightfield).
- **Save** persists the sculpted heights back into the heightfield asset and re-cooks. A sculpted
  heightfield becomes *embedded* (the asset's own data is the truth); re-importing its source
  heightmap explicitly resets it.

## 4. Paint

Select the **Paint Splat** tool in the scene viewport. The floating panel shows the base swatch
(labeled - it is not selectable, erase reveals it), one swatch per paint layer (hover for the
asset name), and the **E** slot = the eraser.

- **Hotkeys**: `1`-`9` select paint layers 1-9; `0` selects the eraser. Shift + wheel = radius.
- **Stamp model**: a stamp lands on press and then every fraction of the radius of pointer
  travel. **Strength (0-1) is the coverage a single stamp deposits** - at 1.0 one stamp paints
  the layer fully (and the eraser erases fully); at 0.5 you build up by scrubbing. Holding the
  button still deposits nothing beyond the press stamp unless **Airbrush** is on.
- **Stamp spacing** (fraction of the radius): stamp density along the stroke - low spacing + low
  strength is the smooth-blending detail workflow; high spacing gives discrete dabs.
- **Airbrush**: while held, also deposits stamps at the cursor ~20x/second - build-up by
  hovering.
- Up to **4 layers blend at any texel**; painting a 5th evicts the weakest. Painting one layer
  fades the others (and the base) toward it; fully painting converges to pure layer.
- One undo entry per stroke. **Save** writes the paint into the weights asset (both the weight
  and index rasters) and re-cooks. A painted weights asset becomes *embedded* - a re-cook keeps
  your paint; re-importing its source PNG explicitly resets it.

## 4b. Holes

Cut Holes is the third terrain brush in the scene viewport, beside Sculpt and Paint Splat. Cut
removes the terrain under a hard-edged disc; Fill puts it back. A hole is a set of cut samples,
and the one rule everywhere is that any triangle touching a cut sample is gone: it does not
draw, cast or receive a shadow, collide, count as walkable in the nav bake, or grow grass and
props. A gameplay trace passes through a hole to whatever sits below, which is what lets you
place a cave mesh or a well under it. The brushes that work on the hole itself pick the plane
where the terrain used to be, so Fill lands inside the cut and the prop brush's eraser reaches
props left standing over one. A prop placed before a cut is not deleted by the cut: it stays in
the layer, hidden while its cell is cut, and comes back with Fill; erase it with the prop brush
if the hole is there to stay.

The drawn rim is a smooth curve through the cut samples (the terrain's pixel shaders cut it
against a mask of the hole plane), and it looks the same at every distance. Collision, the nav
bake and raycasts use the stricter rule (any triangle touching a cut sample is gone), so the
visible edge sits up to half a sample inside the collision hole: a character can stand on
that last half-sample of air. Cut at least a few samples wide so the rim has a shape to trace.
One stroke is one undo step. Saving writes the heightfield's `heights` and `holes`
sidecars into the source asset, so a cut survives a re-cook, and every heightfield re-cooks once
after this feature lands because the cooked form carries the second stream. Cubemap-style
overlays on the heightfield page and hole cutting from an imported image are not there yet.

## 5. Cooking and hot reload

The editor cooks in the background after edits and hot-swaps the products into open scenes:
assigning textures, changing tile scales, or adding layers re-cooks the terrain (the palette
texture arrays are rebuilt); painting and sculpting update live and persist on Save. Engine
updates occasionally bump cook versions - the first open after one re-cooks automatically.

## 6. Vegetation

Grass, flowers and rocks grow from the splat you painted. Select the terrain entity and add a
**Terrain Vegetation** component (Terrain category). It has two lists. **Procedural Layers**
hold the ground cover the engine grows from a source (a splat layer, a mask plane, or
everywhere); **Prop Layers** hold the things you place by hand with the Paint Props brush
(rocks, stumps). The add icon appends a layer to its list, and each layer gets its own section
under the list ("Procedural Layers 1: Grass"), titled by the layer's **Name**. A new procedural
layer follows the mask (Placement Mask): it grows nothing until the component's mask has paint
on its plane. A new prop layer holds nothing until you paint into it. Per procedural layer:

- **Mesh** and optionally **Material**: a grass card, a tuft, a rock. Grass cards want a
  double-sided, masked material; the alpha test already runs in the depth, shadow and pick
  passes.
- **Placement** Splat with **Splat Layer** = the palette index of the layer you painted for
  it (the terrain page lists them in order; 0 is the first palette layer). **Splat Threshold**
  is the painted share below which nothing grows; above it, denser paint means denser grass.
  Placement Uniform grows everywhere the slope and height rules allow.
- **Density** in instances per square metre (2 is a full lawn with small tufts; keep it low
  for rocks). A layer denser than **Max Per Chunk** allows scales itself down and logs once.
- **Scale Range**, **Max Slope**, **Height Range**, **Align To Normal**: the per-instance rules.
- **Fade Start / Fade End** in metres from the camera: full density inside the start, nothing
  beyond the end. Thinning is per instance and stable, so nothing pops.
- **Cast Shadows** stays off for grass (the single most expensive thing a grass layer can do);
  turn it on for rocks and props.
- **Visible** hides one layer; the component's own Visible hides them all.

**Painting where a layer grows.** For ground cover the splat does not describe (flowers in a
meadow, a road carved through grass), create a **Vegetation Mask** asset (Create > Terrain >
Vegetation Mask, or Import a PNG as one plane per channel; set **Planes** on its page to the
number of layers that will paint - a later change keeps the planes that still exist), pick it in the
component's **Mask** field, and give the layer **Placement** Mask with **Mask Plane** = its
plane. Select the **Paint Vegetation** tool in the scene viewport: the panel's first row is the plane
the brush works on (keys 1-9), its second the mode - Paint, Erase (key 0) or Smooth (key
minus); erase is per plane, so the plane stays selected while you erase and the status bar
names both, and a "Grows:" line names the procedural layers that read each plane, so you know
what a stroke on plane 0 will grow. Radius, Strength, Stamp spacing and Airbrush work like the splat brush (Stamp
spacing is how often the stroke stamps, not a distance between plants: density is the layer's
**Density**, and **maxInstancesPerChunk** is what one 64-cell chunk can hold - a chunk that fills
stops placing and the log says so once; a painted patch inside a chunk still grows at the full
density). Placement **SplatTimesMask** grows only where BOTH the splat layer and the
mask allow, so erasing the mask along a path cuts a road through splat-driven grass. Strokes
regrow only the chunks under the brush; Save writes the planes back to the mask asset.

**Props.** Rocks, stumps and other placed things are a **Prop Layer**: its instances are placed
by hand rather than grown from a source, with the same mesh, scale, slope, height, fade and
shadow fields as a procedural layer and none of the source fields. Select the **Paint Props**
tool (it refuses with a notice until a prop layer exists), pick the layer in its panel's
first row (keys 1-9) and the mode in its second (Paint, or Erase with key 0 - erase is per
layer, so the layer stays selected while you erase and the status bar names both), and paint: each stamp tries
**Density** props per square metre over the brush disc, keeps the layer's slope, height, scale
and alignment rules, leaves **Spacing** (in multiples of the mesh's size) between props, and
skips any spot inside a physics body of the scene. **Strength** scales how many a stamp places,
so a scrub builds a field up gently. The eraser removes the props under the brush. Every stroke
is one undo step, and the props save with the scene. A layer whose mesh does not resolve (a
deleted or stale asset) draws nothing: the panel marks it "(no mesh)", the status bar says so
while it is selected, and the log warns once.

**Wind.** A layer's material sways when the **Strength** in its **Wind** section (metres at the
card's top) is above zero; set **Speed** (radians per second) and **Height** (the local height, in
the mesh's own units, where the sway is full - roots at or below the mesh origin stay put). Any
material whose shader can sway has the section: PBR, Phong, Unlit and custom shaders (a script or
an override names the values `Wind.Strength`, `Wind.Speed`, `Wind.Height`). Card and
tuft meshes authored with their root at the origin sway naturally; a rock material leaves the
strength at zero and never moves. The sway follows the card into its shadow and its selection.

Reorder layers with the list's arrows and remove one with its cross; every edit is one undo
step. Sculpting or painting regrows only the touched chunks; the chunks build as the camera
reaches them, a few per frame, so a cold scene fills in over the first frames rather than
stalling one.

## Troubleshooting

- **Painting shows white/gray instead of my texture**: hover the layer swatch and check the asset
  name - the classic cause is a `*_disp_*` displacement map picked instead of the `*_diff_*`
  color map. Then check the Console for cook errors and let the re-cook finish.
- **Erasing shows the greenish ramp**: erase reveals the BASE layer; with no base albedo
  assigned, the built-in height/slope ramp is the base. Assign a base albedo on the terrain page
  for a real material.
- **Everything is one flat color at distance**: tile scale too small (texture repeats sub-pixel)
  - raise the layer's tile scale.
- **Normal/ORM map looks wrong on the base layer**: the texture asset's colorSpace is probably
  Srgb - set it to Linear (see the color-space gotcha above).
- **My paint disappeared after re-importing the weights PNG**: that is the contract - re-import
  resets to the file; painted edits live in the asset once you have saved them.
