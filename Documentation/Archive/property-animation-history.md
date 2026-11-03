# Property animation

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

**Status:** BUILT 2026-08-16 (all 7 phases + H1-H4) - REVIEW PASS 10 ruled
CONDITIONAL: the required-fix list below must land before the track closes
(full findings in Documentation/Archive/handoff-history.md pass 10). First of the
three parity P0 tracks (this, then navigation (Archive/navigation-history.md), then terrain (Archive/terrain-history.md) - see
docs/design/parity-2026-08.md).

## REVIEW PASS 10 - REQUIRED FIXES (Opus, in priority order)

1. **Phase G surface is dead.** PropertyAnimatorComponent is missing from
   `RegisterAnimationScriptFacade`'s `components[]`
   (AnimationSubsystemImpl.cpp:143) - never registered into
   GlobalTypeRegistry, never seeded as a script root, never named for the
   prelude. Add it (facade name count bumps) + a test that asserts the
   REFLECTED method table (the current Phase G test calls the C++ methods
   directly and would pass with every `.Method<>` line deleted).
2. **Empty-channel zeroing.** `PropertyTrack::Sample` evaluates every
   channel; an empty `Curve` returns 0, and the editor's "+ Track" creates
   all-empty channels - the first tick teleports the target to origin.
   Give channels per-channel active semantics (empty channel = do not
   write that component) or read-modify-write. Editor side of the same
   class: canvas key-add seeds sibling channels from `DefaultValue` (0) -
   seed from the channel's curve value at t instead.
3. **Kind-vs-type mismatch is a silent no-op.** BuildBindings never checks
   `track.kind` against the leaf property type and the WriteBinding status
   is discarded - a mismatched track fails every frame with no warning.
   Extend the disable-once-with-warning policy to cover it.
4. **In-scene preview cannot preview Transform tracks.** The preview apply
   loop resolves via component managers only; the runtime's
   `kTransformComponentName` special case was dropped in the fork - the
   DEFAULT track type ("+ Track" = Transform/position) previews as a
   silent no-op and the H4 overlay marker never draws. Share the runtime
   apply path or port the Transform branch (preview + snapshot + marker +
   AddTracksFromSelection).
5. **Undo use-after-free.** `ClipEditCommand` stores `ClipEditorView*` and
   the commands outlive the activation-scoped view on the scene page's
   durable stack (panel Sync destroys the view; undo after tool-deactivate
   dereferences it). Same class: the asset-picker `OnPicked` captures the
   panel raw. Commands must not point at recreatable views (route through
   the tool, which owns the clip), and the picker callback needs a
   lifetime guard.
6. **Preview snapshot staleness.** Snapshot is captured once per entity;
   adding/retyping tracks mid-preview then scrubbing writes properties the
   snapshot does not cover - StopPreview leaves the scene modified.
   Re-snapshot on track-identity change (or snapshot per-track lazily).
7. **Smaller, same batch:** `AddTracksFromSelection` missing `LockGroup()`
   (consecutive clicks coalesce into one undo entry); `clip.loop` is dead
   wire (component `loopMode` is the only truth - delete the field or make
   it the default `loopMode` seeds from); declare `Version()` on
   PropertyAnimationClipAssetBuilder; `WriteBackTrack` flattens per-key
   interpolation track-wide (document or preserve); wire hardening -
   range-guard `trackKind` (out-of-range currently desyncs ALL later
   channel cursors) and default truncated `keyInterp` to Linear (struct
   default), not 0/Constant.
8. **Test debt:** PingPong loop (incl. the guard bail), negative speed,
   component Serialize round-trip, the spec's physics-gotcha test, and the
   four canvas seam properties (Constant staircase, no-edit round-trip
   byte-identity, one-undo-step-per-gesture, MaxKeys) - all currently
   unguarded.

### UX observations - fold into the UI/UX rebuild (user-directed 2026-08-16)

The user has commissioned a UI/UX rebuild of this editor. Review pass 10
observations that belong in it rather than as patches to the old layout:
- The transport scrub is an EditableLabel (type a time + Enter) - live
  preview reads step-wise. A draggable timeline/slider is the expected
  interaction; keep the field as the precise-entry companion, and refresh
  it from the scrub time outside rebuilds (it currently goes stale).
