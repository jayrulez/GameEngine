# Navigation P4b (editor UI): how to consume the viewport-tool seams

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

Status: DESIGN QUESTION for Fable (Opus, 2026-08-18). The navigation runtime + bake
core are built and tested (Archive/navigation-history.md P0-P4a). What remains is the editor UI:
zone gizmo, a Bake action, and navmesh/agent-path debug draw. This is exactly the
consumer the viewport-tool seams were built for (property-animation.md Phase H said
so), so before building I want a ruling on how navigation should plug in.

## What already exists (verified 2026-08-18)

The in-scene authoring seams from the property-animation work, all navigation-ready:

- **`editor.viewporttools` / `IViewportTool`** (Editor.ViewportTools/ViewportTools.cppm:82)
  - a UI-free modal interaction mode: `Id()`, `DisplayName()`, `OnActivate/OnDeactivate`,
  `Update(input)`, `Draw(DebugDraw&)` (built-in debug-draw hook), `StatusText()`.
  Registered via `IViewportToolProvider` -> `ViewportToolProviderRegistry`.
- **`editor.app:tool_panel` / `IViewportToolPanelProvider`** (Editor.App/ToolPanel.cppm)
  - a UI settings panel keyed by tool id, ACTIVATION-scoped, mounted beside the viewport
  while its tool is active. The seam's own doc names the consumers: "property-animation
  authoring, a future terrain-brush panel, **a nav-mesh bake panel**." Registered via
  `ViewportToolPanelRegistry`.
- **`GizmoRendererRegistry` / `IGizmoRenderer`** (Editor.Scene/ComponentGizmos.cppm:41)
  - a per-component gizmo keyed by `ComponentType()`, with `DrawWhenUnselected()`. Box-volume
  precedents already exist: `DecalGizmoRenderer`, `ReflectionProbeGizmoRenderer`.
- **Per-domain registrar precedent**: `RegisterPropertyAnimationEditor(EditorContext&,
  IApplicationHost&)` (Editor.PropertyAnimation/PropertyAnimationEditorModule.cppm:75) is
  where a domain registers its tool provider + panel provider + gizmos at editor startup.

## How P4b maps onto them (proposed)

- **Zone gizmo**: an `IGizmoRenderer` for `NavMeshZoneComponent` drawing the extents box
  (mirror `DecalGizmoRenderer`), registered in `GizmoRendererRegistry`.
- **Bake trigger + params**: a `NavigationTool : IViewportTool` (the mode) + a
  `NavigationToolPanelProvider : IViewportToolPanelProvider` whose panel shows the bake
  profile fields + a "Bake" button that calls the existing `BakeNavigationZone`.
- **Debug draw** (navmesh polys + agent paths): the tool's `Draw(DebugDraw&)` and/or a
  persistent per-scene debug-draw contributor.
- **`RegisterNavigationEditor(context, host)`**: the registrar, wired into the editor app
  beside `RegisterPropertyAnimationEditor`.

## Open decisions (what I need from Fable)

1. **Tool-mode vs one-shot action.** The seam is built around a persistent MODE (terrain
   brush, property-animation authoring). Baking a zone is arguably a ONE-SHOT action on the
   selected zone, not a mode you dwell in. Three shapes:
   - (a) Full `NavigationTool` mode; its panel lists the scene's zones with a per-zone Bake
     button + debug-draw toggles. Baking is done "inside navigation mode."
   - (b) No tool: Bake is an inspector/component-context button on `NavMeshZoneComponent`,
     and debug draw is a persistent view toggle. The tool_panel seam goes unused by nav.
   - (c) Hybrid: a lightweight "Navigation" mode that exists mainly to host the panel + turn
     on debug draw; the Bake button lives in the panel.
   Which matches the intent when we said "nav-mesh bake panel"? My lean: (c) - it uses the
   seam as designed and keeps the bake discoverable, without pretending baking is a dwell-mode.

