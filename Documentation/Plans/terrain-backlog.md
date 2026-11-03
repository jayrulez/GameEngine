# Terrain backlog - the follow-ups that outlived the track

> The terrain track is COMPLETE; its eight docs moved to Documentation/Archive/
> (terrain-history.md is the track spec, *-phase2 / *-topk / *-layer-pbr /
> *-height-blend / *-coverage-mask carry the sub-track rulings). This doc is the
> one live list of everything deliberately left on the table, each with its
> trigger. Several graduated into week-2026-09-05 seeds - this list is the
> terrain-side index, the seeds are the work orders.

## Seeded for week 2026-09-05 (full write-ups live in that doc)

- **CDLOD morph** - per-chunk geo-mip seams are hidden by skirts today; CDLOD
  vertex morphing is the proper fix. Proper cross-chunk stitching rides this.
- **Erosion + hole cutting** - hydraulic/erosion brushes (the Traktor
  heightfield extras) and holed cells. SEQUENCING RULE: holes must EXCLUDE
  their cells from the nav bake (nav-terrain integration shipped 2026-08-31 -
  CollectNavigationGeometry triangulates the heightfield; holes subtract there).
  HOLES BUILT 2026-09-23 (P0 to P2; its seeds are in weekly_backlog.md): `Archive/terrain-holes-history.md` (a per-SAMPLE flag
  plane on the heightfield, Jolt's own rule everywhere, holed chunks draw their
  own index buffers, a `terrain.hole` brush); erosion stays its own seed.
- **Grass / vegetation** - phase-3; renders through the instanced-mesh path
  with per-layer distance/density rules, NOT mesh-LOD chains (a terrain-wide
  set's aggregate bounds degenerate per-set selection; per-instance bucketing
  is the mesh-LOD deferral it would trigger).
- **Large-world paging** - streaming multiple terrains / sector paging.

## Smaller items (no seed; build when touched)

- **Eraser + smooth-weights splat brush modes** - BOTH DONE. Eraser shipped
  with top-K splat (hotkey 0, EraseTopK fades painted weights to reveal the
  base). Smooth shipped 2026-08-31 (hotkey minus / the panel's "S" slot,
  SmoothTopK blurs each texel toward its 3x3 neighborhood average - feathers
  a painted seam without repainting either side).
- **Derived-texture bakes** - normal/occlusion maps generated FROM the
  heightfield (Traktor feature); pipeline-side, rides no other work.
- **Coverage-mask R4** - mip softening of stencil masks accepted as-is;
  the real fix is coverage-preserving mip scaling, which is the same math as
  the alpha-coverage-mips seed (week-2026-09-05) - build them together.
  POM + per-layer amplitude deferred with it.
- **Top-K extension points** (recorded in terrain-splat-topk-history.md):
  K > 4 blended layers per texel (one-constant change), 16-bit palette
  indices (> 256 layers), bindless albedo binding. None has a driver yet.
- **Triplanar projection per layer** - the one per-layer material feature
  not shipped (PBR/height/mask per layer all landed); steep-slope stretching
  is the trigger.
- **Terrain asset thumbnail** - the browser tile is the type icon; the page's
  3D orbit preview is the natural GPU-stage generator when someone wants it
  (Archive/asset-thumbnails-history.md records the ruling: heightfield carries the visual
  identity meanwhile).

## Done since the track closed (so nobody re-plans them)

- Terrain <-> navigation: heightfields feed the zone bake (2026-08-31,
  0c05ad89; flat/ramp path + cliff-splits tests).
- Sculpt re-cook persistence: the persist closure clears fileName and writes
  the "heights" sidecar; the builder treats the empty fileName as
  sidecar-authoritative. Verified in-tree 2026-09-01.
- Heightfield + splatmap browser thumbnails (grayscale relief / false-color
  weights, 2026-08-31).
