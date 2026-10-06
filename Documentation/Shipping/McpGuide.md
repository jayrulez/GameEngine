# Working through the MCP server

The engine's MCP server (`engine-mcp`, the `Tools.Mcp` binary) is the headless authoring
surface: newline-delimited JSON-RPC over stdio, the full project/asset/scene/script
workflow with the editor closed. This is the operating manual for any agent connected to
it - it is itself served as `docs://McpGuide.md`, so you can re-read it over the wire.

## Two hosts, one surface

The headless host above is `Tools.Mcp`. The EDITOR serves the same engine tools over HTTP
(`engine-editor-mcp`) for the project it has open - the live one, one content database, one
writer - so an agent can work on what the user is looking at. Tell them apart by
`host_info.serverName` and `host_info.host.kind`. What differs: the stdio host has
`project_create` / `project_open` (the editor's project is the editor's), and the editor's
long tools (a cook) keep the call open until the editor's own background service finishes -
give the client a generous timeout rather than polling. The editor adds the page tools
(`page_list` / `page_open` / `page_reload` / `page_close`), the action bridge (`action_list`
/ `action_state` / `action_execute`: everything a user can do by menu, chord, toolbar or
palette, over the active page, executed unattended - a dialog an action would open is
closed as cancelled and named under `suppressedDialogs`, so the action most likely did
nothing; use a dedicated tool or ask the user) and the scene page's live tools
(`selection_get` / `selection_set` / `simulate_start` / `simulate_stop` / `entity_inspect`,
each addressed by the page's asset guid; `entity_inspect` is the inspector's view of one
entity - hierarchy, transform, every component's reflected properties with asset references
as guids and enums by name - the primary selection by default; `component_set` writes one
of those properties through the page's undo path, one labelled step per call, the page dirty
after and nothing saved, refused while simulating or on a wrong shape (a list takes an array of
its elements, the whole list at once: a mesh's materials, an animator's mesh entities; a list
of structures takes an array of objects, each naming the fields it sets, the rest at their
defaults, foot IK's legs for one; a list or structure inside an element still goes through
`scene_write`); `viewport_camera_get` /
`viewport_camera_set` read and move the viewport's editor camera in degrees, position, yaw,
pitch or a `lookAt` point, editor state only; `viewport_screenshot` writes what the viewport
shows to a PNG and returns its path and size, bringing the page to front first since a hidden
viewport never renders - move the camera, shoot, read the file; `navigation_bake` bakes a
zone, as the inspector's Bake Navigation button does: the static geometry inside the zone's
box (static, non-trigger rigid bodies and terrain; render meshes are not read, so a floor or
an obstacle agents should respect needs a static body, and what moves never bakes) into the
zone's Navigation Zone asset, which must be assigned first, the scene itself unchanged - cook
for the game to see it). A `scene_write` / `prefab_write` over an asset the user has open
reaches its page at once: a clean page reloads in place, a page with unsaved edits keeps
them and warns the user - ask before `page_reload` with `force`, which discards them.

To build ON AN OPEN PAGE instead, live and undoable (the editor host): `entity_create`,
`entity_update` (name, parent, active, transform), `entity_delete`, `component_add`,
`component_remove`, `prefab_spawn`, `behavior_add` and `behavior_set` (a script behaviour's
properties by name, typed by its cooked class), with `component_set` for a component's fields.
Each call is one undo step, the page dirty after and nothing saved; an entity is named by guid,
name or slash path; each answers the entity as `entity_inspect` shows it, a Script component's
`behaviors` included. The user watches the level take shape and can undo any step; `file.save`
(action_execute) keeps it. XML suits a whole level written at once, these tools an edit.