2. **UI-capable editor lib layering.** `Editor.Navigation` is currently UI-free (the bake
   core, linking engine.render/geometry). The tool/panel/gizmo build `ui::View`s and link
   editor.viewporttools + editor.app + editor.scene(gizmo). Add them to `Editor.Navigation`
   (make it UI-capable), or split a UI-free core from an `Editor.Navigation.UI`? The
   pipeline-UI-free rule is about Pipeline targets, not editor libs - is there an editor-tier
   convention here, or does property-animation's single UI-capable editor lib set the pattern?

3. **Debug-draw scope.** Tool `Draw()` is activation-scoped (only while nav mode is active).
   Navmesh + path visualization usually wants to PERSIST regardless of the active tool (like
   collider debug draw). Should nav debug draw be tool-scoped `Draw()`, or a persistent
   per-scene debug-draw contributor with its own view toggle? (Leaning persistent + toggle.)

4. **Async bake.** `BakeNavigationZone` is synchronous today. The spec says the editor bake
   runs async on a worker with progress. For small zones it is instant. Build the job-system
   + progress wrapper now, or defer async until a real large-zone bake actually stutters?
   (Leaning defer - keep P4b synchronous, add async when measured.)

5. **Gizmo interactivity.** Read-only extents box (like Decal), or interactive resize handles
   (like `TransformGizmo`)? Read-only is far cheaper and the extents are also inspector-editable.
   (Leaning read-only for P4b.)

6. **Navmesh debug geometry accessor.** The runtime `NavigationMesh` has no
   triangle-extraction accessor. For navmesh debug draw I would add
   `NavigationMesh::DebugTriangles(Array<Float3>&)` (walk the dt tile polys) in
   foundation.navigation. Confirm that is the right layer (vs caching a debug outline in the
   cooked `NavigationZoneSource` at bake time, the collision-asset precedent).

## Not blocked

None of this blocks the runtime - agents already navigate (P3). This is purely the authoring
surface, and every piece has a clear seam to land on; I only need the shape decisions above
before building so I do not build the wrong shell.

## FABLE RULINGS (2026-08-18) - build to these

First, one stale premise to correct: **the tool_panel seam is PARKED with ZERO
consumers.** Property animation built it (Phase H) and then moved OFF it to the
persistent BottomDock panel (A2 REVISED, user-directed 2026-08-17); the "nav-mesh
bake panel" line in the seam's own doc predates that move. Nav should not
resurrect a parked seam out of loyalty to a stale comment - it should follow the
pattern that actually won. That reshapes question 1.

1. **Shape: (b), not (c).** Baking is a ONE-SHOT asset-writing action on a
   selected component (BakeNavigationZone writes the NavigationZoneAsset
   sidecar the component references). There is no dwell-mode interaction in
   P4b: no brushing, no click-in-viewport authoring (extents are
   inspector-edited, the gizmo is read-only per ruling 5). A mode whose only
   job is hosting a button is ceremony. Concretely:
   - The Bake button lives in the INSPECTOR on the NavMeshZoneComponent
     section. The inspector has no generic per-component action-button seam
     yet - add a SMALL one (a per-type action-row dispatch, the same
     table idiom the ref pickers use). That seam will be reused (probe
     re-bake, effect preview, ...) and is far cheaper than reviving the tool
     ecosystem. It shows bake status/result inline (flash the outcome - the
     Save-button lesson: no silent success).
   - NO NavigationTool, NO panel provider in P4b. The seam stays parked. The
     REAL future trigger for a NavigationTool is genuinely modal authoring -
     off-mesh link placement or walkable-area painting. When that lands, the
     tool + panel shape is right; record that here and build it then.
   - `RegisterNavigationEditor(context, host)` still exists as the per-domain
     registrar (gizmo + inspector action registration) beside
     RegisterPropertyAnimationEditor.

2. **Layering: ONE UI-capable Editor.Navigation.** The pipeline-UI-free rule is
   about Pipeline targets and their tests, deliberately; editor libs are the
   UI-capable tier and property-animation's single-lib shape is the pattern.
   Do not split an Editor.Navigation.UI - no other editor domain has one.
   Keep the bake-core FUNCTIONS in implementation units that import no ui
   modules so headless consumers (tests, a future CLI bake) stay clean; the
   linker prunes the rest.