- `RefreshPreview` re-formats every track's summary string on every
  write-back (every mouse-move of a drag) - rebuild should make row
  refresh incremental.
- The track kind-cycle button (Float->Float3->Color->Quat) leaves stale
  channel/quat data behind (Float->Float3 yields y=z=0; ->Quat orphans
  the scalar curves). The rebuild should either convert sensibly or
  confirm-and-clear.
- LinkedTime assumes aligned channel key lists but nothing enforces it -
  a Float3 track with differing per-channel key counts edits oddly
  (ReSortLinked skips mismatched channels). Decide: enforce alignment on
  write-back, or make the canvas handle ragged channels.
- Keys past the editable Length normalize past 1.0 and become invisible
  and unreachable; Length is a non-undoable, non-persisted view scale.
  The rebuild should make out-of-range keys visible (clamp-to-edge
  indicator) or auto-grow Length to the clip.
- Preview never dirties the document (correct), but saving the SCENE
  mid-preview persists previewed values silently - stop preview on scene
  save.
- REQUIRED-fix items 4-6 above (Transform preview, undo lifetime,
  snapshot staleness) are structural: fold them into the rebuild's
  design rather than patching the old panel first. Prior art: Lumix `property_animator`
(reflected float curves); ours is multi-type from day one (user decision)
because Variant + reflection make it nearly free and transform animation
(Float3/Quat) is the number one use case.

## BUILD STATUS (2026-08-16)

All phases shipped, green on clang + gcc:
- **A** foundation.core:curve - scalar keyframe curve (Constant/Linear/Cubic
  Hermite). 36 assertions.
- **B** foundation.propertyanimation - TrackValueKind{Float,Float3,Quat,Color},
  PropertyTrack (per-component Curves + slerped QuatKeys), Sample->Variant, the
  reflection binding resolver (nested paths, live-re-walk write). 37 assertions.
- **C** foundation.propertyanimation.resource - cooked clip (flat SoA source +
  FromClip/FillClip, bounds-guarded) + factory (model-A). 54 assertions.
- **D** propertyanimation.pipeline - PropertyAnimationClipAsset embeds the source
  + builder; registered in Pipeline::Registration (kBuilderCount 21->22). Cook
  round-trip + tripwire tests.
- **E** engine.animation:propertyanimator - PropertyAnimatorComponent + per-scene
  manager (injected by AnimationSubsystem, ticks PostUpdate): Once/Loop/PingPong
  clock, per-track binding built once (component-by-type-NAME + property chain),
  live re-resolve write, failed-track disable-with-one-warning. 43 assertions.
- **G** scripting - reflected play/stop/pause/resume/isPlaying/time/setTime via
  the existing `.of(entity)` surface (no facade lib). 52 assertions.
- **F** editor.propertyanimation - the P1 clip page (track list + keyframe table +
  transport scrub + toolbar undo) + New Asset creator, wired into the editor exe.
