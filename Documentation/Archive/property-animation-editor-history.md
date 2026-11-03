# Property Animation Editor (design)

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

> STATUS: APPROVED WITH AMENDMENTS (Fable review 2026-08-16) - build in the
> proposed phase order; the rulings on the five open questions and the
> amendment list are at the bottom and are BINDING. Nothing here is built yet.
> This supersedes the "IN-SCENE EDITOR REDESIGN" note in property-animation.md
> and is the authoritative design for the in-scene property-animation editor.
> The runtime + data model (property-animation.md phases A-G) are DONE; this is
> purely the EDITOR. Reference sources farmed locally: Godot
> (`/home/robert/Dev/CPP/godot/editor/animation/animation_track_editor.cpp` +
> `animation_bezier_editor.cpp` + `animation_player_editor_plugin.cpp`), Traktor
> (`/home/robert/Dev/CPP/traktor/code/Ui/Sequencer/*` +
> `code/Theater/Editor/TheaterComponentEditor.cpp` + `code/Core/Math/TransformPath.*`),
> Sedulous (`/home/robert/Dev/Beef/SedulousEngine/Code/Editor/Sedulous.Editor/src/Pages/TimelineView.bf`).

## Goal

A property-animation editor that is genuinely good - better than Sedulous's basic
dopesheet, drawing the best ideas from Godot (the deepest) and Traktor (the
cleanest widget architecture + viewport preview). It keyframes ANY reflected
component property (plus the built-in Transform) over time, scrubs the live scene,
and lets you shape curves. Docked below the viewport, resizable, persistent.

## Reference survey (what we take from each)

- **Godot** - the richest interaction model. TAKE: one time-mapping authority all
  views share; per-lane playhead overlay (cheap scrub); drags-are-visual +
  atomic-commit + reselect-by-time (the anti-glitch discipline); per-key
  interpolation + a bezier sub-editor; box-select; inspector "key" button; two
  snap toggles; ruler tick algorithm. AVOID: the 10k-line god-object; hand-rolled
  hit-test rects for the whole row; rebuild-the-world on every change; scattered
  re-entrancy booleans.
- **Traktor** - the cleanest architecture. TAKE: a DOMAIN-AGNOSTIC timeline widget
  that emits gesture EVENTS and lets the host commit (widget never mutates the real
  model); Act -> Track(entity+property) -> Key model; scrub-poses-the-live-scene
  with physics auto-disabled + a playback->playhead feedback loop; capture-based
  keying; viewport PATH PREVIEW cross-highlighted from timeline selection; Ease
  Velocity / Time-Scale. AVOID: no real ruler; single-select only; key-drag inverts
  order (never re-sorts); no numeric value editor; TCB-only; Euler orientation.
- **Sedulous** - the baseline we are replacing. Its dopesheet (label column + ruler
  + diamonds + draggable playhead + Play/Pause/Stop) is the right SHAPE; its
  interaction has the glitches the user flagged (from the same class Godot's
  discipline in D4 below prevents).

## Current substrate (already built - reuse, do not rebuild)

- Data: `PropertyAnimationClip { f32 duration; Array<PropertyTrack> tracks }`;
  `PropertyTrack { String componentType, propertyPath; TrackValueKind kind;
  Curve channels[kMaxChannels]; Array<QuatKey> quatKeys }`. `Curve` holds
  `CurveKey { time, value, tangentIn, tangentOut; CurveKeyInterpolation
  (Constant/Linear/Cubic) }` - so PER-KEY interpolation + tangents ALREADY EXIST
  (Godot-parity on that axis; Sedulous/Traktor are per-track only).
- Editor seam: `IClipEditorHost` (clip + command stack + dirty + scrub hook) +
  `ClipEditorView` (the shared view). `CurveCanvas` (a working curve/bezier editor
  widget, per channel).
- Runtime + preview: `PropertyAnimatorComponent` + the binding resolver
  (`ResolveBinding`/`WriteBinding`/`ReadBinding`), the built-in "Transform" target
  (RMW via Set/GetLocalTransform), and a working live preview onto the selected
  entity (snapshot/restore, empty-channel-safe via `SampleMerged`).

We are missing the SHELL: a timeline/dopesheet, a real playhead scrubber, a
transport, and viewport path preview. Those are this spec.

## Core architecture decisions (the crux - please review these first)

- **D1 - one Timeline transform authority.** A single object owns time<->pixel
  mapping: `pixelsPerSecond` (zoom), `scrollSeconds` (pan), `labelColumnWidth`.
  Every view (ruler, each lane, playhead, curve view) READS it; none caches its own
  copy. Horizontal scroll/zoom/pan then fall out of one place. (Godot's `Range`
  timeline; Traktor's bug was caching the scale per-row.) Views subscribe to a
  "transform changed" signal rather than a hand-maintained web of redraw calls.
- **D2 - playhead is a cheap overlay.** Scrubbing updates only the playhead's
  draw + the live preview, never a full dopesheet repaint. (Godot per-lane overlay.)
- **D3 - reference keyframes by STABLE IDENTITY, not array index.** A dragged/edited
  key must survive re-sorting without the selection pointing at the wrong key. Two
  options for review: (a) add a `u32 id` to `CurveKey`/`QuatKey` (stable, cheap,
  wire-neutral if not serialized - or serialized for cross-session stability), or
  (b) keep index-based storage but ALWAYS re-derive selection by TIME after any
  mutation (Godot does this; more code, no data change). RECOMMEND (a) - it makes
  D4 nearly free and kills the whole index-thrash bug class. DECISION FOR FABLE.
- **D4 - drags are purely VISUAL; mutation is one atomic undo command at commit;
  reselect by identity/time.** During a keyframe drag NOTHING in the clip changes -
  the widget draws the key at an offset. On release, ONE `ClipEditCommand`
  (before/after snapshot, our existing pattern) applies the move, re-sorts, and the
  selection re-resolves by id (D3a) or time (D3b). This single discipline is what
  eliminates Sedulous's glitches (drag-past-neighbour inverts order; selection
  points at the wrong key mid-drag). A small epsilon gate distinguishes a click
  (select) from a drag (move) so a click never jitters a key.