Play in editor (PIE) is the editor's too, and it is how gameplay is tested: `simulate_start`
previews ONE scene in its page, while PIE runs the project as the player does (the default
scene, the startup script, the input map). PIE is not one game: every Game tab runs its own
instance, addressed by its `pie` id (`game-page`, the primary; `game-page-1`, ... for each new
instance), default the primary. `pie_start` cooks, opens and runs the primary (or with
`newInstance` another tab, a host and a client say) and answers once the first frame has
rendered; `pie_state` reports whether it runs, its scene, `runTime` (seconds of frames since
the start, unscaled, so it keeps going while the game pauses its scene at time scale 0, as
behind a menu), the frames rendered and the startup script (`running`, or `faulted` with the
reason); `pie_list` shows every instance, the ones the user started too; `pie_stop` stops one (or
`all`), its tab staying open and the others running. `pie_screenshot` writes what an
instance's tab renders, the game through its own camera with its UI, as `viewport_screenshot`
does for a scene page: the pixels the tab drew, without the bars. A tab smaller than the game's
render resolution draws the game scaled down; the answer then adds `renderWidth`, `renderHeight`
and `scale`, the PNG is that scaled image (soft text in it is the scale, not the font), and
`pie_run`'s mouse positions, which are in render-resolution pixels, are its pixels divided by
`scale`.

`pie_run` is the playtest: it plays a device-level input timeline into one running instance
for `duration` seconds of run time and answers what happened. The timeline takes keys by their
KeyCode case names (`{"at": 0, "key": "D"}`, `{"at": 0.4, "key": "Space", "down": false}`),
mouse moves, buttons and the wheel (`{"at": 0, "mouseMove": [650, 253]}`, `{"at": 0.2,
"mouseButton": "Left"}`, in the game's render-resolution pixels: a `pie_screenshot`'s, divided
by its `scale` when it reports one) and gamepads (`{"at": 0,
"gamepad": 0, "axis": "LeftX", "value": 1}`); `type_info` lists every enum's names. It goes
through the project's input map as a player's would. While it runs, that tab ignores the real
mouse and keys, the other instances keep theirs, and whatever it holds is let go at the end.
`probes` read entities (by guid, name or slash path) at field paths (`worldPosition`,
`position`, `rotation`, `scale`, `active`, `<component>.<property>` as `entity_inspect` names
them, or `<BehaviorClass>.<field>` for a behaviour running on the entity, private fields too,
then `.x`/`.y`/`.z` or a key or an index) and the game script's fields (`{"script":
"score"}`), sampled `every` N seconds (0.5 by default) or at `sampleAt` times; each row is `{t,
frame, values}`, the last one at the end, and an entity gone by then reads null. `screenshots`
writes the tab at those run times. `until` ends the run early, on a death or a win:
`{"entity": "Player", "field": "worldPosition.y", "op": "<", "value": -10}`. `endedBy` says why
it stopped: `duration`, `until`, `stopped` or `timeout`. A menu is played the same way:
`pie_screenshot`, find the button, click it with a timeline. Probes and names are checked
against the running game before the run starts, so a typo fails at once. Runs are real frames
at real frame rates: a time lands within a frame of where it was asked, so compare with
tolerances, and start from `pie_start` for a run you mean to repeat. One `pie_run` per instance
at a time (a second is refused while the first plays); instances run side by side. Over HTTP
each call is its own, even two identical ones from two agents: two `newInstance` starts open two
tabs (they come to front in turn, since a hidden tab renders no first frame), two screenshots of
one tab each get their file, two `asset_import`s or `project_export`s run side by side, and a
call whose connection closes ends there (an unfinished `pie_run` lets go of the tab's input).
`entity_inspect` with `pie` (and an `entity` by guid, name or path) reads a running game's
entity outside a run, each running behaviour's properties under `live` beside what is authored.

## First moves in a session

1. `tools/list` - read the real surface before guessing; descriptions carry the contract, and
   each tool's `annotations` say what it does to the project: `readOnlyHint` (changes nothing),
   `destructiveHint` (overwrites what exists - the scene and prefab writes), `idempotentHint`.
2. `host_info` - pid (kill a hung host by it) and `buildStamp`. After rebuilding the
   engine, compare stamps: a stale host serves yesterday's engine.
3. `resources/list` - the curated `docs://` shipping docs (Scripting/Assets/Scenes/
   KnownIssues), the scene format reference this host generated from its own build
   (`docs://generated/SceneSchema.json` + `docs://generated/SceneExample.scene.xml`) and,
   once a project is open, every scene/prefab as `project://scene|prefab/<guid>`.