- **H** in-scene authoring mode (Phase H below, per Fable's rulings). All four
  steps shipped, green clang + gcc:
  - **H1** `editor.app:tool_panel` - the panel seam (IViewportToolPanelProvider
    keyed by tool id + ViewportToolPanelRegistry + the ViewportToolPanelHost mount
    controller), NOT on the UI-free IViewportTool; ScenePage reserves a left-of-
    viewport rail and docks/undocks on tool activation. 103 assertions.
  - **H2** ClipEditorView - the track/curve/transport widgets lifted out of the page
    into a shared view over IClipEditorHost (clip + command stack + dirty + scrub
    hook); the page is now a thin host, behavior-identical. 15 assertions.
  - **H3** PropertyAnimationTool (editor.propertyanimation:tool) - the viewport tool
    AND clip-editor host; docked panel with New/Pick/+From-Selection/Save chrome;
    add-from-selection walks the selected entity's reflected components
    (InferTrackKind + CollectAnimatableProperties, ResolveBinding-validated) as one
    undo group. Panel seam extended to pass the active tool for the id-guarded
    downcast. 44 assertions.
  - **H4** live preview - the scrub drives the runtime WriteBinding path onto the
    selected entity through a transient snapshot (foundation gained the symmetric
    ReadBinding); restored on stop/deactivate/Simulate; never dirties the document
    or hits undo; overlay marker at the previewed entity. 63 assertions total in the
    editor tool suite.

REMAINING (follow-ups, not blocking the feature):
- Live preview from the STANDALONE clip page (the in-scene tool now does live
  entity preview; the page's scrub still only shows sampled values - it has no
  scene selection).
- The physics-kinematic-rule TEST (documented in the component; the cross-subsystem
  test rides the physics manager).
- Acceptance UAT: the demo scene (animated position + light color, looping) in
  play-in-editor AND the exported player + a user visual pass.

CORRECTION 2026-08-16: the "P2 = BUILD the curve-editor widget" framing below was
STALE - the widget ALREADY EXISTED (`Foundation::UI.Toolkit CurveCanvas`, ported
from Sedulous, tested, used by the particle page). DONE 2026-08-16: the clip page
now hosts a CurveCanvas per scalar track (Float/Float3/Color); Quat keeps the xyzw
key table. The mapping seam (canvas Time is [0,1], interpolation is per-channel
Hermite/Linear/Step) resolved via: keys normalize by an editable clip Length
(tangents rescale with it), a per-track interp cycle maps {Cubic/Linear/Constant}
<-> {Hermite/Linear/Step} and applies to all channels/keys, edits write back live
with one undo per gesture, MaxKeys 8 -> 64. So P1 shipped WITH a curve canvas.
Remaining canvas niceties (true per-KEY interp modes, box-select multi-key ops)
are optional polish, not blocking.

GOAL: animate ANY reflected property on ANY component with keyframe curves,
as data (a clip asset) driven by a component - no code per animated thing.
This is also the substrate for a later sequencer/cutscene tier (Traktor
Theater shape: acts of property tracks over time) - design nothing that
blocks that, build none of it now.

## Module layout (the library shape)

NOT an extension of Foundation/Animation - that module is skeletal (bones,
poses, skinning deps). Property animation depends on foundation.core
reflection ONLY, and that independence is the point: headless consumers
(editor page, tests, MCP hosts) must never pull skeletal machinery to
evaluate a property clip. Dotted-suffix naming would lie about the
dependency (Animation.Property reads as "part of Animation"), so it gets
its own name. The full triad, mirroring the Particles/Physics pattern:

- `Code/Foundation/PropertyAnimation` - module
  `foundation.propertyanimation`, alias `Foundation::PropertyAnimation`.
  The clip/track/curve data model, evaluation, and the reflection binding
  resolver (resolves against TypeRegistry/Instance - no scene dependency).
  Links Foundation::Core only.
- `Code/Foundation/PropertyAnimation.Resource` - module
  `foundation.propertyanimation.resource`. The cooked runtime resource +
  loader (model-A pattern, same as Animation.Resource/Texture.Resource).
- `Code/Pipeline/PropertyAnimation.Pipeline` - the source asset
  (PropertyAnimationClipAsset, Pipeline domain), XML->binary builder, New
  Asset creator. Registered via Pipeline::Registration (bump kBuilderCount
  + tripwire).
- Engine side: EXTEND `Engine/Engine.Animation` - PropertyAnimatorComponent
  + PropertyAnimatorComponentManager (house pattern: scene::
  SerializableComponentManager<T> IS the per-scene system - storage +
  lifecycle + serialization + the playback tick in one class, like
  SkeletalAnimationComponentManager), injected into scenes by the existing
  AnimationSubsystem; the DefaultApp/registration surface is unchanged.
  Engine-side coupling is acceptable where foundation-side coupling was
  not: a game with the animation subsystem wants both.
- `Code/Editor/Editor.PropertyAnimation` - the clip page (per-domain editor
  lib precedent: Editor.Physics, Editor.Audio, Editor.Input).
- Curve math (Sedulous Curve/CurveKey port) lands in foundation.core Math -
  it is general math per the gap policy, shared with any future consumer
  (skeletal can migrate to it later); NOT private to this module.
- Tests: PropertyAnimation.Tests + PropertyAnimation.Resource.Tests +
  builder cases in the pipeline test dir, per the module-test rule.

## Data model

- **PropertyAnimationClip** (source asset, XML text like scenes; cooked to
  binary product through the usual triad):
  - duration, loop hint
  - tracks[]; each track:
    - target: component type name (string, resolved via TypeRegistry) +
      property PATH (dot-joined for Nested properties, e.g.
      "light.color" - reflection already walks nested)
    - value kind: Float | Float3 | Quat | Color (v1 set; Bool/enum STEP
      tracks are a later addition, the wire format reserves the kind byte)
    - curve data: Float/Float3/Color = per-component keyframe curves
      {time, value, interpolation}; Quat = quaternion keys with slerp
      (NEVER per-component euler curves)
- Interpolation per key: Constant | Linear | Cubic (tangents). CURVE MATH:
  port from Sedulous first per the math gap policy - Sedulous has the
  XNA-heritage Curve/CurveKey; evaluate it before writing anything new.
  The existing skeletal Sampler/Easing in foundation.animation is the
  second source to check.

## Instance model (clarified 2026-08-10)

One PropertyAnimatorComponent per entity (value-pool rule), holding ONE
clip - so P1 = one clip playing per entity at a time (script-swappable;
LAYERING two clips on one entity is out, and the P2 shape for it is a
clip LIST on the component, not multiple components). Tracks target the
OWNING entity's components only - that is what keeps clips reusable
across entities ("pulse" plays on any entity with a light); multi-entity
orchestration is the sequencer's job later, not this component's. Cooked
clips are immutable SHARED data; each playing component owns only its
{time, play state, binding cache}. The manager is PER-SCENE (injected by
AnimationSubsystem), so per-scene time/time-scale/pause apply with zero
special-casing.

## Runtime

- **PropertyAnimatorComponent** {clip Ref, autoplay, speed, loop mode
  (Once | Loop | PingPong), playing state}. displayName + category
  attributes (editor-polish rule); the clip Ref needs its InspectorView
  ref-picker dispatch entry or NO picker renders (known trap).
- PropertyAnimatorComponentManager ticks playback in scene update BEFORE
  render extraction (same phase family as skeletal). Per-instance binding
  cache:
  - Resolve at play start: component by type on the owning entity ->
    PropertyInfo chain by cached hash path. A track that fails to resolve
    is DISABLED with one warning (never per-frame log spam), the clip
    keeps playing its other tracks.
  - Re-resolve on structural change via the existing mutation-generation
    guard (the entity.get lesson: never cache raw component pointers
    across structural changes - sparse-set swap-remove corrupts).
  - Writes go through reflection set (Variant) - so RESOLVE-mode rules
    and any property setter side effects apply exactly as if the
    inspector wrote the value.
- GOTCHA to spec-test: animating the transform of a physics-owned entity.
  Rule: the animator writes the transform like any other property; a
  DYNAMIC rigid body on the same entity wins (physics sync overwrites) -
  document that animated physics entities should be kinematic. Add the
  test that proves the documented behavior.
- Scripting: NO new facade lib. The component's reflected methods
  (play/stop/pause, speed, time) reach scripts through the existing
  reflected-component surface; write them with natural types (i32/f32) per
  the facade-numerics rule. Facade name count unchanged.

## Editor

- P1 ships a PROPERTY ANIMATION PAGE (bespoke-pages recipe): track list
  (add track = component-type picker + property picker filtered to
  animatable kinds), keyframe TABLE editing (time + value + interp per
  key), transport (play/scrub) with live preview on a selected entity in
  the scene page. NO curve canvas in P1.
- P2 (separate, after P1 proves the data model): the curve editor widget
  in ui.toolkit (multi-curve canvas, tangent handles, box select). It is
  real toolkit work - do not let it block the runtime shipping.
  ^^^ STALE (see CORRECTION 2026-08-16 in BUILD STATUS): the widget ALREADY
  EXISTS (UI.Toolkit CurveCanvas). "P2" is now ADOPTING it in the clip page
  (a mapping layer), not building it.
- Preview must respect the UI mutation-queue rule and write through the
  undo path when editing keys (one undo step per key operation).

## Pipeline

- New Asset creator (no OS-file importer - clips are authored in-editor).
- Builder cooks XML source -> binary product; bump kBuilderCount and the
  Pipeline.Registration tripwire test. Product registered for the player.
- Types: rtti::pipeline domain for the asset, Runtime domain for the
  component + cooked clip (they SHIP in the player - unlike authoring
  assets).

## Tests (required, per house rule)

- Curve sampling: exact key hits, between-keys lerp/cubic, Constant
  stepping, loop/pingpong time wrapping, quat slerp shortest-arc.
- Binding: resolve nested paths; missing component; missing property;
  renamed property (fails disabled, no crash); structural-change
  re-resolve under the generation guard.
- Scene integration: a clip drives Float3 position + Color light over a
  simulated tick sequence; values sampled at t match expected.
- Physics interaction test proving the documented kinematic rule.
- Wire: XML source round-trip + cooked product round-trip (count-guard on
  tracks/keys, wire-symmetry lesson).
- Tripwires: builder count; inspector picker dispatch entry present.

## Acceptance

Battery green both compilers incl. the new suites; a demo scene (one
entity: animated position + light color, looping) runs in play-in-editor
AND in the exported player; the clip asset survives export; user visual
pass on the demo.

## Explicitly deferred

- Sequencer/timeline (multi-clip, multi-entity orchestration) - later
  track over this substrate.
- Bool/enum step tracks, events-on-keyframe (the bus makes this natural
  later), clip blending, curve editor widget (P2 above).

## Open design questions for Fable - editing UX / scene-editor extensions (2026-08-16)

RAISED by the user, not yet decided. The runtime + the standalone clip page are
built and staying; this is about whether/how to ADD an in-scene authoring path,
and whether that generalizes to other spatial editors. Fable: add notes inline;
this is a genuine open question, not a proposal to react to.

FIRM CONSTRAINT (user, 2026-08-16): the standalone PropertyAnimationClip editor
page STAYS - it is not being retired. It remains until the user decides it is no
longer necessary. Any in-scene path is IN ADDITION to it, and the two must not
diverge into two separate UIs to maintain.

Context / what exists today:
- The clip is authored in a STANDALONE bespoke page (editor.propertyanimation):
  track list + curve-canvas keying (scalar) / xyzw key table (quat) + a transport
  scrub that shows sampled values. It has no scene - track targets are typed
  component-type + property strings; preview is a value readout, not a live entity.
- We ALREADY have a scene-viewport tool substrate, live-wired into the scene page
  (Editor.ViewportTools): IViewportTool (consumes viewport input + draws an overlay
  into the scene debug-draw + a status line), ViewportToolManager (per-viewport tool
  palette + gesture-safe switching), IViewportToolProvider + registry (domain libs
  contribute tools; explicit registration + count tripwire), and a
  ViewportToolHostContext that hands a tool the Scene* + command stack + entity
  Selection. Today the only tool is SelectTransformTool (the gizmo). The file's own
  comments were written anticipating terrain ("terrain.sculpt", "Editor.Terrain adds
  sculpt + splat", "a terrain brush needs a terrain in the scene").
- GAP: IViewportTool contributes an OVERLAY + STATUS only, not a dockable PANEL.
  Terrain would want a brush-settings panel; property animation the timeline/curve
  panel.

Prior art (surveyed, for calibration - "not to recreate, but to exceed"):
- Lumix: three editor-plugin flavors - IPlugin (owns editor windows), MousePlugin
  (hooks the scene-view mouse; terrain + spline paint IN the real viewport), GUIPlugin
  (owns a dockable panel with onGUI). Terrain/spline editing is in-scene brush + a
  settings panel. Lumix's property_animator editing is weaker (component-property
  curves, no in-scene timeline). Our IViewportTool is a narrower/cleaner MousePlugin
  (consume-flag + a real overlay channel); we have no GUIPlugin-style panel seam yet.
- Sedulous (our prior model): the property-animation asset page EMBEDDED its own
  viewport and let the user pick a prefab/scene document + entities to choose track
  sources, with playback/preview in the same page. It worked; the user's read is the
  UX may not have been great (the preview world is separate from the real scene).

The user's (un-fleshed) idea: "scene editor plugins" - plugins inside the scene
editor that contribute editing modes, gizmos/nodes, and UI pieces. Property
animation could be one such mode (author against the REAL selected entities, preview
in the REAL viewport); terrain and nav mesh could be others. Note the per-feature
shape differs: terrain data lives on a component (in-scene brush, no reusable asset),
nav mesh is baked from scene geometry (mostly in-scene), but a property-animation
clip is a REUSABLE cooked asset that plays on any entity (asset AND in-scene
authoring) - a hybrid the other two are not.

Questions:
1. Is a general "scene-editor extension" abstraction worth introducing (a mode/plugin
   that bundles a viewport tool + a contributed dockable panel + optional gizmos), or
   is the existing IViewportTool + per-page panels enough, wired case by case? What is
   the right SHAPE of the panel-contribution seam - does IViewportTool grow an
   optional panel view, or does the host page own a dock area the tool drives?

   > **FABLE (2026-08-16): no grand abstraction - ONE small seam, and it must
   > not live on IViewportTool.** The framework's deliberate constraint is that
   > Editor.ViewportTools has NO ui.toolkit dependency (input as plain structs,
   > overlay via debug-draw) so domain libs and headless hosts implement tools
   > without linking UI. A `CreatePanelView()` on the tool would break that.
   > The seam is a SECOND registry one layer up, in the editor-UI tier:
   > `IViewportToolPanelProvider` keyed by TOOL ID - UI-capable libs register a
   > panel factory for their tool's id; the scene page owns a reserved tool-
   > panel dock slot and, on tool activation, looks up panel-by-tool-id and
   > docks it (host owns placement, tool drives content, mirroring how
   > ViewportToolHostContext already flows context in). A "mode" is then just
   > a tool that happens to have a panel - no new plugin concept, no gizmo
   > machinery (overlay + input already cover gizmos), count tripwire like the
   > tool registry.

2. For property animation specifically: is an in-scene authoring/preview MODE worth
   adding ALONGSIDE the standalone page (which stays)? If yes, how do the two share
   the editing UI so they never diverge - does the in-scene mode HOST the same clip-
   page widgets in a scene-docked panel, or is there a shared editing core both call?

   > **FABLE: yes eventually (see Q6 for when), and the divergence answer is a
   > SHARED EDITING VIEW, not shared widgets ad hoc.** Extract the clip page's
   > track list + CurveCanvas/key-table + transport into a reusable composite
   > (`ClipEditorView`) over a small host seam (supplies the clip, the undo
   > Mutate, and preview callbacks). The standalone page hosts it full-page;
   > the in-scene panel docks the SAME view. One widget, two hosts -
   > divergence is impossible by construction, and the user's firm constraint
   > (page stays) costs nothing to honor. Do the extraction WHEN the in-scene
   > mode is built - no speculative refactor now.

3. Track-source picking + live preview from the live scene: [...]

   > **FABLE: all three concerns are real and all have house answers.**
   > (a) Preview only in EDIT state - the transport disables during
   > Simulate/Play. An animator fighting physics/scripts is exactly the
   > divergence the kinematic rule exists for; in edit state nothing else
   > writes, so it cannot fight.
   > (b) Preview writes are TRANSIENT: snapshot affected properties on
   > preview start, restore on stop/scrub-end (the gizmo drag-preview
   > pattern), NEVER through undo, never dirtying the document. Track ADDS
   > from the selection are normal document mutations through undo.
   > (c) Drive the REAL runtime evaluation (the component manager's sample +
   > apply path) against the selected entity rather than reimplementing
   > sampling in the editor - the generation guard and binding re-resolve
   > come free and preview-vs-runtime divergence is structurally impossible.
   > Mutation-queue: preview writes VALUES only (no view/entity lifetime) -
   > out of scope for the queue rule by design; keep it that way.

4. Do terrain + nav mesh genuinely share this system [...]

   > **FABLE: they share exactly the tool + panel seam, nothing more.**
   > Terrain = brush tool + settings panel (the framework's own comments
   > anticipated it). Nav = bake panel + debug overlay (barely a "tool" -
   > maybe a test-path probe). Prop-anim = panel + selection-driven tracks +
   > preview. Design the Q1 seam's SHAPE now (it is one interface + registry,
   > cheap to hold in mind), but BUILD it with its first real consumer -
   > whichever of navigation/terrain ships first per the parity queue - then
   > the other two adopt. Do not retrofit three times, and do not build it
   > bare with zero consumers.

