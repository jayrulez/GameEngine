# Particle authoring + cooked runtime resource

> Status: CURRENT
> Verified: 2026-08-12 @ 9812521b
> Track: [[particles-plan]]

The two tiers that turn the shipped CPU particle runtime (`particles.md`) into an artist-authorable,
cooked-at-build asset: the cooked runtime resource and the editor asset + builder. Implemented; the
bespoke authoring PAGE and LUT curve baking are the deferred pieces.

## Cooked runtime resource (`foundation.particles.resource`)

`ParticleEffectResource` - the cooked runtime input - with its factory + resource-manager registration,
content-DB GUID keying, and a bidirectional serializer. `ParticleEffectComponent` carries an opaque ref
resolved through the standard `resource::Ref` + scene resolve pass, and reloads live: ref resolution +
hot-reload `Proxy`s are done (not deferred), so an edited + recooked effect swaps behind the component.
The runtime path is `Data/Output/ParticleFX/*.rasset` -> load -> render.

## Editor asset + builder (`particles.pipeline`)

`ParticleEffectAsset` (source) + a builder (`Status Build(const editor::Asset&,
editor::AssetBuildContext&)`) that cooks the source effect to `ParticleEffectResource`, registered with
the pipeline registration composition root. The `ParticleFX` sample exercises the full authored ->
cooked -> loaded -> rendered path.

## Editor authoring page (shipped)

`ParticleEffectEditorPage` (`Editor.Particles`) is a full three-pane authoring tool: an authoring TREE
(Effect -> System -> Emitter -> init/behavior modules) driving selection, a LIVE PREVIEW with transport
(Play/Stop/Restart + speed slider + pause) where the effect plays and picks up edits immediately, and a
node INSPECTOR showing the selected node's reflected fields - including CurveCanvas-backed curve editors
(`CurveFieldEditor` over `ParticleCurveFloat`/`Float2`, Hermite tangents). Edits mutate the authored
effect in place with blob-snapshot undo; structural edits (add/remove/reorder a module or system) defer
the tree/inspector rebuild through the UI mutation queue and reselect by identity; Save writes the asset
+ requests a re-cook. Beyond Sedulous: real undo/redo, a live particle-count stats overlay, an
emission-shape gizmo. Covered by `ParticleEffectPageTests`.

## Deferred

- **LUT curve baking.** Over-lifetime behaviors evaluate their curves directly today; baking curves to
  LUTs would mean changing those shipped behaviors, so it is deferred until a measured need (it is
  independent of authoring).
