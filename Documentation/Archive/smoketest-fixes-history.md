# Smoke-test fixes (weekend 2026-08-08 pass)

> ARCHIVED 2026-11-02, obsolete: its subject was replaced or removed. Non-authoritative: the
> original spec, kept as the record of what was built and why; the code and the tests are the truth.

Fixes for the failures the user annotated in docs/smoke-checklist.md. Ordered
by diagnosis confidence: items 1-5 are root-caused (fix as stated), 6-7 need
investigation-first, 8-9 are user-directed (Wren coverage widening; Sandbox
game-UI samples), 10 is checklist bookkeeping. Each lands independently with
tests per CONVENTIONS; user re-runs the matching checklist line to close.

## Status (2026-08-08; hashes updated post-rebase onto the msvc merge)

- 1  Network components in "Other" - DONE (64b8c84f)
- 2  Default UI font row + migration warning - DONE (8ad6c5e6 + f380bef5)
- 3  Character push strength - DONE (959a9a50)
- 4  Character capsule debug draw - INVESTIGATED -> deferred to issues-triage
     I1 (bodies only exist post-Simulate; needs component-data collider draw +
     a Simulate-path ordering fix - larger than a show-flag toggle)
- 5  Collision-groups matrix refresh - DONE (20a0a61b)
- 6  Editor joints live motor - DONE (df49906a: SetJointMotor wakes driven
     bodies; sleeping body ignored the target-velocity edit)
- 7  Collision Shape UX - DONE (8eee6c30 (+5ebe5f44, 159e889f): bespoke CollisionShapeEditorPage,
     typed mesh picker + cook toggle + Cook now + status; plus an inspector
     NoticeEditor warning for shape=Cooked + nil ref). Import-cook FAILURE
     part - FIXED (9f7c6829, diagnosed via 90495015): the real editor cook
     resolves the mesh `reads` edge to the cooked PRODUCT (StaticMeshSource),
     but Build() only Cast<StaticMeshAsset> and failed with "source is not a
     mesh asset". Now cooks from the product (asset fallback for source-db
     paths); regression test feeds the guid a StaticMeshSource as the db holds
     it between cooks. User repro (barrel/block-grass) named the exact cause.
- 8  AS/Wren API gap diagnostic - NOT STARTED
- 9  Sandbox game-UI samples - NOT STARTED
- 10 Checklist bookkeeping - applied to docs/smoke-checklist.md