5. Where does the reusable-asset-vs-scene-local tension land [...]

   > **FABLE: the "inconsistency" is the data model showing through, and it
   > is correct.** Terrain lives on a component (scene-local editing only);
   > nav is baked scene output; a clip is a reusable cooked asset that also
   > wants in-context authoring. Tools follow their data. Godot/Unity make
   > the same split (in-scene animation editing over a reusable asset).
   > Forcing one model would flatten a real difference - explicitly rejected.

6. Scope check: is this a near-term track, or does it sit behind the current queue [...]

   > **FABLE: behind the queue.** The standalone page ships a working
   > authoring path; nothing blocks on in-scene authoring. Order: navigation
   > + terrain proceed as planned; the Q1 panel seam lands with the first of
   > those needing a panel; the prop-anim in-scene mode follows AFTER the
   > seam exists (at that point it is small: dock the extracted
   > ClipEditorView + the Q3 preview rules). This also honors
   > validate-incrementally: the seam gets a real consumer before it
   > generalizes.
   >
   > **OVERRIDDEN by the user (2026-08-16): in-scene editing builds FIRST.**
   > Both paths are wanted; the in-scene mode is the priority and lands
   > BEFORE any further focused-page work (the page's remaining follow-ups -
   > live scene-entity preview etc. - are PAUSED until the in-scene mode
   > ships). Consequences: the Q1 panel seam's first consumer is property
   > animation (not navigation/terrain - they adopt it later), and the Q2
   > ClipEditorView extraction happens NOW as part of this build. All other
   > rulings above stand unchanged. Build plan: Phase H below.

