# Smoke checklist — outstanding items (as of 2026-07-27)

> Failing items from the 2026-08-08 weekend pass are diagnosed + specced in
> docs/specs/smoketest-fixes.md (item numbers noted inline). Passed items pruned.
>
> Weekend-pass status (2026-08-08): #1 #2 #3 #5 #6 SHIPPED + #7 UX SHIPPED (bespoke
> collision-shape page) - all ready for RETEST (commits noted on each item). Still open:
> #4 (deferred to issues-triage I1), #7 import-cook (awaiting an editor repro; a diagnostic
> log now names the cause), #8 (AS/Wren parity diagnostic), #9 (Sandbox game-UI samples).
> #10 is retest-on-new-surface bookkeeping (static Physics facade retired).

Pruned as confirmed: the ENTIRE prefab track (P1–P4 incl. nesting, rebuild rescue, sibling
order, nested-placement propagation, wire-refusal degrade), materials unification, the
responsive + auto-cooking import flow, and cook-on-open with request coalescing. Only the
long-standing stragglers remain. 2026-07-27 pass pruned: Game tab play/stop/
restart + game script, the embedded runtime host (all), Pause/Res polish, the
Simulate-stop hang + gizmo-follows-simulation fixes, audio P1 core (emitters/doppler/
music isolation), game-UI P1 HUD click consumption, post-fx no-regression + inspector +
tonemap, scene XML round-trip basics + small-diff saves, and the LMB crate shove
(root-press consumption fix + cursor pick-ray, verified on screen). 2026-07-28: the ENTIRE CodeEditView P1 section (script + UI-document panes on the new code editor incl. the two first-run fixes: caret auto-scroll vs stale scrollbar max, gutter clip) verified and pruned. P2 syntax highlighting fully verified same day (+ the per-region cursor fix). 2026-07-29: P3+P4 fully verified (incl. the retest fixes: two-row find bar, punctuation key bridge, pointer tooltips without focus steal, completion ranking tiers + overflow indicator, ToggleButtons). CodeEditView track COMPLETE.

## Name changes since these items were written (debrand) - substitute when running commands

Older items reference pre-debrand names. The features are unchanged; the binaries,
targets, and env vars are not. Substitute:

| Item says | Run instead |
|---|---|
| `DraconicExport` / `Draconic.Tools.Export` | `Tools.Export` |
| `DraconicPlayer` / `Draconic.Engine.Player` | `Engine.Player` |
| `Draconic.Tools.Editor` | `Tools.Editor` |
| `Draconic.Tools.ShaderPack` | `Tools.ShaderPack` |
| `DraconicSample001_Triangle` | `Sample001_Triangle` |
| `DraconicSmoketest` | `Smoketest` |
| `DRACONIC_USE_SHADER_PACK` | `OPTION_USE_SHADER_PACK` |
| `DRACONIC_WEBGPU_WGSL` | `ENV_WEBGPU_WGSL` |

Also: web export template ids were debranded (77ada6c3) - an installed
`draconic-web-debug-*` template on this machine is stale and needs re-creating
before the web session.

2026-08-15 audit pass: 14 functional items below are checked off against automated
test evidence (cited inline as TEST-COVERED); 2 items struck as superseded. Visual/
audible/interactive items were never auto-checked - those remain for user sessions.

## Text scenes (0ba08e9) — scenes/prefabs are now XML sources
- [ ] A prefab OVERRIDE (e.g. a changed material or health-style field on an instance
      member) appears in the scene XML as readable fields inside its componentOps record
      — not a hex blob.

## Input P1 (8bf26b1) — the action layer
- [ ] **InputActions full stack (fd93a68) — best on the Steam Deck:** the ImGui panel
      shows live Move/Jump, a time-scale slider (Move slows/pauses with it, Jump's PRESS
      still registers at 0), the exclusive-menu toggle, and REBIND rows: Rebind Jump →
      press any key/button (Esc cancels), survives an app restart (user-file overlay),
      Reset restores Space. On the Deck: drag in the blue left zone = floating stick
      moves the color; tap the orange zone = Jump; finger circles track your touches.
- [ ] **InputMapPage finished:** per-binding source button cycles valid sources; the
      detail line edits dead zone/scale/invert/pad/region fields (click a number to
      edit); WASD composite rows capture each direction key separately; processor and
      interaction-seconds fields edit per action — every edit one undo step.
- [ ] Run `Bin/Debug/Linux64-Clang/InputActions`: WASD (smoothed, ramps like a stick) and
      the left pad stick shift the window color; Space/pad-south logs "Jump!". Escape
      opens the exclusive Menu: movement freezes, Space now logs "Confirm." — and a key
      HELD across the menu boundary (either direction) never re-fires until re-pressed.
- [ ] **Input Map page (c8bf24e):** double-click the asset — an outline of sets > actions
      > bindings. Renames in place (double-click a name), kind/interaction buttons cycle,
      +/- nudges set priority, "+ Binding"/"x" add/remove, and every action is ONE undo
      step (Ctrl+Z). "Listen" waits for the next input (filtered by the action's kind —
      stick drift can't bind a Button; Esc cancels). Save refuses an invalid map with a
      clear error.
- [ ] **Play-in-editor input (c8bf24e):** set the project's default input map (cook it),
      Game > Play, CLICK THE VIEWPORT (focus gates the keyboard) — a game.wren using
      `Input.isDown("Jump")` / `Input.value("Move")` reacts to WASD/pad; typing in other
      editor panels never leaks into the game; Stop restores normal editor input.

## Play-in-editor polish (4abd862)
- [ ] On the Deck (or any touchscreen): touch bindings work INSIDE the Game tab's
      viewport - fingers on other editor panels don't leak into the game, and coordinates
      match the game's own regions (not the window's).