- **D5 - the timeline is a domain-agnostic widget that emits events.** Following
  Traktor: a reusable `Timeline`/dopesheet widget knows only rows + keys positioned
  in time; it emits `PlayheadMoved`, `KeyDragged(row,key,deltaTime)`,
  `KeySelectionChanged`, `KeyContextMenu`, etc. The host (the animation panel)
  owns the clip and commits every gesture through the command stack. The widget
  never touches `PropertyAnimationClip`. This keeps undo/validation/re-sort in the
  host and makes the widget testable + reusable (a future sequencer/cutscene tier).
- **D6 - scrub poses the live scene, with the guards Traktor has and we lack.**
  We already write sampled values onto the selected entity. ADD: disable physics on
  the previewed entity during scrub (a dynamic body fights authored transforms -
  the documented physics rule); and a playback->playhead feedback path so that
  when the scene plays, the playhead follows (not just editor->scene). Explicit
  "who owns the playhead now" state (Editing | Playing) instead of scattered
  booleans (Godot's re-entrancy lesson).

## Layout

Corrected placement (user 2026-08-16): the editor is a RESIZABLE pane BELOW THE
VIEWPORT ONLY - a vertical SplitView between the viewport and the panel, inside the
viewport column, NOT spanning under the hierarchy or inspector. Because SplitView
enforces a 50px minimum pane, the panel is PERSISTENT (always present, resizable),
which also fixes the review-pass-10 #5 undo-UAF + #6 stale-snapshot by making the
editing view outlive its commands.

```
[ hierarchy | [ viewport  ]                         | inspector ]
             [ ----------- <- draggable splitter --- ]
             [ transport toolbar                     ]
             [ tracklabels | ruler                   ]
             [   Transform | keyframe lanes + playhead]  <- dopesheet
             [   Light     |    (curve view for the   ]
             [             |     selected track below)]
```

- The dopesheet: a LEFT label column (track = "Component.property", with per-track
  controls) and a RIGHT lane area (ruler on top + one keyframe lane per track +
  the playhead). One shared draggable `labelColumnWidth` divider (Godot's
  `name_limit` - a shared int, glitch-free alignment, simpler than nested splitters).
- The curve/bezier view (our `CurveCanvas`) shows the SELECTED track's channels for
  shaping - either inline-expanded under the track row (Godot bezier-per-track) or a
  toggled sub-panel. The dopesheet is the timing/scrub surface; the curve view is the
  value-shaping surface. RECOMMEND: a mode toggle (Dopesheet | Curves) plus
  expand-a-single-track-to-curves, decided in phasing.
- IMPORTANT (Godot pitfall): use REAL child controls for the left-column per-track
  buttons (enable, interp mode, remove) - hand-drawn hit-test rects are brittle (no
  focus/keyboard/tooltips). Hand-draw ONLY the keyframe lane + ruler + playhead.

## Timeline, playhead, scrubbing

- Ruler: labeled ticks using the {1,2,5}x10^n step so labels never collide at any
  zoom (Godot ATE tick algorithm); primary + secondary grid lines. A Seconds/Frames
  toggle (fps field). Zoom = wheel, ANCHORED at the cursor (Godot), plus zoom-to-fit
  (frame all keys) and a zoom slider.
- Playhead: a draggable red line + a head handle in the ruler. Click/drag anywhere
  in the lane area OR the ruler scrubs. Keyboard: Left/Right step by the snap unit;
  Shift = 0.25x precise; jump to prev/next KEY (a distinct shortcut).
- Two INDEPENDENT snap toggles (Godot): snap-the-playhead and snap-dragged-keys are
  separate - you often want key-snap without playhead-snap. Snap unit = a step
  field (fps-compat rounding). Relative snapping preserves the selection's phase.
- Scrub drives preview (D6): playhead time -> sample clip -> WriteBinding onto the
  selected entity (already built), gated to EDIT state.

## Keyframe interaction

- Insert: (a) an "Add key at playhead" per selected track (type-appropriate default,
  seeded from the channel's value at the playhead - NOT 0, per review-pass-10 #2b);
  (b) an inspector "key" button per animatable property that inserts at the playhead
  reading the property's LIVE value (Godot) - batched into ONE undo when keying a
  whole vector; (c) capture-based keying (Traktor): "Key selected entity" snapshots
  all the selected entity's animated properties at the playhead, overwrite-if-near
  else-insert - reuses the move/rotate gizmos as the value editor.
- Select: single click (reverse-order hit test so the topmost diamond wins, Godot);
  Ctrl/Shift aggregate toggle; BOX / rubber-band select across lanes (a selection
  overlay; on release each lane contributes its keys in the rect). Selection is a
  set keyed by (track, channel, keyId) (D3).
- Move: purely visual during drag; atomic commit on release; reselect by identity
  (D3/D4). Multi-select shares one time-offset (relative spacing preserved). Snap
  per the key-snap toggle.
- Numeric edit: selecting key(s) shows time + value(s) + interpolation in an edit
  strip (or pushes them to the Inspector like Godot - decide in phasing). Direct
  numeric time/value entry with clean undo.
- Copy/cut/paste (relative to the playhead), duplicate, delete, box-delete. Clipboard
  stores time/value/interp so paste can retarget a compatible track.
- Value-axis ops worth stealing: Scale-selection (about selection or playhead),
  Ease-velocity (Traktor arc-length redistribution for constant-speed motion),
  Time-scale the whole clip.

## Interpolation + curves

- Per-KEY interpolation already exists (Constant/Linear/Cubic). Surface it per key in
  the curve view (not flattened per-track - fixes review-pass-10 #7 WriteBackTrack
  flattening); keep a per-track DEFAULT for new keys.
- Bezier handles: our `CurveKey` has tangentIn/Out; the `CurveCanvas` already draws
  Cubic tangent handles. Add handle MODES (Free / Mirrored / Balanced / Auto) with
  the SCREEN-SPACE RATIO CORRECTION Godot uses (balanced looks balanced on screen
  regardless of non-uniform time/value pixel scale - ABE:2259; non-obvious, copy it).
- Quaternion tracks: keep the numeric xyzw key table for timing + slerp preview
  (already slerped) - no curve handles for rotation (Traktor's Euler gimbal risk is
  avoided; we store quaternions).

## Track types, property picker, grouping

- Tracks bind (componentType, propertyPath, kind). The built-in "Transform" target
  animates position/rotation/scale (already wired). Add-from-selection already
  enumerates the selected entity's animatable reflected properties (InferTrackKind +
  CollectAnimatableProperties, ResolveBinding-validated) - keep it; ALSO add an
  explicit picker (component -> property, filtered to animatable kinds) like Godot's
  SceneTree->PropertySelector for adding one track deliberately.
- Group lanes by component (Godot node-grouping); a track filter/search box; a
  "show only selected entity's tracks" toggle.

## Transport

- Play / Pause / Stop + Play-from-playhead; loop mode driven by the animator
  COMPONENT's loopMode (clip.loop is dead, removed); a speed field; a current-time
  (frame/seconds) field; DCC conventions (Stop -> 0; Play-from-end replays;
  loop-wrap). Playback auto-advances the playhead each frame (the panel Ticks it);
  during playback manual scrub is ignored until Stop (Godot's early-return).

## Viewport integration (Traktor path preview)

For a Transform position track: draw the position curve as a polyline in the 3D
viewport (sample the clip), plus a gizmo at each keyframe and a moving axis marker
at the playhead pose. Highlight the curve from TIMELINE selection (selected track =
bright). Scrubbing already poses the live entity along it. This is the spatial
feedback that makes motion authoring feel good. (We already have DrawOverlay + the
debug-draw channel; extend it from a single marker to the path + key gizmos.)

## Lifetime / persistence

The panel is a persistent view the scene page owns (retire PropertyAnimationTool +
IViewportTool + the two providers; the H1 tool-panel seam stays parked for future
modal tools like terrain). This fixes #5 (undo command outliving a recreatable
view -> UAF) and #6 (stale preview snapshot) structurally. Re-snapshot on
track-identity change is already designed.

## Asset picker slot (separate, user-requested; enables activation)

Upgrade `AssetPickerSlot` to 3 controls - preview icon / Edit / Clear - plus
drag-drop from the asset browser (type-filtered; wrong type = warning toast/log),
and click-preview reveals the asset in the browser. Edit on a PropertyAnimationClip
opens THIS panel focused on that clip; Edit on other assets opens their page. This
becomes an activation path for the panel alongside a View toggle and auto-open when
selecting an entity that has a PropertyAnimator.

## Explicitly NOT copying

- Godot's monolith (one 10k-line god-object) - keep clean module seams: a Timeline
  transform service, a domain-agnostic dopesheet widget (D5), the host/command
  layer, the curve view.
- Hand-drawn hit-test rects for the whole row (use real controls for the left column).
- Rebuild-every-widget on structural change (incremental row diffing).
- Traktor's single-selection, no-re-sort key drag, missing ruler, Euler rotation.
- Sedulous's mutate-during-drag (the glitch source).

## Phasing (proposed; land each green on both compilers with tests)

- P1 - shell: persistent panel + splitter-below-viewport + transport
  (Play/Pause/Stop + playhead) + the Timeline transform (D1) + a real ruler. Keep
  the existing per-track CurveCanvas rows as the body for now. Fixes #5/#6.
- P2 - the dopesheet widget (D5): domain-agnostic Timeline/dopesheet with lanes +
  keyframe diamonds + draggable playhead + click-to-scrub, event-driven; wire it as
  the primary timing surface with the curve view for the selected track. Box-select,
  drags-are-visual + atomic-commit + reselect-by-id (D3/D4). Snap toggles.
- P3 - keying + polish: inspector "key" button, capture-based keying, copy/paste,
  scale/ease ops, bezier handle modes, viewport path preview, the property picker.
- P4 - asset slot + activation (its own track; can proceed in parallel).

## Open questions - Fable rulings (2026-08-16, binding)

1. **D3 key identity: NEITHER option as written - the COMMIT RETURNS THE REMAP.**
   Do NOT add an id field to `CurveKey`/`QuatKey`: `Curve` is a foundation.core
   MATH type and editor-selection identity does not belong in it; and
   reselect-by-time alone is ambiguous because coincident key times are legal in
   our `Curve`. The clean third option: D4 already funnels every mutation
   through one commit, and the commit site is the only place a re-sort happens -
   so the commit applies the move with a STABLE sort and returns the index
   permutation; the host maps the selection set through it. No new data, no
   ambiguity, foundation stays clean. For undo/redo (before/after snapshot
   restores where no permutation exists), re-derive selection by
   time-with-epsilon and accept approximation there - it is the rare path.
   If this gets genuinely messy in P2, the fallback is a RUNTIME-ONLY id
   side-table owned by the editor (never a field on the math type, never
   serialized).
2. **Dopesheet vs curves: the MODE TOGGLE ships in P2; inline-expand is P3
   polish if still wanted.** Hard requirement either way: the curve view MUST
   share the D1 time transform (same zoom/pan/playhead/label-column width) so
   toggling keeps visual context - Godot's bezier editor sharing the time axis
   is the part that matters, not which affordance switches views. This also
   means `CurveCanvas` migrates off its internal normalized 0..1 time domain
   for this use: the widget consumes the shared seconds<->pixels transform
   directly. That kills the re-domain drift class (pass-10: `(x/d)*d` per
   gesture) at the root instead of tolerating it.
3. **Numeric key editing: the in-panel edit strip.** Our inspector is
   entity-bound; Godot's throwaway-object pattern would drag reflection shims
   in for little gain. The strip (NumericFields for time + per-channel value +
   an interpolation dropdown) lives in the panel, works identically whether or
   not an entity is selected, and reuses existing controls.
4. **The Timeline widget goes in `foundation.ui.toolkit` from day one** -
   sibling of `CurveCanvas`, rows + keys + ruler + playhead + selection +
   events ONLY, zero clip knowledge (D5 as written). We already know the next
   consumers (sequencer/cutscene tier, skeletal-animation event tracks), the
   incremental cost is small, and the placement FORCES the domain-agnostic
   seam instead of promising it. It also makes the widget headlessly testable
   in UI.Toolkit.Tests.
5. **Capture-based keying: yes in P3, but decoupled by construction.** Do not
   couple to the gizmos: "capture" = read the entity's current property values
   through the EXISTING binding resolver (`ReadBinding`) at commit time -
   however those values got there (gizmo drag, inspector edit, script). The
   gizmo needs zero knowledge of the animation editor and vice versa; the
   spec's own coupling worry dissolves.

## Fable amendments (binding, additive to the design above)

- **A1 - the standalone clip page is RETIRED.** The spec is silent on the
  two-host story; ruling: the panel is THE editing surface.
  `PropertyAnimationClipEditorPage` + its factory go away in P1 (asset-browser
  "Edit" focuses the panel with the clip loaded, per the asset-slot section);
  entity-unbound editing works in the panel with preview simply disabled.
  KEEP a thin host seam (the IClipEditorHost shape: clip + commands + dirty)
  between panel chrome and editing view so a standalone page can return
  cheaply if ever wanted - but do not build or keep one now.
- **A2 - persistent but COLLAPSIBLE.** The panel view is persistent (that is
  what fixes pass-10 #5/#6 - never destroyed while its commands live), but
  scene editing wants its vertical space back: the pane collapses to the
  transport strip (or a thin restore handle) via a header toggle. Collapse
  hides, never destroys. SplitView's 50px minimum is an implementation detail,
  not the reason for persistence - state the lifetime rule, not the widget
  constraint.
- **A3 - seconds are the only time currency.** Widget events, the D1
  transform, and key storage all speak clip-domain SECONDS; there is no
  normalized layer anywhere in the new surface (see ruling 2). The
  Seconds/Frames toggle is a display/snap format only.
- **A4 - transport loop is an editor-local toggle**, DEFAULTED from the
  selected animator component's loopMode when bound. Preview looping must not
  depend on which entity happens to be selected once the user has set it, and
  unbound clip editing needs a loop control too. (clip.loop stays dead.)
- **A5 - theming + design system.** All new chrome resolves from the theme:
  the widget follows the CurveCanvas precedent (background/border/dim-text via
  background-color/border-color/text-dim-color + registered type names for
  both the Timeline widget and the panel); the playhead + selection colors
  resolve AccentColor (playhead may prefer error-red - resolve ErrorColor,
  fall back red); transport/toolbar text is 12px compact chrome per the ramp.
  No hand-picked hex constants in draw code - fallbacks only.
- **A6 - damage-gate producers.** Scrub and playback self-chain damage
  (MarkNeedsRedraw per frame while active); an idle panel contributes ZERO
  redraws. Playhead-only updates must not repaint lanes (D2 is the mechanism -
  state the gate contract explicitly so it is tested, not hoped).
- **A7 - undo inventory.** One command per user gesture, including multi-key
  box drags, paste, scale-selection, ease ops, and whole-vector inspector
  keying (the spec has this); discrete repeatable actions (add-track,
  add-key-at-playhead) take BeginGroup+LockGroup so consecutive invocations
  never coalesce (pass-10 #7 lesson). Commands hold clip snapshots + the host
  seam, NEVER view pointers (pass-10 #5).
- **A8 - test inventory (the seam checks for THIS surface, written with each
  phase, not after).** Widget (UI.Toolkit.Tests, headless): time<->pixel
  transform round-trip incl. zoom-anchor math, ruler tick algorithm
  ({1,2,5}x10^n never collides), hit-test topmost-wins, box-select set math,
  snap math (both toggles, relative phase), event emission per gesture, and
  drag-is-visual (no model mutation before commit - the widget has no model to
  mutate, assert the events carry deltas only). Host (Editor tests): one
  undo step per gesture incl. box drag; commit remap preserves selection
  across re-sort; per-key interpolation survives edit round-trips (the
  flattening regression); add-key seeds from the channel value at playhead,
  never 0; preview snapshot lifecycle across track-set changes; transport
  state machine (Editing|Playing ownership, scrub ignored while playing).
  Keep the four original canvas checks green (Constant staircase, no-edit
  byte-identity - now exact per A3, one-undo-per-gesture, MaxKeys).
- **A9 - track lanes scale.** Label column + lane area share ONE vertical
  scroll; rows are cheap (no per-row allocations per frame) and the structural
  rebuild path diffs rows incrementally (the spec's own "not copying" list) -
  virtualize only if a real clip shows the need, but keep the row model
  virtualization-shaped (flat array of row records, not a nest of ad hoc
  children).
- **A10 - keyboard map (P2 minimum).** Space = play/pause, Home/End = clip
  start/end, Left/Right = step by snap (Shift = 0.25x), Ctrl+Left/Right =
  prev/next key, K (or Insert) = key-at-playhead for selected tracks, Delete =
  delete selected keys, Ctrl+C/X/V = clipboard, Ctrl+A = select all keys in
  visible tracks. Document deviations in the spec when implementation forces
  them.

Review pass cadence: each phase lands green on both compilers with its A8
slice; Fable reviews per phase against this spec (the pass-10 required fixes
1-3 and 7-8 on the RUNTIME side proceed independently and are not blocked on
this editor).

---

## P1 implementation notes (for Fable review)

P1 is COMPLETE and on master, green clang + gcc. Review against the rulings +
amendments above. Commits:

- **P1a `5b8b0d68`** - `Timeline` widget in `foundation.ui.toolkit`
  (`Timeline.cppm`): the D1 seconds<->pixel transform authority
  (pixelsPerSecond / scrollSeconds / labelColumnWidth, TimeToX/XToTime), a
  {1,2,5}x10^n ruler (`PickTickStep`), a draggable playhead firing
  `OnPlayheadMoved`, cursor-anchored wheel zoom. Seconds-only (A3). Headless
  tests: tick algorithm never collides, transform round-trips through
  zoom+scroll, playhead clamps + fires on change. Also added
  `Color::Rgb(u8,u8,u8,u8=255)` to foundation.core.
- **P1b `ba8860c6`** - retired the tool-mode AND the standalone clip page
  (A1). `PropertyAnimationPanel` (`editor.propertyanimation:panel`) is a
  persistent `ui::FlexLayout` + `IClipEditorHost`, scene-page-owned, docked in
  a resizable vertical SplitView BELOW THE VIEWPORT ONLY (A2). Fixes pass-10 #5
  (view never recreated mid-edit; commands hold snapshots+host, never a view
  ptr) and #6 (re-snapshot on track-set identity change - has a regression
  test). The H1 tool-panel seam is parked/unused; the primary module unit is
  now just the plugin registrar.
- **P1c `0c827675`** - transport (Play/Pause/Stop + loop). Play auto-advances
  the playhead by dt each frame (`Tick(f32 dt, bool editingLocked)`) driving
  Timeline + preview; loop wraps, loop-off clamps+stops, Stop rewinds. D6
  Editing|Playing ownership: active-play scrubs ignored, paused/editing scrubs
  reposition the clock. A6 damage gate: only active playback self-invalidates.

### Deviations / decisions to confirm
1. **A4 loop default - NOT per-instance (needs a ruling).** The loop toggle is
   editor-local, defaulting to Loop (matches a fresh PropertyAnimatorComponent).
   Seeding it from the SELECTED entity's bound animator's `loopMode` would
   couple the clip editor to `engine.animation` (the component lives there).
   Deferred pending your call: accept editor-local, or take the dependency?
2. **Numeric scrub field retired from ClipEditorView** - the Timeline is the
   sole scrubber now (`ClipEditorView::SetScrubTime` drives the readout). The
   Length field + sampled-value readout row stay.
3. **P1 keeps the existing CurveCanvas rows** (still on normalized 0..1 time);
   migrating CurveCanvas onto the shared Timeline transform (ruling 2) is P2
   as planned.
4. Panel impl gotchas worth a glance: the accessor is `EditorCtx()` not
   `Context()` (would shadow `ui::View::Context`); `Transform` is spelled
   `core::Transform` (ambiguous once ui.toolkit is imported).

P2 (the dopesheet widget: lanes/keys/box-select/drags-visual/commit-remap
D3/D4, CurveCanvas onto the shared transform, snap, A10 keyboard) starts after
this review.

## FABLE REVIEW - P1 (2026-08-17): PASS with rulings; P1d REQUIRED before P2

The Timeline widget is exactly the D1/D5 shape (seconds-only authority, pure
testable tick step, themed chrome with fallbacks, ErrorColor playhead,
cursor-anchored zoom); the panel fixes pass-10 #6 with a regression test; the
transport's Editing|Playing ownership + damage gating match D6/A6. Battery
green both compilers. Rulings on the deviations:

1. **Loop default: ACCEPT editor-local** (A4 amended). Seeding from the bound
   animator's loopMode is not worth an engine.animation dependency in the
   editor lib; editor-local defaulting to Loop matches a fresh component
   anyway.
2. Numeric scrub field retired for the Timeline scrubber: ACCEPT (Length +
   sampled-value readout staying is right).
3. CurveCanvas normalized-time until P2: per plan.

**Findings:**
- **F1 (fix in P2, note now): the spec note says commands hold "snapshots +
  host, never a view ptr" but ClipEditCommand STILL holds `ClipEditorView*`.**
  It is lifetime-SAFE today purely by ownership (page-scoped command stack and
  page-owned persistent view die together), but that safety is an unstated
  invariant one refactor away from a regression - the exact pass-10 #5 class.
  P2 routes ApplyState through IClipEditorHost (which the command already
  could hold) and drops the view pointer for real.
- F2 (P2, when the label column becomes real): Timeline's band and gutter both
  resolve StyleProperty::Background with different fallbacks - one themed rule
  collapses the distinction. Derive the gutter (darken the resolved band) or
  give it a part-style.

## A2 REVISED - the Godot bottom dock (user direction 2026-08-17; build as P1d)

The persistent always-split pane is NOT the wanted interaction. Target
(Godot's bottom panel): a thin BUTTON BAR always visible under the viewport
("Animation" now; Output/Debug-style tabs join later). Clicking a tab EXPANDS
the bottom section above the bar - dopesheet visible, a draggable splitter
between viewport and the section; clicking it again COLLAPSES to just the bar
- NO splitter shown. The split ratio survives collapse/expand.

Mechanism (ruled):
- **SplitView gains a pane-collapse mode**: `SetPaneCollapsed(pane, bool)` -
  the collapsed pane measures at its content's minimum (the bar height), the
  divider hides and stops hit-testing, and the stored ratio is untouched for
  restore. Generic toolkit capability; no reparenting, ONE container, the
  panel view's lifetime untouched (the F1 invariant holds).
- **A `BottomDock` strip owns the bar**: a toggle-tab row (Animation first,
  N-tab ready - registrations like the page factories) above nothing when
  collapsed, above the active tab's content when expanded. The
  PropertyAnimationPanel body becomes the Animation tab's content; its header
  chrome (clip name/New/Pick/transport) shows only when expanded.
- Default state: collapsed. Expanding drives SetPaneCollapsed(false) +
  body Visible; collapsing the reverse. Keyboard/persistence polish later.
- SCOPE (user ruling 2026-08-17): the dock is PER-SCENE-PAGE - it lives inside
  the scene editor page, one per page, bound to that page's scene instance
  (multiple open scene pages each carry their own dock + dopesheet). Editor
  SINGLETONS (Console/Output) live in the shell docking and NEVER migrate here
  - a page-scoped dock would hide them with the page. The bar stays N-tab
  shaped only for future PAGE-SCOPED occupants (per-scene surfaces), not for
  editor-global panels.

Sequencing: P1d (this rework) lands BEFORE P2 - the dopesheet should grow
inside the final interaction shell, not get re-parented after.

### P1d implementation notes (for Fable review)

P1d is DONE and on master (commit beeb86af), green clang + gcc. Built exactly to
the A2-REVISED ruling. Two new domain-agnostic toolkit capabilities, both
headless-tested (UI.Toolkit.Tests):

- `SplitView::SetPaneCollapsed(SplitPane, bool)` + `IsPaneCollapsed` /
  `AnyPaneCollapsed`: the collapsed pane measures at its content minimum along
  the split axis (`{w,w,0,h}` loose measure), the divider hides + stops
  hit-testing, and the stored ratio is preserved for restore. ONE container, no
  reparenting - the F1 invariant holds.
- `BottomDock` (`:bottom_dock`): always-visible tab bar + expandable content
  region above it; `AddTab(id,label,content)` (content borrowed), click-active-
  collapses / click-other-switches+expands, `OnExpandedChanged(bool)` event,
  `ActivateTab`/`ClickTab`/`SetExpanded`. N-tab shaped; one "Animation" tab today.

Scene page: viewport column is now `[viewport / BottomDock]` vsplit; the dock
hosts PropertyAnimationPanel as the Animation tab and drives the split's
pane-collapse through OnExpandedChanged (captures the page, not a RefPtr, so no
event->split->dock->event cycle). Starts collapsed. Per-scene-page. The panel
lost its own collapse caret (the dock owns collapse); its header shows only when
the tab is expanded.

Not done (deferred as ruled): keyboard toggle + collapse-state persistence
("polish later" per A2-REVISED); active-tab visual highlight in the bar (one tab
today - the expand state is self-evident). P2 (dopesheet) proceeds from here.

## OPEN for Fable: clip edits dirty the SCENE (command-stack coupling)

Found in UAT (user, 2026-08-17): editing a clip in the in-scene panel marks the SCENE
document dirty.

Cause: the panel borrows the scene page's `EditorCommandStack`
(`m_editContext->Commands()`), passed in at construction (P1b). That stack's `OnChanged`
is wired to the scene page's dirty flag, so every clip edit (key drag, duration,
add-track) that executes a `ClipEditCommand` fires the scene's `OnChanged` and dirties
the scene - even though the clip is a SEPARATE asset with its own dirty flag
(`PropertyAnimationPanel::m_dirty`) and its own Save. The clip changes are not even in
the scene document. The retired standalone clip page had its OWN command stack
(`EditorPage::Commands()`), so this is a P1b regression from sharing the scene's stack.

Proposed fix (needs a ruling): the panel OWNS its command stack (a value member;
`Commands()` returns it), so:
- clip edits dirty only the clip (via `MarkClipDirty` -> `m_dirty` -> Save), never the
  scene;
- clip undo/redo is an independent context (matches the old standalone page + how asset
  editors generally scope undo);
- the F1 lifetime invariant is even cleaner (command + stack + host all die with the
  panel).

Trade-off: the scene page's global Ctrl+Z (which operates on the scene's stack) would no
longer undo clip edits. Clip-edit undo would need its own affordance - panel-focused
Ctrl+Z routing, or undo/redo buttons in the panel header. Options for the ruling:
1. Panel owns its stack; add panel-scoped undo (focus-routed Ctrl+Z or header buttons).
   RECOMMEND - correct document ownership, matches the standalone page.
2. Keep the shared stack but suppress the scene-dirty for clip commands (tag clip
   commands; the scene's OnChanged ignores them). Keeps unified undo; more coupling.
3. Accept dirtying the scene (status quo). Simplest; but Save-scene-to-persist-a-clip is
   misleading and the clip edits aren't in the scene.

Not yet built - awaiting the ruling.

## SEDULOUS-SHAPE ADOPTION (Fable, 2026-08-17, user-approved; SUPERSEDES the stacked track rows)

The user asked whether Sedulous's model (labeled dopesheet + click-a-key inspector) beats the
stacked per-track sections below the dopesheet. Verdict after reading Sedulous
PropAnimPageBuilder/TimelineView: YES - the stacked sections duplicated the track list, burned
~140 px per track, and could not survive a ten-track clip. Adopted + built:

- **The dopesheet IS the track list.** Timeline lanes carry labels
  ("Component.property", 140 px gutter); a gutter click - or any key pick - selects the
  TRACK (Timeline::SelectedLane/OnLaneSelected; programmatic SetSelectedLane never fires).
- **The area below shows the SELECTED track only**: one strip (the merged path field, kind
  cycle, interpolation cycle, Key, Key All, remove) + the KEYFRAME INSPECTOR (Time field +
  typed value fields: V / XYZ / RGBA / euler-degree PYR - Sedulous's inspector over OUR undo
  stack, one step per commit; edits UPSERT at the selected time so a channel missing a key
  there gains one) + ONE CurveCanvas for the selected scalar track. Quat tracks get no
  canvas (timing = dopesheet, values = inspector).
- **Key-from-scene capture is the PRIMARY value workflow** (the range-trap answer): pose the
  entity, press Key (per track) or Key All - IClipEditorHost::ReadSceneValue (the panel's
  ReadTrackTarget over the primary selection) captures at the playhead; keys upsert at that
  time as one undo step. Type mismatches warn + skip.
- **The canvas is a value VIEWPORT, not a value limit**: YToValue is unclamped (drags
  extrapolate past the framed range - the old clamp made out-of-frame values unreachable),
  wheel = value zoom about the cursor, middle-drag = value pan; both pin the frame
  (AutoFitValueRange off). The time axis stays the clip's (ruling 2).
- The inspector lives in a persistent row whose CHILDREN swap on selection
  (RefreshKeyInspector, deferred via the mutation queue) - canvas picks never trigger a full
  rebuild, so tangent handles survive selection changes.

## CLIP-EDITING WORKFLOW (user-designed 2026-08-17; BUILT same day)

The clip lifecycle rework: every edit has an asset home from the first keystroke, and every
open goes through one shared path. Replaces the scratch-clip model (New used to author into
an unsaved buffer where Save could silently have no target).

- **No scratch clip - the empty state is EXCLUSIVE.** With no clip loaded the panel shows
  ONLY "No animation clip selected for editing." + Create Clip... / Open Clip... (header,
  transport, dopesheet, and track editor are all Gone). No tracks can exist without an
  asset to save them into.
- **Create Clip...** opens the type-agnostic `app::AssetCreateDialog` (the picker's twin:
  choose a GROUP in the tree + type an ASSET NAME, live validation refuses empty or
  already-taken names). The dialog never creates anything - `OnCreate(group, name)` fires
  and the caller constructs its type (`CreatePropertyAnimationClipNamed`) then loads by the
  new instance's GUID (guids are the open/identity currency). Reusable by any New-asset
  flow.
- **Open Clip...** = the existing AssetPickerDialog filtered to PropertyAnimationClipAsset.
- **Dirty guard on every load-over path** (`RunDirtyGuarded`): a modified clip prompts
  Save / Discard / Cancel via the reusable `app::ConfirmDialog` (Escape = Cancel; a FAILED
  save aborts the switch instead of discarding on top of it). Headless (no UIContext)
  proceeds - the caller decided.
- **The pencil claims the open.** `EditorContext` gained an open-asset interceptor registry
  (`AddOpenAssetInterceptor` -> u64 id / `RemoveOpenAssetInterceptor` /
  `TryInterceptOpenAsset`, consulted NEWEST-FIRST, first true wins); the app's OpenAsset
  consults it before `OpenInstancePage`. The scene page registers a claim for
  PropertyAnimationClipAsset gated on the page being ON SCREEN (`IsEffectivelyVisible` -
  a background scene page never swallows an open): it expands the bottom dock
  (`ActivateTab("animation")`) and routes `RequestEditClip(guid, primary-selection)`.
  Any unclaimed open falls back to the generic asset page. The claim is removed in
  OnClose (+ a destructor backstop - the interceptor captures a raw page pointer).
- **The BOUND ENTITY replaces live selection as the edit target.** Preview, key capture
  (ReadSceneValue), and track seeding all target an explicit `m_boundEntity` (session
  state, not serialized, survives clip switches). The header slot shows
  "Entity: <name>/(none)/(missing)" + Bind... (EntityPickerDialog, injected by the scene
  page through the `RequestEntityPick` seam - the panel cannot depend on editor.scene) +
  Use Selected (the ONE place selection enters). The pencil claim auto-binds the primary
  selection. Rebinding stops + restores any live preview first.
- **Timeline theme rules**: Timeline resolved Background/Border/TextDim/Accent/Error via
  ResolveStyleColor but no sheet rule existed, so it always fell back to its hardcoded
  palette. Both toolkit-dark.sss and toolkit-light.sss now carry a Timeline block
  (mirrors CurveCanvas + accent/error), so it follows the editor theme.

DEFERRED (noted, not built): a dirty-save prompt on scene-PAGE close - the page-close path
has no veto seam today (OnClose is unconditional); adding one is an editor-shell change,
not an animation one. The in-panel guard covers every load-over path meanwhile.

## CURRENT STATE + REMAINING WORK (Fable handoff, 2026-08-17 - THE RESUME POINT)

Fable's takeover rounds are complete through commit ae7c762a; the track returns to Opus
here. Everything below "SHIPPED" is on master, battery-green on clang+gcc, and
user-verified on screen ("works great"). Read the two 2026-08-17 sections above
(SEDULOUS-SHAPE ADOPTION, CLIP-EDITING WORKFLOW) for the design rationale; this section
is the ground truth of what exists and what is next.

### SHIPPED (do not re-plan; verify against code if in doubt)

- **P1 + A2 REVISED**: persistent panel below the viewport inside the Godot-style
  BottomDock ("Animation" tab, collapsible, draggable splitter). Transport
  (Play/Pause/Stop/Loop, editor-local loop default = on), playhead, live preview with
  snapshot/restore (#5 UAF + #6 stale-snapshot fixed by construction).
- **Sedulous shape (P2 core)**: the dopesheet (ui.toolkit Timeline) IS the track list -
  labeled 140px gutter, SelectedLane/OnLaneSelected, key picks select their lane. Below
  it, the SELECTED track only: edit strip (merged "Component.path" field, kind cycle,
  interp cycle, Key, Key All, remove), persistent keyframe inspector (Time + typed
  V/XYZ/RGBA/euler-PYR fields; commits upsert at the selected time, one undo step),
  ONE CurveCanvas for scalar tracks, quat keys-strip for rotation. Key-from-scene
  capture (ReadSceneValue over the BOUND entity) is the primary value workflow.
- **Timeline widget** (foundation.ui.toolkit, domain-agnostic per ruling 4): lanes +
  labels + diamonds, ruler with index-based %.4g ticks, playhead, box-select, key drag
  (visual-then-commit), zoom, middle-drag/Shift-wheel pan, ClampScroll (end + 15% tail),
  whole-widget + gutter clipping, theme-sheet rules in toolkit-dark/light.sss.
- **CurveCanvas on seconds** (ruling 2, partially - see D1 below): TimeSpan seconds
  axis, px-per-second tangent projection, unclamped value viewport (wheel value-zoom,
  middle-drag value-pan), OnSelectionChanged, draw clipping.
- **F1 CLOSED**: ClipEditCommand holds IClipEditorHost (the durable seam), never the
  recreatable view. Undo/redo routes ApplyClipState through the host.
- **Clip-editing workflow** (section above): exclusive empty state, AssetCreateDialog +
  ConfirmDialog (both reusable, in editor.app), RunDirtyGuarded on every load-over
  path, EditorContext open-asset interceptors + the scene page's pencil claim
  (visible-page gated, auto-binds primary selection), the BOUND-ENTITY slot
  (Bind... via RequestEntityPick seam / Use Selected), RequestEditClip entry.
- **Registration facts**: Tools.Editor main calls foundation::core::RegisterCoreTypes()
  at startup (Transform bindings; do NOT rely on ScriptSubsystem's lazy call);
  PropertyAnimationClipFactory is in DefaultApplication's standard set
  (kStandardHeadlessFactoryCount = 19).

### REMAINING WORK (in priority order)

1. **D1 full time-transform share - DONE (Opus, commit 73f9b68c).** CurveCanvas gained
   a shared-transform mode (UseSharedTimeTransform + PixelsPerSecond + ScrollSeconds
   mirroring the Timeline); TimeToX/XToTime, the tangent projection + its inverse, the
   polyline sampling (screen-x space now), and the x-axis labels all route through one
   PixelsPerTime() + XToTime, so the curve follows the dopesheet's zoom/scroll at any
   pan. Timeline gained OnViewChanged (pan/wheel/zoom scroll writes routed through
   SetScrollSeconds); IClipEditorHost gained TimeAxis + ClipTimeTransform() (panel
   returns the dopesheet Timeline's pps/scroll/label-column); AddCurveCanvas insets the
   canvas by the 140px gutter so t=0 aligns under the lanes; the panel pushes the
   transform on OnViewChanged (ClipEditorView::SyncCanvasTransform, Invalidate-on-change).
   Legacy fit-to-width stays the default for other callers (37 canvas tests unchanged).
2. **Ruling 1 commit-returns-remap** - key selection across a commit is still
   re-derived by time-with-epsilon everywhere (BuildLanes ReselectMark). The commit
   site should return the index permutation for the direct-move path; time-epsilon
   stays only for undo/redo snapshot restores. Only matters once coincident key
   times show up in practice; do it when touching the commit path anyway.
3. **A4 loop default** - transport Loop is editor-local (default on). Seeding it from
   the bound entity's PropertyAnimatorComponent instance (now that a BOUND entity
   exists, the seam is there) means taking an engine.animation dep in the editor
   panel - decide, then either wire it or stamp it permanently-editor-local.
4. **A9 lane scrolling/virtualization** - the Timeline pane height is clamped
   (kTimelineMaxHeight 220px); a clip with many tracks clips its lanes with no
   vertical scroll. Needs a scroll strategy before big clips are authorable.
5. **Scene-page-close dirty prompt** - deferred: OnClose has no veto seam (see the
   workflow section's DEFERRED note). Needs an editor-shell page-close-veto seam
   first; the in-panel guard covers load-over paths meanwhile.
6. **P3 polish backlog** (unstarted, from the phasing plan): copy/paste keys,
   scale/ease ops, bezier handle modes, snap toggles, viewport path preview
   (Traktor-style), box-select in the CurveCanvas.
7. **P4 asset-slot activation track** - the animator component's slot pencil now
   routes into the panel; the remaining P4 items (activation affordances beyond the
   pencil) ride the inspector track.

### Working rules that bit us (obey them)

- Full battery = build clang+gcc (`-k 0` AFTER `--`), run every *Tests binary from a
  scratch cwd; ALL_GREEN before commit. Net.Tests is timing-flaky under load - rerun
  standalone before diagnosing.
- Every duration read = Max(clip.duration, ComputeDuration()) - never one alone.
- ui::Visibility::Gone (there is no Collapsed).
- Timeline/CurveCanvas hit-test helpers run auto-fit BEFORE testing - pin
  AutoFitValueRange=false in tests; panel dopesheet drag tests offset x by
  Dopesheet().LabelColumnWidth.
- Linked-time multi-channel drags fire OnKeyChanged per channel - only honor the
  selected key's event.
- The panel must never depend on editor.scene - page-side features inject through
  seams (RequestEntityPick is the model).