## Phase H - the in-scene authoring mode (user-prioritized 2026-08-16)

Build order (each phase battery-green before the next; the standalone page
must remain fully functional THROUGHOUT - it re-hosts the shared view in H2
and never regresses):

- **H1 - the panel seam.** `IViewportToolPanelProvider` registry in the
  editor-UI tier (NOT on IViewportTool - the tool framework stays UI-free),
  keyed by tool id; ScenePage reserves a tool-panel dock slot and
  docks/undocks the panel on tool activation/deactivation; explicit
  registration + count tripwire + tests (panel appears/disappears with the
  tool; unknown tool id = no panel, no crash; gesture-safe tool switching
  keeps working). DESIGN WITH NAVIGATION IN MIND (user, 2026-08-16: nav
  builds right after this track, so accounting for it is not wasted): nav's
  consumer shape is a BAKE/settings panel + debug overlay + a test-path
  probe tool - which fits this seam as long as (a) the panel factory gets
  the same ViewportToolHostContext the tool gets (scene + selection +
  command stack - the bake panel needs the Scene*), and (b) panel lifetime
  is activation-scoped but the panel VIEW may be recreated cheaply (no
  state hoarding in the view - state lives in the tool/domain lib). Note
  both properties in the H1 interface comments and hold them in review.