## The ground rules

- **Files are truth.** The tools read and write the project's files directly; no editor is
  involved. Do NOT run against a project that an open editor is actively editing - two
  writers, one set of files.
- **Validate-first writes.** `scene_write`/`prefab_write` refuse invalid content with the
  full report; a refusal is the tool working, never something to bypass.
- **Arguments are checked against the tool's schema.** One the schema does not declare is
  refused (`pie_run: no argument 'timeline' (it takes: pie, duration, input, ...)`), as are a
  missing required one and one of the wrong type, before the tool runs: a misspelt name never
  quietly drops what it carried. The same holds at every depth, the field named by its path
  (`pie_run: no field 'probes[0].field' (it takes: entity, fields, script)`); only a map of
  names the tool resolves itself (asset_import's `options`, a behaviour's `properties`) takes
  any key, and the tool refuses one it does not know.
- **Read before destructive changes.** `asset_uses` before deleting anything;
  `project_health` after - dangling references surface later, not at delete time.
- **Check `known_issues` before re-diagnosing** an odd symptom; if it matches a recorded
  issue, report the match and use its workaround.

## Workflows

**Project**: `project_create` makes the project with the editor's starter content, as New
Project does: Roboto as a distance-field font set as the default UI font (what an exported game's
text draws with: the engine's built-in font is not in a dist), the default sky, and a cube, sphere
and plane. Then `project_open` -> `project_info`, which also reports the
settings play reads (default scene, startup script, default input map, bus layout, UI theme,
loading screen, UI font, MSAA, and `uiFontIds`, the other fonts the game UI loads beside the
default, each a family a label picks with `font-family="<family>"`, a title face say, and the
display: `renderWidth`/`renderHeight`, the resolution the game draws at, 0 for its output's size,
`renderFit`, and the player's `windowWidth`, `windowHeight`, `windowMode` and `windowResizable`).
`project_settings_set` changes them, by the names `project_info` reports: each asset setting must
name an asset of its type, `""` clears it, `uiFontIds` takes the whole list, a choice takes one of its values by name, and nothing changes
when any of it is refused. `project_health` is the
one-call soundness sweep (dangling refs, broken sources, cook state); a dirty count alone
is normal - clear it with `asset_cook`.

**Assets**: `asset_import` (OS file -> Sources/ + typed asset) -> `asset_cook`
(incremental; `force` for full). `options` sets the importer's toggles, the import dialog's
checkboxes by label (`{"Generate collision": true}` gives an imported model's prefab colliders);
an unknown one is refused with the importer's list, and the result lists every toggle's value.
An imported model gets its prefab, the `Prefab` asset beside its manifest, in both hosts (and
a scene with `Generate scene`).
`asset_list`/`asset_info` to inspect either database. `asset_delete` removes a source asset as the
Assets browser does (the next cook sweeps its product; the original file under Sources/ stays); it
is refused while anything uses it - the refusal names the users, as `asset_uses` would - unless
`force`. What File > New makes comes from
`asset_create` (`asset_creators` lists them). A data asset's content (an input map's actions
and bindings, a material, a physics material, a sound cue) is edited through its envelope:
`asset_data_read` gives the XML, and `asset_data_write` takes the edited whole back, loading it
exactly as the engine would and refusing it, unchanged, when it does not load. Enum fields are
numbers there; `type_info` on the field's type names the cases.

**Scenes**: read `docs://generated/SceneSchema.json` once (or `component_schema` for one
component), copy from `docs://generated/SceneExample.scene.xml`, read the target with
`scene_read` (or the `project://` resource), author XML, loop on `scene_validate` (xml or
guid; `valid` + empty `warnings` = the engine will load it), then `scene_write`. Prefabs
mirror it with a single-root rule. To PLACE a prefab (an imported model's is the `Prefab` asset
beside its manifest), add an element to the scene's `prefabInstances`, in the format's
`prefabInstanceRecord` (the schema's `format` section): the prefab's guid, the root's parent and
transform, a fresh `rootLive` guid, and empty `members`, `destroyed`, `transformOverrides` and
`componentOps`; the load spawns it and a save fills those in. `scene_validate` counts them as
`prefabInstances`, apart from `entityCount`, and warns about one whose prefab is not in the
project.

**A game's look (render profiles)**: a scene's `environment` and `postprocess` settings take
their values from the scene itself or from a shared profile, by their `source`: 0 (Scene) uses
the values stored in the scene, 1 (Profile) uses the `Environment Profile` or `Post Process
Profile` asset its `profile` names, and the scene's own values then do nothing. To give several
scenes one look, make the profile with `asset_create` (the creators of those names), set its
values with `asset_data_read` / `asset_data_write` (its fields are the block's, without
`source` and `profile`), `asset_cook` it, and set each scene's block to `source` 1 and `profile`
the profile's guid. A change to the look is then one profile edit and a cook. To change one
scene's look, edit the profile when every scene should follow, or set that scene back to
`source` 0 with its own values.

**Colours**: every colour is written as it is meant to look, in sRGB, the value a colour picker
and a hex code show (0.5 is `#808080`): component colours, the environment's ambient and sky, a
material's `Color` properties, particle colours, debug draw. The renderer decodes them; never
pre-convert. Brightness above white is an intensity, not a component above 1: a light's
`intensity`, or the w of a material's `ColorHdr` (its emissive). In a material's
`uniformDefaults` a colour is four floats (rgba, or rgb and the intensity).

**Scripts**: `script_api` first - the LIVE bound API per backend (angelscript | luau);
never trust memorized signatures. A member with `readOnly: true` (a network identity's
`authority`, for one) reads and refuses assignment in every backend. `script_create` seeds a starter asset
(behavior | level | game tier), then edit the returned source FILE, loop on
`script_validate`, and `asset_cook` to make the class attachable.

**Diagnostics**: `log_write` a marker -> do the risky thing -> `log_read` with
`sinceSequence` = the marker's sequence to see exactly what the engine said after it.

**Export**: `project_health` first (catch breakage before a long cook), then
`project_export` (preset optional; default = first/host preset). The dist lands under
`<project>/Dist` unless `out` says otherwise. `export_presets` lists the project's presets and the
export templates this machine has (each preset's `template` is the one it resolves to here, null
when none); `export_preset_set` creates, changes or removes one by `name` - another device's
target, say, with its own render size or window. Its platform and config must be ones a template
here has; its templateId need not, since presets travel with the project and templates do not.

## Per-tool gotchas

- `script_validate` is a COMPILE check (`checkLevel: "compile"`): a call the language cannot
  see through, a misspelled member on a handle say, compiles and fails at runtime. Cross
  check against `script_api`.
- `scene_validate` warnings mean component records of a type the engine does not know, or
  prefab instances whose prefab is not in the project - they would be SKIPPED on load. Treat
  warnings as breakage to fix, not noise.
- `asset_uses` reports DIRECT users only; re-run on a user to walk the chain. An empty
  result plus empty `projectSettingsUses` is the "safe to touch" signal.
- `log_read` is incremental - always pass the previous `lastSequence`; a non-zero
  `dropped` means the ring overflowed and old lines are gone.
- `asset_cook` and `project_export` are long-running calls (a full cook may run inside
  them); do not assume a hang before minutes have passed.
- `project_export` failures say little in the response by design - the detail is in
  `log_read` (Cook/Export categories).

## When something looks wrong

1. `host_info` - is the host the binary you think it is (buildStamp)?
2. `log_read` - what did the engine actually say?
3. `known_issues` - is it already recorded?
4. `project_health` - is the project itself broken?
Only then diagnose fresh - and write a `log_write` marker before retrying so the next
read starts at your action.