Remaining work: 8, 9. (7-bug fixed 2026-08-08 after the user's barrel/block-grass repro.)

## 1. Network components land in "Other" in Add Component - DIAGNOSED

`NetworkComponent` and `NetworkedTransform` (Net.Replication/
ReplicationImpl.cpp ~415+) reflect with NO `displayName`/`category`
attributes - exactly the missing-attribute case the component menu punishes
with the "Other" bucket. Fix: `displayName "Network Identity"` /
`"Networked Transform"`, `category "Networking"` (new category - verify the
menu accepts a category with one entry). Standing rule already requires this
on new components; these predate it. Test: the attribute-presence check the
component-menu tests use, extended to these types.

## 2. No "Default UI font" in Project Settings + game UI renders no text - DIAGNOSED

Two symptoms, one root: `ProjectSettings.defaultUiFontId` EXISTS on the wire
(Engine.Project/ProjectModule.cppm ~64, serialized ~106) and the export
manifest carries it, but the Project Settings dialog has NO row for it - so
only manager-scaffolded NEW projects (which seed Roboto) ever have it; the
user's real project has nil -> the game-UI subsystem falls to the dev-tree
font probe, which resolves nothing in a real project -> ui.Canvas documents
render with no glyphs.

Fix:
- SettingsDialog gains "Default UI font" (FontAsset ref picker) in the
  project section, next to defaultSceneId - the same picker pattern the
  loading-screen document row uses.
- MIGRATION KINDNESS: when a project has font assets but a nil
  defaultUiFontId, the Game tab/player logs ONE actionable warning naming
  the setting ("game UI has no default font - set Project Settings >
  Default UI font"), instead of silently rendering nothing.
- VERIFY the .ttf/.otf import path end-to-end in a real project (drop a
  .ttf into the asset browser -> FontAsset + cook). The importer exists
  (Fonts.Importer - since renamed Fonts.Coverage.Baker: it is a baker, not an IFileImporter); the user's note "no ttf/otf importer setup it seems"
  suggests the drop route may not be REGISTERED for those extensions -
  check the drop-import extension table and add if missing.
- Test: settings round-trip for the new row; cook-level: a project with
  defaultUiFontId set renders a ui.Canvas label through the cooked font
  (headless UISubsystem test precedent).

## 3. Character cannot push crates (PhysicsPlayground) - DIAGNOSED (tuning + missing knob)

The character pushes with a fixed ~500N (the CharacterVirtual strength set
at creation) while the playground crates are ~1m boxes at default density -
roughly a tonne each; 500N moves that at ~0.5 m/s^2, i.e. imperceptibly.
Everything else about P3 passed (user note).

Fix:
- Expose `strength` on `CharacterComponent` (authored field, reflected +
  inspector + wire version bump; plumb into the CharacterVirtual at
  create/update).
- Tune the playground: either crate density down or hero strength up so the
  shove reads on screen; the HUD's existing push feedback stays.
- Test: the existing character-walks battery gains a push case - a crate of
  known mass displaced beyond a threshold after N steps of walking into it.

## 4. Character capsule debug draw absent in editor Simulate - DIAGNOSED PATH, verify

`DrawPhysicsDebug` DOES draw characters (two wire spheres + box,
grounded/airborne colors) - but the whole call is gated on the per-scene
physics `Settings().debugDraw` flag (PhysicsSubsystemImpl.cpp ~160). The
RigidBody editor item passed earlier precisely because that user run had
toggled the setting. So this is either discoverability (flag off by
default, buried in scene settings) or a real editor-path break - decide by
testing first:
- Headless/editor test: Simulate with debugDraw ON in scene settings -> the
  scene's DebugScene receives the capsule draws (extract-level assertion,
  the ExtractTests precedent from 2e4db157).
- If that passes (likely), the FIX is discoverability: add "Physics" to the
  scene-viewport show-flags (next to the Post button) that toggles the
  scene's physics debugDraw for THIS viewport session (editor-side state,
  not the authored setting), so the answer to "why can't I see it" is one
  click, not a settings dig.

## 5. Collision-groups matrix does not refresh on Add Group - UI refresh bug

User: "UI doesn't update when added". The matrix view builds its rows from
the group list once; Add Group mutates the list without rebuilding. Fix in
the Physics settings section view: rebuild the matrix grid on any
group-list mutation (add/rename/remove), keeping the one-undo-step-per-click
contract. Test: the settings-section view test adds a group and asserts the
row/column count changed (factor the grid-build into a testable helper if
it is not).

## 6. Editor joints "didn't work" - INVESTIGATE with the user's exact repro

The checklist asked for: (a) entity with RigidBody + Joint (Hinge,
motorEnabled, target nil, no ancestor) -> spins in place on Simulate; (b) a
child of a body entity with a Distance joint hangs from its parent; (c)
motorTargetVelocity live-edit mid-sim. User: "didn't work, give more
specific instructions for the setup."
- FIRST write the walkthrough as a doctest that builds exactly scenes (a)
  and (b) through the editor-facing component API and asserts the motion
  (hinge spins: angular displacement after N steps; distance joint: child
  hangs within the constraint length). Whatever the test finds broken IS
  the fix list - suspects: joint creation ordering vs body creation,
  target-nil handling, the live motor edit not reaching the constraint.
- THEN write the human walkthrough into the checklist item (exact component
  fields, values, and what to expect), so the retest is unambiguous.

## 7. Collision Shape authoring UX + "Generate collision" import cook FAILURE - INVESTIGATE + REDESIGN

Two related notes: the manual CollisionShape asset flow is "bad UX, can't
get it right", and the import dialog's Generate-collision path "cooking
failed" - the good flow is broken and the manual flow is unpleasant.
- BUG FIRST: reproduce the generate-collision cook failure headless (import
  a test GLB with generateCollision on; cook; assert the CollisionShape
  product exists + a RigidBody shape=Cooked resolves it). Fix the failure -
  suspects: the cook reads the mesh product before it exists (cook-order
  dependency) or the generated asset misses a source reference
  post-SourcePath-typing.