- **H2 - ClipEditorView extraction.** Lift the clip page's track list +
  CurveCanvas/key-table + transport into the shared composite over a small
  host seam (clip access + undo Mutate + preview callbacks). The standalone
  page re-hosts it BEHAVIOR-IDENTICAL (existing page tests keep passing
  unchanged - that is the acceptance for this phase).
- **H3 - the PropertyAnimationTool.** An IViewportTool + panel provider in
  editor.propertyanimation: activating the tool docks the panel hosting
  ClipEditorView; clip pick-or-create (asset picker + New); "add track from
  selection" - the component/property picker seeded from the SELECTED
  entity's actual components (typed, filtered to animatable kinds); track
  adds go through undo as normal document/asset mutations.
- **H4 - live preview (the Q3 rules, mechanized).** Transport drives the
  RUNTIME evaluation path against the selected entity; preview writes are
  snapshot/restore transients (never undo, never dirtying scene or asset);
  transport hard-disabled outside the EDIT scene state; overlay draws
  animated-property markers (debug-draw channel). Tests: snapshot-restore
  round-trip, disabled-under-sim, structural-change re-resolve via the
  generation guard, scene not dirtied by preview.

Acceptance: author a clip end-to-end against a selected entity WITHOUT
leaving the scene page; the same clip opens identically in the standalone
page; preview never dirties the scene; battery green both compilers; user
visual pass. Navigation/terrain adopt the H1 seam when they build.