## Physics P2 (cff7c0b) — collision assets + import + scripting
- [ ] Editor: New Asset > Collision Shape — it now opens the BESPOKE page (not the generic
      form): pick the source mesh via the typed "Pick..." picker (StaticMesh/SkinnedMesh,
      no guid string), toggle cook kind (convex hull / triangle mesh), "Cook now"; the
      status line names the mesh. Set a RigidBody to shape=Cooked + reference the shape;
      Simulate shows the outline wireframe + collision matching the mesh. Reimporting the
      model recooks the shape. A RigidBody with shape=Cooked + a nil shape ref now shows an
      amber inspector warning row. -> #7 SHIPPED 891b3647 (UX) - retest
- [ ] Model import dialog: "Generate collision" (+ "Convex collision") toggles — import a
      Kenney prop with it on; the generated prefab drops into a scene as a collidable
      static prop (Simulate: crates rest on it). -> #7 IMPORT-COOK still open: not
      reproduced headless (pipeline cooks both kinds clean); a diagnostic log now names the
      cause on the empty-vertices path (PhysicsAsset.cppm). If this still "cooking failed",
      grab the log line - that IS the bug to fix.
- [ ] Game tab / player Wren script: raycast/hit-surface/impulse work from game scripts.
      -> #10 the static `Physics.*` facade was RETIRED; retest on the new surface:
      `ScenePhysics.of(scene).rayCast(...)` / `.hitSurface()` / `.impulseOnHit(...)`.

## Physics P3 (049bdd5 + 4909a1e) — character, joints, groups, matrix editor
- [ ] PhysicsPlayground: drive the capsule with ARROW KEYS (Space jumps; HUD shows
      grounded/airborne) — it climbs the ramp panels, stairs onto small ledges, pushes
      crates around, and gets batted by the spinning hinge blade near the back.
      -> #3 SHIPPED 41d105ef: CharacterComponent.strength authored knob (reflected +
      inspector + live) + playground tuning so the shove reads - retest
- [ ] Editor joints: entity with RigidBody + Joint component (Hinge, motorEnabled,
      target nil) under no ancestor → spins in place on Simulate; child of a body entity
      with a Distance joint hangs from its parent; changing motorTargetVelocity in the
      inspector mid-sim takes effect live. -> #6 SHIPPED 50dee8f1: the live motor edit now
      WAKES driven bodies (a sleeping body ignored the target-velocity change) - retest
- [ ] Editor character: add a Character component to an entity, Simulate — it drops,
      lands, debug-draws as a capsule (cyan grounded / purple airborne). -> #4 DEFERRED to
      issues-triage I1: DrawPhysicsDebug only draws POST-Simulate (bodies exist only then);
      the real fix is component-data collider draw + a Simulate-path ordering fix, larger
      than a show-flag toggle. Not shipped this pass.
- [ ] Collision matrix: Physics settings section shows "Collision Groups" — rename the
      default row, Add Group, toggle a pair off (symmetric), assign collisionGroup 1 to
      a crate → on Simulate it falls through bodies in the disabled-pair group; each
      matrix click is one undo step; groups round-trip through scene save (XML). -> #5
      SHIPPED fbf9020f: the matrix now REBUILDS on Add/rename/remove Group (mutation-queue
      deferred) - retest
- [ ] Wren: character move/jump/grounded drive the scene's character from a game script in
      the Game tab. -> #10 retest on the new surface: `CharacterComponent.of(entity)`
      (the static `Physics.*` facade was retired).

## Game-UI P1 (f78ffc8 + 64b8243) — screen-tier canvases + consumption
- [ ] Scene flow: add a `ui.Canvas` component to an entity (inspector: document picker),
      point it at a cooked UI Document, Game > Play — the UI renders over the game in the
      tab; the same project in DraconicPlayer shows the identical overlay. -> #2 SHIPPED
      75e79d16 + d73970fe: Project Settings now has a "Default UI font" FontAsset picker;
      a project with font assets but nil defaultUiFontId logs ONE actionable warning (Game
      tab + player) instead of rendering no glyphs. VERIFY the .ttf/.otf drop-import path
      in a real project as part of retest.
- [ ] Wren + UI: while a menu is under the mouse, `Input.isDown` actions bound to mouse
      read released; gamepad actions keep working.

## Game-UI P2 (2fa3bc7..8c7b978) — warnings, billboards, screen tier, nav, UI page
- [ ] Screen tier: overlays pushed via PushScreenOverlay live outside any scene — they
      persist across scene swaps and always draw topmost.
- [ ] Gamepad nav: with a pad, dpad/left-stick moves focus between HUD buttons
      (hold repeats), South presses the focused button, East acts as Escape.