- UX SECOND (small redesign, not a rebuild): on the CollisionShape asset
  page/inspector, `sourceMesh` becomes a typed MESH picker (not a raw guid
  row - the Ref-picker dispatch rule), add a "Cook now" affordance and a
  one-line status ("cooked from <mesh> at <time>"); a RigidBody with
  shape=Cooked and a nil ref gets an inspector warning row. Acceptance =
  the user can go mesh -> shape -> simulating collider without touching a
  guid string.

## 8. AS shows far more API than Wren in the browser - EXPLAINED; close the gap UPWARD

(REVISED after user pushback - the first draft had this backwards.) The
browser is truthful: the backends genuinely differ. But the DIRECTION of the
fix is not "limit AngelScript to Wren's set": Wren's reachability closure
exists to work around Wren-side constraints (a bounded emitted set + the
finite foreign-method dispatch pool), while AngelScript registers the whole
registry cheaply - its wider surface is real, working, bound API. Limiting
the capable backend to the constrained one's workaround would throw away
scriptable surface for symmetry's sake.

Do instead:
- **Parity DIAGNOSTIC, not parity enforcement**: a test-time report listing
  the registry types AngelScript binds that Wren's closure misses. Each
  entry is triaged once: script-relevant -> add the missing Wren
  root/edge (the extra-roots API exists); genuinely irrelevant to scripts
  (internal/cook-only types) -> a documented exclusion list.
- **Widen Wren where it matters**: components/facade-adjacent types the
  diagnostic surfaces get emitted (raise the dispatch-pool bound again if
  the closure growth demands it - it went 256->1024 once already; that is
  the knob, not the surface).
- Long-term the gap shrinks toward the documented exclusions; the browser
  keeps reporting per-backend truth, and the [editor]-marker rule stays.
- Test: the diagnostic itself (runs in the script battery, fails on an
  UNTRIAGED delta - new types must either reach Wren or be listed).

## 9. Sandbox game-UI samples - the real gap (REVISED after user pushback)

Sandbox ALREADY renders two views of one scene - the harness exists. What is
missing is that Sandbox exercises no game-UI at all, which is a coverage gap
in its exercise-the-engine role AND what blocks the split-screen HUD
checklist item. Add a UI block to Sandbox (toggleable, like its other
feature demos):
- a scene-tier ui.Canvas HUD (a couple of labels + a button that counts
  clicks - the consumption check),
- a billboard nameplate on one of the scene objects,
- a screen-tier overlay badge (passive, IsHitTestVisible false),
- fed by the same document/font recipe WebScene uses.
With that, the split-screen items run as written in Sandbox's existing
two-view mode: per-half HUD clipping (no bleed across the seam), billboards
projecting per half, screen overlay drawn once over the whole window.
Sample-side work only; no engine code expected - anything that DOES break is
a real per-view UI bug and gets fixed as part of this item.

## 10. Checklist rewrites (no engine fix needed)

- **Browser WebScene build error**: FIXED by d909f708 (verified: build/wasm
  WebScene compiles + links clean today). Item is retest-only.
- **Physics.rayCast/hitSurface/impulseOnHit from game scripts**: the static
  Physics facade was retired (Phase E). The checklist item is superseded:
  retest as `ScenePhysics.of(scene).rayCast(...)` / `.applyImpulse(entity,
  ...)` + `CharacterComponent.of(entity)` - the game-ready-scripting
  surface. Same for the moveCharacter/jumpCharacter item.
- **Billboards in a non-primary view / same scene in two pages**: opening
  one scene in two pages is an editor limitation (by design for now). The
  shipped CAMERA PREVIEW (#118) is a true second view of the same scene -
  retest billboards against the preview inset instead. If that passes, the
  checklist item is satisfied; a second same-scene page remains a separate
  (unscheduled) editor feature.

## Phasing

1-2 first (small, user-facing, unblock font + menu polish), then 3-5
(physics playability), 6-7 (physics authoring - the investigation items), 8
(backend alignment), 9 (harness). Item 10 is checklist bookkeeping only -
already applied to the checklist.