STATUS 2026-08-16: H1-H4 all BUILT + battery-green on clang and gcc (see the
BUILD STATUS block at the top for the per-step summary + assertion counts). The
editor executable links on both compilers. Remaining: the user's on-screen visual
pass (smoke-checklist.md 2026-08-16 in-scene-authoring section).

## IN-SCENE EDITOR REDESIGN (user direction 2026-08-16, post-UAT)

The Phase-H tool-mode/left-rail in-scene editor was wrong. New shape (user-decided):

1. **Persistent bottom panel, NOT a viewport tool-mode.** The animation editor is a
   view the scene page OWNS for its lifetime (not activation-scoped) - which also
   kills review-pass-10 #5 (undo command pointed at a recreatable view -> UAF) and
   #6 (stale preview snapshot) by construction. Retire PropertyAnimationTool +
   IViewportTool + the two providers; the H1 tool-panel seam stays PARKED for future
   modal tools (terrain), unused here.
2. **Docked below the VIEWPORT only, with a RESIZABLE SPLITTER** - a vertical
   SplitView between the viewport and the panel. It must NOT extend under the
   hierarchy or inspector (correction to the transitional full-page bottom dock in
   commit 45cafbb0). Layout: hierarchy | ( [viewport / panel]-vsplit | inspector ).
   SplitView min-pane is 50px, so the panel is always present (persistent) - a resize
   splitter and a "Gone until active" panel are incompatible.