## Overlay roles refactor (a78ec94) — scene UI in the compose, screen UI per window
- [ ] Billboards in a non-primary view: open the same scene in two pages with different
      cameras — nameplates project correctly per view (previously primary-camera only).
      -> #10 opening one scene in two pages is an editor limitation (by design for now);
      retest billboards against the shipped CAMERA PREVIEW inset (#118), a true second view
      of the same scene. If that passes, the item is satisfied.
- [ ] Screen tier: a pushed screen overlay (loading screen) draws over every scene in
      the player window AND the Game tab viewport, topmost.
- [ ] Split-screen scene HUD (d03c8fd): render one scene through two RenderScene calls
      with side-by-side viewports — each half shows its own correctly-placed HUD,
      clipped to its half (no bleed across the seam); billboards center per half. -> #9
      NOT STARTED / BLOCKED on it: Sandbox already renders two views but exercises no
      game-UI. Plan (smoketest-fixes.md #9): add a toggleable UI block to Sandbox (scene-
      tier ui.Canvas HUD + billboard nameplate + screen-tier badge, WebScene font recipe);
      then this item runs as written in Sandbox's two-view mode. Sample-side work.

## Audio P1 (merge 8b8b334 + fixes ..047d12e) — VERIFIED by user 2026-07-18
- [ ] T (scene pause): bed + emitters fade out together; LMB one-shots still play
      (one-shots bypass the scene group).
- [ ] Pitch check: the four emitters are the SAME clip at 0.75/1.0/1.5/2.0 — four
      different notes proves real resampling (Sedulous played everything at one speed).

## Audio P2 (e4cafcd..6ba5f5b) — audition, muffling, cross-fade, mixer data
- [ ] Bus layout: New Asset > Audio Bus Layout, set music volume 0.3 + an effects
      lowpass ~1500 Hz, cook, set it as Default bus layout in project settings —
      the player and Game tab start with quiet music and muffled effects.
- [ ] User volumes: run the player, change a bus volume from script
      (Audio.setBusVolume(AudioBus.Music, 0.1)), quit, relaunch — the volume persisted
      (<userdata>/<project>.user.settings.xml).
- [ ] Wren: game script calls Audio.setBusVolume/busMuted/stopMusic without faulting.

## Game-UI P3 (merge 3c35f15) — order/scaler/typing/RT/theme
- [ ] Order: two overlapping canvases, different `order` — higher draws on top and
      takes the click.
- [ ] Scaler: canvas set to ReferenceResolution 1920x1080 — resize the window: UI
      scales uniformly, centered with letterbox bars; buttons still hit correctly.
- [ ] Typing (player): canvas with an <EditText> — click it, type (incl. dead keys);
      keys reach gameplay actions only while NOT focused.
- [ ] Typing (Game tab): same doc in the editor — focus viewport, type into the field;
      editor panels don't receive the keys and editor typing doesn't leak into the game.
- [ ] Default theme: Project Settings > Default UI theme > pick a cooked UITheme,
      restart — game UI restyles (Clear = GameTheme).
- [ ] RenderTexture: set renderMode=RenderTexture on a canvas + assign its view to a
      sprite from code — the UI appears on the sprite in-world.

## Audio P3 + UI completion (15c5df8..merge)
- [ ] AudioPlayground: LMB shots now vary (3 clips, weighted, pitch-jittered, never the
      same twice in a row); fly to the ORIGIN — reverb tail fades in (the cave), fly
      out — it dries.
- [ ] Editor: New Asset > Sound Cue — SoundCuePage opens (8 slots + Pick/weights +
      mode/jitter); Audition plays a resolved variant and reports slot/pitch/vol; a cue
      with no clip FAILS the cook; assign the cooked cue to an AudioSource's `cue`
      field (picker) — Play triggers vary.
- [ ] audio.ReverbZone component in the inspector; listener inside → Effects reverberate.
- [ ] Editor keystrokes no longer leak: editing viewport HUD renders but ignores
      clicks/typing (gizmo keys keep working); same during Simulate.
- [ ] Game tab binding: HUD click fires ONLY in the playing Game viewport; a ScenePage
      beside it with UI at the same coordinates never reacts; EditText typing follows
      the game only while playing.
- [ ] RT canvas + Sprite on one entity: live UI texture in the EDITING viewport,
      updates on document edit, survives renderTexture resize, reverts on component
      removal.

## World tier (acf506e) — ui.WorldPanel
- [ ] Editor: Add Component > ui.WorldPanel, pick a document, set sizeMeters — the
      panel appears in the scene (WYSIWYG; interactive only in the Game tab / player
      per the scene-binding rules).

## Audio niceties (merge, 2026-07-19)
- [ ] Named bus: bus layout with a custom slot (drums, parent effects, lowpass 1200) as
      project default; an AudioSource with busName=drums sounds muffled while other
      Effects stay clean; Audio.setNamedBusVolume("drums", 0.2) dims just it.
- [ ] Faded steal: tiny voice pool (SetAudioEngineSettings voiceCount 4) + one-shot
      spam — stolen voices fade ~30 ms, no clicks.
- [ ] Playhead: clip page playhead tracks the TRUE cursor (looping clip visibly wraps;
      pitch changes no longer drift it); cue page shows seconds while auditioning.
- [ ] Reverb send: reverbSend 0.6 on one source — it carries a tail while a dry source
      beside it doesn't; zones still wet the whole scene on top.

## Export: templates + (platform, config) axis (8f0fafd + merge 9b94341)
- [ ] **DraconicExport CLI parity:** `DraconicExport <project> --template list` shows the templates;
      `--template import <file>` imports one; `--preset <name>` / `--all` export using the
      (platform, config) template — same staged output as the editor path.

## Export: editor UI (15c695a) — presets panel + templates manager
- [ ] **Edit > Export Templates…** lists every template (installed + this editor's own build) by
      its icon, name, platform, config and engine version, a warning icon on a stamped version
      that differs from this build; the selected one's details on the right. Install from
      Folder… (folder picker → ImportTemplate), Create from Build… (folder picker on a
      `Bin/<Config>` dir → CreateTemplate), Reveal Folder, Remove… (asks first; never the host
      build). Each action refreshes the list in place.
- [ ] **Project > Export…** lists the presets, each by its resolved template's icon (a warning
      when none is installed); the selected one's settings in General / Content / Display tabs
      under the card naming its template. Edits stick when switching presets and after reopening.
      Export Templates… opens over it, and the cards follow an install or removal.

## Export: "Always Export" roots + badges (f22bc1a + 412c789 + 5ee987f)
- [ ] Right-click a GROUP (row or left tree) > **Always export contents** — the group gets the
      green dot; the flag stores the group PATH (open export_roots.xml). Drop a new asset into that
      group, export via CLI with `pruneToReachable`: the new asset is included (dynamic membership).
- [ ] **Editor pruning (cee4a13):** in the editor, set a preset's "Prune to reachable content
      only", flag an otherwise-unreferenced asset Always Export, File > Export — the exported
      player is pruned (unflagged/unreferenced assets absent, flagged + referenced present),
      export-report.txt lists the roots. The export stays non-blocking (status-bar progress). A
      preset WITHOUT pruning still ships everything. (The reachability closure is computed on the
      main thread before the background job — brief pre-scan pause on huge projects is expected.)

## Export: reachability pruning — CLI (Phase 1, merge)
- [ ] **Scene-edge scanning:** a resource referenced ONLY through a scene component (e.g. a
      RigidBody's collision shape, an AudioSource clip, a ui.Canvas document) survives the
      prune — SceneReferenceScanner bridges scene→asset edges. Nothing referenced goes missing.

## ParticleEffect editor page (e1fe2311) — three-pane authoring tool
- [ ] Tree: right-click menus add/delete initializers, behaviors, and systems;
      Move Up/Down AND drag reorder modules within their folder; double-click a System
      row renames it in place. Every structural change updates the preview immediately
      and reselects the touched node.
- [ ] Inspector completeness: System node shows name/sim/space/blend/render,
      max particles (edit restarts the sim at the new budget), sort/soft/prewarm, LOD,
      full Flipbook + Trail groups, and a texture picker (AssetPickerDialog). Emitter
      node shows mode/rate/duration/looping + burst count/interval/cycles.
- [ ] Curves: Alpha/Rotation/Speed-over-lifetime show an interactive CurveCanvas
      (drag keys, tangent handles); Size shows a two-channel (X/Y) canvas;
      Color-over-lifetime shows a GradientEditor (drag stops, double-click = color).
      Edits change the running preview.
- [ ] Transport: Play/Stop/Restart/Pause behave; the speed slider slows/speeds the
      sim; the stats line tracks alive/cap per system live; the emission-shape gizmo
      draws for the selected system and follows shape-type/radius/extents edits.
- [ ] Undo: scalar scrubs coalesce to one Ctrl+Z step; add/remove/reorder module and
      system are each one undoable step; undo/redo rebuilds tree + inspector and the
      preview matches. Save recooks ([cooked] badge refreshes); reopen shows the edits.

## AnimationGraph + AnimationClip editor pages (bespoke pass 4) — NEW, needs first on-screen run
- [ ] Asset Browser: New Asset > Animation Graph creates a seeded graph (Base layer, Idle default
      state, Speed float param) that cooks. Double-click opens: LEFT Layers+Parameters lists,
      CENTER the state-machine canvas above a preview strip, RIGHT inspector.
- [ ] Canvas: right-click empty = Add Clip/Blend1D/Blend2D state; node menu = Make Transition
      (rubber arrow follows the mouse, click a target state; Esc cancels), Set as Default
      (header turns orange), Delete State (transitions remap). Dragging nodes persists across
      save/reopen. A->B AND B->A transitions draw as offset arrows; selecting an edge shows the
      transition inspector (duration/exit time/priority + conditions param/op/threshold).
      (Create-then-open crash + empty-space-click deselect/clear-inspector: verified 2026-07-27.)
- [ ] Inspector: state name/speed/loop edit; Clip state picks an AnimationClip; Blend1D/2D pick
      driver params + entries (threshold/position + clip each). Parameter type switch changes the
      default-value row. Every scrub coalesces to one Ctrl+Z; structural ops undo cleanly.
- [ ] Live preview: pick a Skeleton (imported model) - the bone wireframe plays the DEFAULT state;
      scrub the parameter's "Live (preview)" value so a condition passes - the transition fires,
      the ACTIVE state ring moves on the canvas, the status line shows "(transitioning)".
- [ ] AnimationClip page: double-click an imported clip - stats + loop flag + events; pick a
      skeleton: the wireframe plays; the slider scrubs (pauses playback); "+ Add Event" lands at
      the playhead; events survive save/reopen and undo works.

## Skeleton page (bespoke pass 5) — NEW, needs first on-screen run
- [ ] Double-click an imported Skeleton asset: LEFT bone tree (stats line above: bones/roots/
      depth), CENTER the bind-pose wireframe on a grid (orbit camera), RIGHT selected-bone info.
- [ ] Click bones in the tree: the orange marker jumps to that joint in the wireframe and the
      info pane shows index/parent/children/bind TRS. Reimporting the model refreshes the page.
## AudioBusLayout page (bespoke pass 6) — NEW, needs first on-screen run
- [ ] Double-click a Bus Layout asset (or New Asset > Audio Bus Layout): LEFT bus tree
      (Master with Effects/Music/UI under it), RIGHT the selected bus's volume/mute +
      lowpass/highpass/delay/reverb fields. Edits coalesce to one Ctrl+Z each.
- [ ] "+ Add Bus" claims a slot under Master; rename it (children follow; duplicate/fixed
      names are refused); re-parent it under Effects then under another custom bus - the
      dropdown never offers a choice that would cycle. Remove re-parents its children to
      Master. Everything survives save/reopen and the asset cooks clean.

## Image page (bespoke pass 7) — NEW, needs first on-screen run
- [ ] Double-click an Image asset (a dropped .png/.jpg source): LEFT the decoded preview +
      a facts line (file, WxH, pixel format, size); RIGHT Color Space (sRGB/Linear) +
      read-only source rows. An HDR source previews clamped; a missing file shows the
      "no preview" note instead of a blank page.
- [ ] Flip Color Space, Ctrl+Z restores it (row updates in place), Save recooks
      ([cooked] badge refreshes) and a Texture built from the image follows the change.

## AngelScript debugger end-to-end (code-editor capstone) — NEW, needs first on-screen run
The AS backend debugger (suspension-based, battery-certified) was ALREADY live - only the
editor story pieces were new. AngelScript behaviors only (Wren has no debug API).
- [x] Open an AngelScript behavior's ScriptPage, click a breakpoint on a line inside
      update, Game > Play: when the line executes, the run pauses (sim freezes), the
      debugger panel shows the stack + locals, and the ScriptPage shows the YELLOW ARROW
      + row highlight on that exact line (view scrolls to it).
- [x] Step Into / Step Over from the debugger panel: the arrow follows each step; Continue
      clears the arrow and the sim resumes; hitting the breakpoint again re-arrows.
- [ ] While paused, HOVER a local variable's name in the ScriptPage: a tooltip shows
      "value : Type" from the live innermost frame; unknown identifiers and hovering
      after Continue show nothing.
- [ ] LIVE breakpoint toggling: while paused at a breakpoint, REMOVE it in the gutter,
      Continue - the game runs freely (no more stops) and the Locals panel goes quiet
      (no per-frame flicker; the game script no longer re-ticks while paused). ADD a
      breakpoint mid-run - it arms without a restart. Stop clears the arrow and
      hover-values immediately.

## Asset-name dedup (general, 2026-07-29)
One `Group::UniqueInstanceName` / `UniqueGroupName` ("Base", "Base.2", "Base.3", ...)
replaces every hand-rolled creator loop. Convention change: suffix is now DOT-separated
("Scene.2", not "Scene2"), and two reversed-digit bugs died with the old loops
(counter 12 used to produce "Prefab21").
- [ ] Capture prefab from an entity with a taken name: "EntityName.2"; from an UNNAMED
      entity: "Prefab" (the old loop lost the fallback and produced ".2").
- [ ] Reimport a model: its "Prefab" instance keeps its name + guid (reimport
      idempotency deliberately NOT swept - importers still reuse taken names).

## ScriptPage API browser (2026-07-29)
Traktor-style bound-API panel on the script page; completion + browser share one surface.
- [ ] Double-click a member row: its NAME is typed into the editor at the cursor (replacing
      any selection); one Ctrl+Z removes the whole insert. Double-click a type row inserts
      the type name.
- [ ] Editor-only marker: asset types the player never registers (e.g. the reflected
      *Asset classes) show " [editor]" on their type row in the browser AND on their
      completion label; runtime types (Float3, Entity, the facades) show no marker.
      Inserting a marked type still types the bare name.

Angelscript registers far more than Wren in the api browser. -> #8 NOT STARTED. Plan
(smoketest-fixes.md #8): the browser is truthful - the backends genuinely differ. Fix is
UPWARD, not clamping AS down: a test-time PARITY DIAGNOSTIC listing registry types AS binds
that Wren's closure misses; each triaged once (script-relevant -> add the Wren root/edge;
cook-only -> documented exclusion list). Test fails on an UNTRIAGED delta.

## WebGPU backend triangle (2026-07-29)
The fourth RHI backend's first pixels. Same DXC HLSL -> SPIR-V path as Vulkan.
- [x] Run DraconicSample001_Triangle --webgpu : a colored triangle renders,
      identical to the --vulkan run of the same sample. Resize the window; no
      validation errors print to the console.
- [x] DraconicSmoketest prints a WebGPU section listing all adapters with the
      RTX 2060 FIRST (DiscreteGpu), all OK lines, "device lost: no".

## Stragglers
- [ ] **Unreproduced crash watch (2026-07-28):** one-time Array index assert right after
      "cook finished" during code-editor testing (undo + save); never reproduced. The
      tooltip focus churn live at the time is since removed, and asserts now print their
      own backtrace - if it ever recurs, the console output pins it.
- [ ] FBX with separate metalness+roughness maps → baked "mr.packed.*" texture appears and
      the material responds to both channels — needs such an FBX.
- [ ] Import or delete WHILE a cook runs → "queued until the current cook finishes" toast,
      action replays automatically after.
- [ ] Save during a cook → the follow-up recook still runs (request no longer dropped).
- [ ] **Floating OS windows on layout restore** — with several restored floats only one
      renders; the rest render only after redocking (double-click title). The no-drag
      limitation of chromeless Wayland windows is expected; the no-render is the bug.

## 2026-08-01 - review-fix batch (Fable) + TAA-off flip fix + web export pipeline

Commits df21a8d0..3f27d38a plus the tonemap flip + web-export commits on top. Everything
below is code-verified + test-covered; these are the on-screen confirmations.

### Renderer regressions fixed (review)
- [ ] DEBUG DRAW ON THE BROWSER PATH: `DRACONIC_WEBGPU_WGSL=1 Sandbox --webgpu` (the
      forced-WGSL harness) -> debug gizmos/lines render (debug_geom was silently dead on
      the emulated push-constant path).

### Shaders pack/dev policy (review)
- [ ] DEV-FIRST: with a cooked Data/Shaders/shaders.dpak present (the ONLY place the host
      looks since 2026-09-15 - no exe-dir/cwd probing), run Sandbox, edit any Data/Shaders
      file -> hot reload still works (log says "runtime shader compiler", NOT "cooked shader
      pack").
- [ ] OPT-IN PACK MODE: `DRACONIC_USE_SHADER_PACK=1 ./Bin/.../Sandbox --vulkan` and
      `--webgpu` -> renders identically off the cooked pack (re-cook first if shaders
      changed: `Tools.ShaderPack Data/Shaders Data/Shaders/shaders.dpak spirv`). The webgpu run previously PANICKED on a desktop-cooked pack (vulkan1.3).

### VG gradient-LUT cache rework (the review blocker) + Opus's per-pixel gradients
- [ ] VGSandbox: gradients (linear/radial/conic) render with correct, stable colors across
      frames; radial/conic falloff is per-pixel smooth at low tessellation.
- [ ] ANIMATED gradient colors (or hover-recolored gradient UI): the ramp updates live -
      no stale/last-frame colors, no swapped ramps between two gradients (the raw-pointer
      cache-hit bug), and long runs don't grow GPU memory.
- [ ] Editor + UISandbox gradient-using chrome unchanged.

### Web input fixes (browser)
- [ ] Browser keys: F5 / F12 / Ctrl+C work while the app has focus; space/arrows still do
      NOT scroll the page.
- [ ] HiDPI display (if available): UI clicks land exactly under the cursor (previously
      offset ~2x on dpr-2 screens).
- [ ] Firefox: mouse-wheel speed is now comparable to Chromium (deltaMode fix - one item
      off the Firefox-compat list; the render-bundle strictness item remains).
- [ ] Two gamepads: disconnect the FIRST -> buttons on the remaining pad still route.

### WEB EXPORT PIPELINE (new - the export-template flow for the browser)
The web player no longer bakes any content; it FETCHES player.xml + Content.pak +
Data/.dataroot + Data/Shaders/shaders.dpak from the serving folder. The wasm build is a reusable export template
(already created + installed on this machine as 'draconic-web-debug-0.1.0').

- [ ] Editor flow: create an export preset with platform "Web" in the project's export
      settings -> Export. The output folder contains: Draconic.Engine.Player.html/.js/
      .wasm, serve.py, Content.pak, player.xml, Data/.dataroot, Data/Shaders/shaders.dpak (WGSL).
      (CLI equivalent: `Draconic.Tools.Export <projectDir> --preset <name>`.)
- [ ] Serve + browse: `cd <exportOut> && python3 serve.py` ->
      http://localhost:8000/Draconic.Engine.Player.html in Chromium -> the exported
      project boots, renders, and runs its game script.
- [ ] Re-export after a content change -> plain reload picks up the new Content.pak
      (serve.py sends no-store; no hard-refresh dance).
- [ ] Template refresh recipe (only after engine changes):
      `cmake --build build/wasm --target Draconic.Engine.Player` then
      `Draconic.Tools.Export --template create Bin/Debug/Emscripten-Clang --install`.

## 2026-08-01 (later) - WebScene exercise scene + remaining Y-flips + Sandbox particles

### WebScene: the full-renderer comparison scene (NEW - start every backend check here)
One scene, four ways to run it; they should all match:
- [ ] Browser: `cmake --build build/wasm --target WebScene` then serve
      Bin/Debug/Emscripten-Clang and open WebScene.html -> FIXES #10 (build fixed, retest)
Content checklist per run: sky+sun aligned under pitch/roll; sphere-grid PBR response;
CSM + spot + orbiting point shadows; SSR on the glossy floor reflecting the spheres
(ON by default now); decal projected FLAT on the floor at the right spot; 3 sprites
(alpha/additive/post-tonemap); fountain + spark trails; instanced ring; chrome sphere
showing the box-probe reflection; TAA on/off both upright.
- [ ] The ImGui panel appears and works - DESKTOP and IN THE BROWSER (the extension
      climbed to web): exposure/sky/post/feature toggles all live.
- [ ] THE SHADOW REGRESSION CLICK-TEST: panel -> uncheck "Sun shadows (CSM)" (and/or
      "Sun enabled") -> the spot + orbiting point shadows stay correct.

### Remaining Y-flip fixes (SSR + decal) - compare --vulkan vs the WGSL harness
- [ ] SSR reflections hang BELOW the spheres on both (previously mirrored on webgpu).
- [ ] The decal lands on the same floor spot on both (previously projected wrong on webgpu).
- [ ] Floor Metallic/Roughness sliders (Sandbox Environment window + WebScene Post
      section) retune the SSR reflection live - roughness fades SSR toward IBL at the
      cutoff, metallic sharpens/darkens the base so the reflection reads clearly.

## 2026-08-01 (latest) - WebGPU clip-space convention unified (26bd0902 + cbb09672)

The mirrored-WebGPU world model is DEAD. Root cause of every remaining desktop
--webgpu flip (mirrored geometry, particles emitting down, TAA toggling the sky):
the WGSL cook let naga bake a clip-space Y adjustment that wgpu's runtime SPIR-V
frontend does not apply, so the two WebGPU shader paths disagreed. Fixed by cooking
with naga --keep-coordinate-space; NeedsClipSpaceYFlip() is now FALSE on WebGPU
(all Y compensations dormant, Vulkan values everywhere); the front-face winding
inversion is gone. Pixel-readback proof: Draconic.Render.Backend.Tests - all 5
stages (raw/tonemap/tonemap+TAA/plane-only/cube-only) byte-identical between
Vulkan and WebGPU on BOTH shader paths.

IMPORTANT: old WGSL packs bake the adjustment - re-cook before testing
(`Tools.ShaderPack Data/Shaders Data/Shaders/shaders.dpak wgsl spirv`; the
clang + gcc bins and build/wasm bundles are already re-cooked/rebuilt).

- [ ] Browser WebScene (re-exported/re-served from the rebuilt Emscripten-Clang):
      identical to desktop --webgpu. NOTE first browser run after this change is the
      real Dawn-vs-wgpu-native conformance check - if Dawn renders it flipped,
      report it (would mean the two implementations disagree; probe can't see Dawn).

## 2026-08-01 (web resize + submit-drop fixes, d09d9ae8..9d42ded3)

Root causes from the browser log: (a) the canvas backing size flip-flops (template DPR
handler vs browser layout) and a mid-flight reconfigure destroyed the pending submit's
surface texture; (b) ImGui's one-frame-stale DisplaySize produced an out-of-attachment
scissor, which WebGPU punishes by dropping the WHOLE command buffer (hence no ImGui at
all); (c) those dropped submits are also what ate one-shot bakes (why IBL needed the
brute-force warmup). Fixes: surface texture now released from an OnSubmittedWorkDone
callback (never mid-flight), FrameContext reports the backbuffer size, ImGui scissors
clamped to the target.

- Note: the "UI: default font ... Roboto-Regular.ttf not loaded" warning is unrelated
  (game-UI subsystem font not packed for WebScene; WebScene draws no game-UI text).

## 2026-08-15 - mobile-web smoke (precursor for Specs/asset-variants.md P3)

Mobile-web is now IN SCOPE. Today's web build uses uncompressed RGBA textures,
which every WebGPU device accepts - so this smoke isolates input/surface/memory
from the format work that follows. Serve the existing web export and open it on
a phone (same LAN or tunnel):

- [ ] Chrome on Android: WebScene loads, renders the 3D scene (sky, meshes,
      probe reflection), no console format/feature errors (chrome://inspect
      remote devtools). Note the reported adapter + which texture-compression
      feature it exposes (expect ASTC and/or ETC2, no BC).
- [ ] iOS Safari (WebGPU is on by default in recent iOS): same load + render
      check; note adapter + compression features.
- [ ] Touch: does anything respond? (No touch bindings are expected to be
      wired for WebScene - record what happens, don't fix here.)
- [ ] Memory/perf note: does the tab survive the full scene, and roughly what
      frame rate? (Uncompressed textures are the known memory hog - this
      number is the "before" for the ASTC variant pak.)

## 2026-08-01 - built-in project manager (b5db25ec..97c7b757)

- [ ] Version prompt: hand-edit a scratch project's Project.xml engineVersion to 0.0.1 ->
      Open shows Back Up & Open / Open Without Backup / Cancel; backup creates
      Project.xml.0.0.1.bak and the manifest re-stamps to 0.1.0 after open. Set it to
      99.0.0 -> the hard newer-engine warning.
- Note: a scratch project `.test-scratch/manager-smoke-project` may appear in your recents
  (created by the automated smoke) - Remove From List.

## 2026-08-02 - fonts triad + MSDF UI + polish batch (01ce2c5c..056f90e6)

Fonts triad + MSDF (task #117 - the big one):
- [ ] Editor font override: Preferences > UI font / Mono font paths (restart applies);
      with no override and no dev tree, the EMBEDDED Roboto still renders (rename the
      dev font dir to prove the fallback if you're thorough).
- [ ] FontEditorPage: import a .ttf -> page opens with a live atlas preview (MSDF mode
      shows DECODED white glyphs, not raw rainbow channels); switching Raster/MSDF shows
      only that mode's fields, does NOT hitch the UI (bake runs on a worker), and the
      preview updates BOTH directions; File row Browse... opens the project-constrained
      picker (.ttf/.otf/.ttc only); undo/redo across a mode switch rebuilds the grid.
- [ ] Exported/distributed player: game-UI text renders from the COOKED font (no TTF on
      disk needed) - the dist manifest now carries defaultUiFontId (+ theme/inputmap/
      buslayout ids that were never exported before). -> FIXES #2

Editor fixes from your first pass:

Polish P1+P2 (056f90e6):
- [ ] [+ Add Component] shows category submenus (Rendering / Physics / Audio / Animation /
      Effects / Scripting / UI) with friendly names ("Rigid Body", not
      "RigidBodyComponent"); inspector section headers use the same names; nothing lands
      in "Other" (that submenu appearing = a component missing its category attribute).
      -> #1 SHIPPED 7127fa88: NetworkComponent / NetworkedTransform gained displayName
      ("Network Identity" / "Networked Transform") + category "Networking" - they no longer
      land in "Other". Retest that the new "Networking" submenu appears with both entries.

      -Instanced Skinning in Other category.

Cross-platform (after pulling):
- [ ] Web export still renders game-UI text (cooked font travels in the pak).

## 2026-08-02 (later still) - icon bake + snapped chrome + physics inspector (56d4aeee)

- [ ] High-DPI (if you have a scaled monitor handy): icons stay crisp at 125%/150%/200%,
      and DRAGGING the editor between monitors with different scales re-bakes them
      (momentarily fine either way - the check runs per frame).

## 2026-08-02 (evening) - UI scale + canvas stencil (ecf6829e, 291c81c1)

- [ ] Game UI on a world-panel CANVAS (render-texture UI): complex vector shapes (SVG
      with holes / self-intersection) now fill correctly; screen-overlay HUD unchanged.

## 2026-08-02 - overlay-tier stencil fills (8ebcf4d6)

- [ ] Game HUD (scene overlay tier): draw a self-intersecting star / donut shape in a
      per-scene UI root - core should fill NonZero-correct (no winding artifacts).
- [ ] Screen overlay tier (global overlay layer): same shapes fill correctly.
- [ ] Resize the game window: screen-overlay DS recreates without flicker or
      device-loss (replacement goes through the retire queue).
- [ ] Canvas RTT UI still renders as before (shared stencil-keyed renderers).

## 2026-08-02 - gradient spreads (721951bc)

- [ ] VGSandbox fill-correctness corner, row under the stars: pad square clamps to
      blue after the first third; repeat square shows 3 hard-seamed red->blue bands;
      reflect square ping-pongs red->blue->red; circle shows repeating rings.
- [ ] Windows/Vulkan AND web (recooked pack): same appearance.
- [ ] Existing pad gradients everywhere (editor UI, MSDF text, icons) unchanged; image
      edges (icons/atlas) show no new artifacts (default sampler is now clamp).

## 2026-08-02 - blend modes + SVG gradients (b6994725, e0029d11)

- [ ] VGSandbox fill-correctness corner, second row: additive circle brightens over the
      grey strip, multiply darkens, screen lightens, normal reference unchanged.
- [ ] VGSandbox SVG demo: the VG badge disc now shades light-to-dark blue (radial
      gradient, highlight upper-left); the TINTED badge copy stays FLAT orange.
- [ ] Editor icons (tinted SVGs) unchanged.

## 2026-08-02 - vertex-color sRGB decode (09db16f1) - INTENTIONAL global shift

- [ ] Consistency check: VGSandbox fill-correctness corner, right of the spread row -
      one red block made of a solid half + a same-color gradient half must be SEAMLESS
      (any visible seam = a decode regression). Added in dfce215e.
- [ ] Blend demo re-check: additive still clips toward white over the light strip.
- [ ] Web after recook: same appearance as desktop.

## 2026-08-15 - UAT-session fixes (regression checks)

The raw findings from this session moved to Documentation/Plans/week-2026-08-15.md ("UAT findings").
The items FIXED in-session, as regression checks to re-run:

## 2026-08-16 - Property animation (task #129) — NEW, needs first on-screen run

Animate any reflected property on any component with keyframe curves (foundation.core:curve ->
foundation.propertyanimation -> resource/pipeline -> PropertyAnimatorComponent -> editor page). The
runtime path is unit-tested (222 assertions); these verify the editor page + in-scene playback.

- [ ] Asset Browser: New Asset > Animation > "Property Animation Clip" creates an empty clip that
      cooks; double-click opens the page (toolbar Save/Undo/Redo/Discard on top, a transport row,
      then the track list).
- [ ] Tracks: "+ Track" adds a Transform.position Float3 track; edit the component-type + property
      fields (click-to-edit); the kind button cycles Float/Float3/Color/Quat; "x" removes the track.
- [ ] Curve canvas (scalar Float/Float3/Color tracks): click the canvas adds a key, drag moves it,
      right-click deletes; Cubic shows draggable tangent handles; the per-track interp button cycles
      Step/Linear/Cubic; the transport "Length" field rescales the time axis; each drag = one Ctrl+Z.
      Float3/Color draw N colored channels on one canvas (shared time). Quat tracks instead show a
      numeric xyzw key table (+ Key / x).
- [ ] Transport scrub: typing a Scrub time updates the "path=value" readout per track (P1 preview;
      driving a selected scene entity is a known follow-up - see below).
- [ ] Save cooks the clip; a bound proxy hot-swaps. Reopen shows the same tracks/keys (round-trip).
- [ ] ACCEPTANCE (demo scene): add a PropertyAnimatorComponent (Add Component > Animation > Property
      Animator) to an entity, pick the clip, set Loop; author one track animating the entity's
      position and one animating a Light's color. In play-in-editor the entity moves + the light
      pulses, looping. Once/PingPong loop modes behave. A track to a MISSING component/property is
      disabled with one log warning (no crash); the other tracks keep playing.
- [ ] Export: the clip asset ships; the EXPORTED player plays the same animation.

In-scene authoring mode (Phase H, 2026-08-16) - the same editing view, docked in the scene viewport:
- [ ] Scene page: the viewport toolbar has a "Property Animation" toggle (after the gizmo mode
      buttons). Toggling it ON activates the tool and docks a panel (left rail beside the viewport)
      with clip chrome (New / Pick... / + From Selection / Save) above the SAME track + curve +
      transport view the standalone clip page uses. Toggling it OFF (or another tool) undocks the
      panel and returns to the Select/gizmo tool.
- [ ] New creates + loads a clip; Pick... opens the clip asset picker and loads the chosen clip.
- [ ] Select an entity, "+ From Selection" seeds one track per animatable property of its components
      (Transform.position/rotation, a Light's color, ...); it is ONE Ctrl+Z. Non-animatable
      properties (ints/bools/strings) are skipped.
- [ ] Live preview: with a clip + a selected entity, typing a Scrub time drives the entity's real
      component values (it visibly moves/changes in the viewport); a marker highlights the previewed
      entity. Leaving the tool (or entering Simulate/Play) restores the entity to its pre-preview
      state. Preview NEVER dirties the scene and is not undoable. Save writes + re-cooks the clip.
- [ ] Preview is disabled during Simulate/Play (scrub does nothing until you Stop).

Known follow-ups (NOT expected to work yet - do not file as bugs):
- Live preview from the STANDALONE clip page (page has no scene selection; the scrub there only shows
  the "path=value" readout - use the in-scene tool above for live entity preview).
- Animating a DYNAMIC rigid body's transform: physics wins (documented rule - animate KINEMATIC
  bodies; a dynamic body's non-transform properties animate fine).
