# Game UI - design history (archived)

> Status: ARCHIVED
> Superseded by: Documentation/Archive/game-ui-history.md
> Track: [[game-ui-subsystem]]

NON-AUTHORITATIVE. The reference survey, the Sedulous.Engine.UI deep-read, and the locked-decision
rationale behind the 2026-07 game-UI build. Present-tense truth is `Systems/game-ui.md`; the full
original design doc (goals, per-tier design, phasing) is in git at the P0 commit 3b92560d. Kept for
the "why".

## Reference survey - conclusions

**Sedulous** (`Engine.UI`): three-tier chain with first-consumer routing - good bones, tier model
kept. World panels = render-to-texture + sprite (one RT + material per panel, no batching). Billboards
= screen-space anchored to projected world points with distance scaling - cheap and good, kept as-is.
Avoid: dual hosts, markup-not-integrated, no assets, `IsMouseOverUI` polling as the only gameplay gate,
world-UI input hand-rolled against the raw mouse.

**Flax** (`UICanvas`/`UIControl`) - the cleanest ECS bridge: a canvas component owning one
retained-mode root; world-space UI as projected direct-draw (widget tree drawn with a wvp transform +
optional depth test), not RT - cheaper, crisper, trivial ray->UI mapping; pluggable `CalculateRay` for
split-screen; `CanvasScaler` per-canvas DPI policy; gamepad nav via explicit focus neighbors +
geometric fallback. Weakness: theme is a global singleton, not an asset.

**Godot** (`Control`/`Viewport`) - the input contract to copy: strict phased dispatch with
`set_input_as_handled`; per-control `mouse_filter` STOP/PASS/IGNORE; `Theme` as a standalone swappable
Resource with variations + per-instance overrides. World-space = SubViewport RT with manual picking -
more boilerplate than Flax; not copied.

**Traktor Spark** - the asset split: source asset -> pipeline -> immutable cooked resource -> factory
-> separate player/renderer; authoring-time parsing stays out of the runtime.

## Sedulous.Engine.UI deep-read (the authoritative reference)

What the doc called "three tiers" is really FOUR over TWO render mechanisms:

| Tier | Sedulous class | Owner | Draw path |
|---|---|---|---|
| 1 Screen overlay (window) | ScreenUIView | subsystem (global) | screen-overlay pass onto the HDR target, after 3D, before blit |
| 2 Per-scene HUD | UISceneModule | per scene | pipeline OverlayPass (after particles, before debug) |
| 3 Billboards | BillboardUIComponentManager | per scene | same OverlayPass - ONE shared root, ONE VG batch for ALL billboards |
| 4 World panels | UIComponentManager + WorldUIPass | per scene | one RT pass PER DIRTY panel -> SpriteComponent w/ MaterialInstance |

Structure copied: the UIContext topology is a TREE (1 global owning the theme/stylesheet + 1/scene HUD
+ 1/scene billboards + 1/world panel; non-global contexts borrow the global stylesheet); two-stage
scene init (construct/inject, then a second pass after the pipeline exists to add the world-UI pass +
register overlays); update-vs-render split; the billboard math (worldPos -> clip -> NDC -> screen px,
behind-camera parks at (-10000,-10000), distance scale clamped); dirty-gated world panels; UI ticks
with UNSCALED dt. Confirmed warts (all avoided): dual input hosts, world-panel input hand-rolled
against the raw mouse, `IsMouseOverUI` polling gate, markup loaders never consumed, no UI assets,
unbatched per-panel RT + sprite.

## Locked decisions (2026-07-18)

1. Core UI only, never the toolkit (toolkit is editor tooling).
2. One `UIContext` owned by the subsystem, theme on the context; a built-in `GameTheme` default; the
   cooked-UITheme story layers on top.
3. Input consumption: actions-only gating, SEPARATE consumption classes (pointer/keyboard/text); raw
   device facades stay unfiltered.
4. Tier shape follows Sedulous EXCEPT input, which is built on our stack (shell event stream ->
   InputSurface/InputRouter -> action layer with the consumption mask). Sedulous input read only to
   catalog what to avoid.
5. Rendering via a registered overlay STAGE (after post), not bespoke player hooks.
6. Script wiring was PARKED at design time (blocked on entity handles); it later shipped as
   id-addressed control access (`setText`/`setProgress`/`setVisible` + `onClick` delegate) on the
   screen tier.
7. `UIDocumentPage` ships an honest v1 (existing multi-line EditText + live preview; no dedicated
   code-editor control yet).
8. World panels done RIGHT or DEFERRED - no stopgap. Resolved 2026-07-19: RT-quad panels (option B)
   ship now; projected direct-draw (option A) becomes a future per-panel MODE. Resolution =
   pixels-per-meter; interactive from day one; unlit/emissive only. Shipped acf506e (EntityOriented
   sprites + ui.WorldPanel + ray-interactive input).
9. The VG renderer is OFF-LIMITS without consultation; game-UI is expected to need no VG changes.

Verified in code (retired from open questions): the `.sml`/`.sss` loaders are fully ported with tests
(MarkupLoader + MarkupRegistry + Styling/Parser with 62 SSS cases + VFS provider); the VG renderer
already routes vertex transforms through a per-slice Float4x4 projection cbuffer, so a world-tier
transform path would be small - but per decision 9 that is a consult-first conversation.