3. **A ported DOPESHEET timeline scrubber** (Sedulous TimelineView, Code/Editor/
   Sedulous.Editor/src/Pages/TimelineView.bf - reference at /home/robert/Dev/Beef/
   SedulousEngine): left track-label column, top time RULER (ticks/labels), keyframe
   DIAMONDS per row, a draggable red PLAYHEAD (click/drag the grid or ruler to scrub
   -> fires a time-changed event that drives the live preview), + a Play/Pause/Stop
   TRANSPORT that auto-advances the playhead (DCC conventions: Stop->0, Play-from-end
   replays, loop-wrap). Replaces the numeric "Scrub" field. The per-track CurveCanvas
   becomes a curve editor for the SELECTED track (shaping), the dopesheet is the
   timing/scrub surface.
   - **Do NOT port blindly - Sedulous had interaction glitches.** Improve: robust
     keyframe drag (sort on release, keep "their" keyframe by time not index - Sedulous
     did this, keep it; but avoid mid-drag index thrash), clear playhead-vs-keyframe
     hit priority, keyboard scrub (arrows), optional snapping, empty-state that is not
     a "Phase 3 picker" stub. Audit the .bf on-screen behavior before copying.
4. **Asset picker slot upgrade** (separate track, user-requested): the inspector
   AssetPickerSlot gets 3 controls - preview icon / Edit button / Clear button - plus
   drag-drop FROM the asset browser (type-filtered; wrong type = warning log/toast),
   and click-preview reveals the asset in the browser. Edit on a property-anim clip
   opens this in-scene panel focused on that clip; Edit on other assets opens their
   page. This becomes an activation path for the panel (with a View toggle + auto on
   selecting an entity that has a PropertyAnimator).

Sequencing: (a) persistent panel + splitter-below-viewport placement (fixes #5/#6,
honors the placement correction), (b) the dopesheet widget, (c) the asset slot +
activation. Each lands green on both compilers with tests.