3. **Debug draw: persistent, engine-side, settings-gated - the PHYSICS
   precedent exactly.** Physics debug draw is gated by a per-scene settings
   flag (PhysicsSceneSettings.debugDraw) and drawn by the runtime system into
   DebugScene each frame - working in editor AND player, independent of any
   editor tool. Navigation mirrors it: a debugDraw flag on the navigation
   scene settings, drawn by the navigation scene system (navmesh polys;
   agent paths under the same flag, or a second field if the noise annoys).
   Tool-scoped Draw() is wrong here - the navmesh would vanish the moment you
   click the move gizmo. This also means the debug draw lives in the ENGINE
   navigation system, not the editor lib; the editor merely toggles the
   setting (scene-settings inspector gets it for free as a reflected field).

4. **Async bake: defer, RECORDED.** Keep P4b synchronous; note the deferral in
   the nav backlog (week-2026-09-05) with its trigger ("go async when a measured bake stutters -
   roughly >100ms on a real zone"), so it is a deliberate deferral, not a
   silent scope-down. Structure the call site as one function (collect ->
   bake -> write -> flash outcome) so moving it onto a worker later is
   mechanical. Do not build the progress plumbing speculatively.

5. **Gizmo: read-only extents box**, mirror DecalGizmoRenderer,
   DrawWhenUnselected() = false. Interactive box handles are a SHARED problem
   (decals, probes, nav zones all want the same thing) - if handles ever get
   built, build ONE box-extents-handles helper for all three, never a
   nav-specific one. Recorded so nobody builds bespoke handles.

6. **DebugTriangles on NavigationMesh (foundation.navigation): yes, and
   PREFER it over caching an outline in the cooked asset.** The debug draw's
   job is to show what the runtime is ACTUALLY pathing on - the loaded
   dtNavMesh - so drawing from the live mesh catches load/version/transform
   drift that a bake-time outline would mask. The collision-asset outline
   precedent exists because cooked Jolt blobs do not enumerate cheaply at
   runtime; dt tiles DO. Append-into-caller-array signature, Detour-only
   internals (PIMPL holds). Test: baked zone yields >0 triangles, all inside
   the zone AABB inflated by one cell.

Not a ruling, a reminder: P4b lands with tests per phase (the gizmo +
inspector action are testable headless via the registries; DebugTriangles in
foundation tests) and the plan's on-screen verify stays a user step.

## FOLLOW-UP QUESTION for Fable (Opus, 2026-08-18) - P4b-3 seam reality

P4b-1/2 shipped to your rulings (55a859d9: DebugTriangles + settings-gated engine
debug draw, both compilers, Player links). Building P4b-3 (gizmo + inspector Bake
button) surfaced a premise your ruling assumed that does NOT hold - please re-rule.

**Finding (verified in code):** the editor gizmo + inspector are NOT
plugin-extensible today. There are no registries for a domain lib to register into:

- **Gizmos**: `GizmoRendererRegistry` is instantiated PER ScenePage (`m_componentGizmos`)
  and populated by `RegisterBuiltinGizmoRenderers(registry)` (ComponentGizmos.cppm),
  called from ScenePage.cppm:188. Every built-in renderer (Light, Decal, ReflectionProbe,
  Camera) is added there, inside Editor.Scene. There is no seam for an external lib.
- **Inspector**: per-component-type rendering is a CENTRAL hardcoded `if (type ==
  &TypeOf<X>())` chain in Editor.Scene/InspectorViewImpl.cpp (PhysicsSceneSettings ~532,
  RigidBodyComponent ~964, the Ref pickers, ...). No per-type action registry exists.

So your ruling 1 ("Editor.Navigation registers the gizmo + a generic inspector
action-row seam, so Editor.Scene does not depend on nav") and the "reminder"
("testable via the registries") both assume registries that are not there yet. Two
honest paths:

- **(A) Build the seams you specified.** Add two small registries Editor.Scene
  consults: an inspector action-row registry keyed by component type (the reusable
  seam - probe re-bake, effect preview), and an extra-gizmo-renderer hook so
  ScenePage adds domain renderers after the built-ins. Editor.Navigation registers
  into both; Editor.Scene stays nav-free. More infra, touches central Editor.Scene
  files, but matches your intent and is reusable + headless-testable.
- **(B) Follow the pattern that exists.** Put the nav zone gizmo in
  RegisterBuiltinGizmoRenderers and the Bake button in the central InspectorView
  dispatch, coupling Editor.Scene -> Engine::Navigation exactly as physics/decal
  already are. Smaller, consistent with today's code, but no reusable seam and
  Editor.Scene grows another domain dependency.

**Question:** (A) or (B)? You ruled (A) not knowing the dispatch is centralized; (B)
is what the codebase does today for every existing component. If (A), confirm the two
registries are worth adding now for one consumer (the reuse case is real but future).
If (B), the coupling is the same shape Editor.Scene already has for physics/decal.

Nothing blocks on this - the runtime + debug draw are done; this is only the bake
BUTTON + gizmo placement. Holding P4b-3 for the ruling.

## FABLE RE-RULING (2026-08-18) - (B), and the seam promotion rule

Good catch: ruling 1 assumed extension registries that do not exist, and the
"testable via the registries" reminder inherited the same error. Corrected
ruling: **(B) - follow the pattern that exists.** Three reasons, strongest
first:

1. **Central explicit dispatch IS this codebase's architecture, not an
   accident.** The inspector ref-picker table, Pipeline.Registration,
   Engine.SceneSurface, Engine.ScriptSurface - the house style is composition
   roots with explicit per-type entries and count tripwires, chosen over
   plugin discovery on purpose. The InspectorView if-chain and
   RegisterBuiltinGizmoRenderers are that style. Nav should join it, not
   fork it.
2. **The freshest lesson in this repo is that seams built for ONE consumer
   rot.** The tool_panel seam was built for property animation, which then
   moved to the BottomDock; the seam is parked with zero consumers and this
   very doc nearly resurrected it from a stale comment. Do not manufacture a
   second one. A registry earns its existence with its SECOND consumer.
3. **The coupling in (B) is the exact shape Editor.Scene already has** -
   InspectorViewImpl imports engine.physics directly for the RigidBody
   dispatch; the gizmo builtins couple to decal/probe/camera. Editor.Scene ->
   Engine.Navigation (component type) + Editor.Navigation (BakeNavigationZone)
   is acyclic (Editor.Navigation does not import editor.scene) and
   precedent-identical.

Mechanics for P4b-3 under (B):

- Zone gizmo: add to `RegisterBuiltinGizmoRenderers` mirroring
  DecalGizmoRenderer (read-only extents, DrawWhenUnselected false - ruling 5
  stands).
- Bake button: a `NavMeshZoneComponent` entry in the central InspectorView
  dispatch, calling BakeNavigationZone directly; outcome flashed inline
  (ruling 1's no-silent-success requirement stands). Direct imports, matching
  the physics entry.
- If nothing is left for `RegisterNavigationEditor` to register, DROP the
  registrar rather than shipping a hollow one. (Rulings 2/3/4/6 are
  unaffected; debug draw already landed engine-side.)
- Tests ride Editor.Scene.Tests' existing inspector/gizmo harnesses, not a
  registry.

**Promotion rule (recorded so the seam question does not re-litigate):** when a
SECOND per-component inspector action arrives (reflection-probe re-bake is the
likely one), THAT change extracts the by-then-two central entries into the
action-row registry ruling 1 described - designed against two real consumers
instead of one imagined one. Same rule for an external gizmo-renderer hook:
second out-of-tree gizmo pays for it.

## USER RULING (2026-08-18) - the seams are scheduled, superseding the trigger

The user has ruled the editor extensibility seams are something we "ultimately
must fix": the promotion rule above is superseded AS A TRIGGER - the work is
SCHEDULED into the week of 2026-08-22 (Documentation/Plans/week-2026-08-22.md,
seeded item). P4b-3 still lands as (B) now (nothing blocks); next week's work
builds the action-row + gizmo registration seams and migrates nav's entries
onto them, at which point Editor.Scene drops its nav imports and
RegisterNavigationEditor returns as the domain registrar. The design guidance
in the re-ruling (house composition-root style, tripwires, headless tests,
validate the seam shape against the real consumers) carries over.
