# Draconic build specs

Specs for the current backlog, written to be built by an implementing agent
(Opus) with review-only oversight. Each spec is self-contained: context, exact
scope, files to touch, phased plan, acceptance criteria, and known gotchas.

Read CONVENTIONS.md FIRST - it is binding for every spec in this folder.
HANDOFF.md records the review baseline commit (everything after it is the
implementing agent's work).

## Index (rough priority order)

| Spec | Task | Size |
|---|---|---|
| [game-ready-scripting.md](game-ready-scripting.md) | - | L (closed 2026-08-08) |
| [game-ready-scripting2.md](game-ready-scripting2.md) | - | M |
| [smoketest-fixes.md](smoketest-fixes.md) | - | M (10 items) |
| [issues-triage.md](issues-triage.md) | - | M (10 issues) |
| [mcp-agent-access.md](mcp-agent-access.md) | - | L (P0 building) |
| [luau-backend.md](luau-backend.md) | - | L |
| [async-resource-loading.md](async-resource-loading.md) | #123 | L |
| [web-remainder.md](web-remainder.md) | #112 | L |
| [reflection-track.md](reflection-track.md) | #110 | L |
| [editor-polish.md](editor-polish.md) | - | M |
| [source-path-p3-p4.md](source-path-p3-p4.md) | - | S |
| [vg-quality-leftovers.md](vg-quality-leftovers.md) | #121 leftovers | M |
| [msaa.md](msaa.md) | I11 | M (P1 built; P1g open) |
| [property-animation.md](property-animation.md) | #129 | L |
| [asset-variants.md](asset-variants.md) | #133 | L (variants axis + texture compression) |
| [mesh-lod.md](mesh-lod.md) | - | M (spec prepared, not scheduled) |
| [terrain-vegetation.md](terrain-vegetation.md) | - | L (P0 splat-driven grass BUILT 2026-09-21; P1 mask + wind and P2 prop scatter not scheduled) |
| [ui-theme-migration.md](ui-theme-migration.md) | #135 | L (P0 shipped; P1-P4 phased) |
| [ui-layout-and-style-model.md](ui-layout-and-style-model.md) | - | XL (PROPOSED: uniform LayoutStyle, real cascade, box model, transitions; P0-P4) |
| [ui-browser-parity-tests.md](ui-browser-parity-tests.md) | - | M (PROPOSED: measured Chrome expectations for layout, not derived ones) |
| [ui-text-and-gaps.md](ui-text-and-gaps.md) | - | L (PROPOSED: shaping + inline runs; the UI gap list vs RmlUi/MewUI) |
| [whiteboxing.md](whiteboxing.md) | - | M (PROPOSED: parametric blockout pieces driving mesh + collider; P0-P3) |
| [scene-format-reference.md](scene-format-reference.md) | - | M (RULED 2026-09-27, ready to build: the worked example scene and the component/settings wire schema, recorded from the Serialize bodies, generated at run time and served live by both hosts with component_schema; the reference hop declared on the runtime factory; pregeneration and the golden later) |
| [script-run-modules.md](script-run-modules.md) | - | M (PROPOSED: one script module per run shared by Game, Levels and behaviours; hot reload that migrates live state; Luau loads only what changed; P0-P2) |
| [editor-actions.md](editor-actions.md) | - | L (BUILT through layer 6: one action declaration behind menus, shortcuts, toolbars, the palette and the MCP action bridge; layer 7 PROPOSED: the asset browser over the published asset selection, the page leftovers, the page toolbar roll-out) |
| [scene-authoring-tools.md](scene-authoring-tools.md) | - | L (PROPOSED 2026-09-28: structured scene operations for agents - one vocabulary, a headless `scene_edit` over the file and, needing a ruling that reverses mcp-agent-access's "structural live ops NOT planned", a live `page_edit` as one undo step; lists and scripts by name; builds on editor-actions layer 7) |
| [render-textures.md](render-textures.md) | - | L (PROPOSED 2026-10-01, building on branch `render-textures`: render texture assets, cameras that render into them, orthographic cameras, texture images in game UI; P0-P4; motivated by the PaperKid minimap) |
| [editor-lists-and-asset-slots.md](editor-lists-and-asset-slots.md) | - | L (PROPOSED 2026-09-28: one list widget (ContainerListEditor with element bodies, an add menu and drop) for every editable list, every asset reference an AssetPickerSlot that accepts drops, one entity picker with hierarchy drags, the generic page reflection-first; folds in reflection-track's REMAINING 1 and 2) |
| [2d-games.md](2d-games.md) | - | L (PROPOSED: 2D inside the 3D world - sprite fixes, an orthographic camera and sort layers, sprite sheets, a plane lock on Jolt, a 2D sample, then tilemaps and the editor tooling; prior art Zero + ezEngine) |
| [paperkid.md](paperkid.md) | - | game plan |
| [snowline.md](snowline.md) | - | game plan (PLAN 2026-10-04: Snowline, the third demo, a downhill snowboard time trial with tricks on terrain, vegetation, splines, an animation graph, decals and joints) |
| [inverse-kinematics.md](inverse-kinematics.md) | - | L (PROPOSED 2026-10-05: two-bone and aim solvers, foot placement, an ordered pose-modifier stage, components, scripts, debug draw; "it must be solid") |
| [root-motion.md](root-motion.md) | - | L (PROPOSED 2026-10-05: per-clip root settings, a cooked motion curve with the pose stripped in place, deltas split at the loop wrap and blended like poses, applied to an entity, a character or a script) |
| [documentation-system.md](documentation-system.md) | - | process |
| [scene-prefab-unification.md](scene-prefab-unification.md) | - | WIP design question (needs Fable) |
| [navigation-editor-ui.md](navigation-editor-ui.md) | - | design question (needs Fable): nav P4b editor UI |
| [deferred-by-design.md](deferred-by-design.md) | - | note only |

> Fully-built specs are archived at `../Archive/<name>-history.md` and dropped from this index.

Sizes: S = a session or less, M = a few sessions, L = a multi-session track
that should land in phases with a green build after each phase.
