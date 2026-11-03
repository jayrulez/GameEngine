# Render profiles - a game's look set once, shared by its scenes

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

## The problem

Each scene carries its own `environment` and `postprocess` settings blocks (`EnvironmentSettings`,
`PostProcessSettings`, `Engine.Render/RenderComponents.cppm`). A game with several levels sets its
look in every scene by hand and re-tunes them one by one: PaperKid's six scenes are written by
`block.py` so they stay alike; Sky Hopper's three drifted apart (its levels 2 and 3 were dimmer
than its first until 2026-10-03).

## Proposal

### 1. Two profile assets

An **Environment Profile** and a **Post Process Profile**: data assets carrying the same fields as
the scene blocks (ambient, sky, IBL dimmers, shadow reach; exposure, tonemap, bloom, AO, AA, ...),
including their texture references (the sky texture, the grading LUT). File > New and
`asset_create` make them; they cook and load like any data asset, and an edit reaches every user on
the next cook (the resource reload).

### 2. A settings block's source: the scene, or a profile

Each block gains a **`source`**:

- **Scene** (the default, what every scene has today): the values stored in this scene.
- **Profile**: a shared profile holds the values; the block holds only the reference
  (`profile`). The block's own stored values are not used while it is.

Values live in one place, chosen by `source`, so nothing copies behind the scenes and nothing is
stomped (the Godot shape: a WorldEnvironment's Environment is built-in or an external resource;
Unity's Volume edits its referenced profile, Clone makes a scene copy).

- **Reading**: the renderer, the scene's script handle (`EnvironmentSettings.of(scene)`,
  `PostProcessSettings.of(scene)`) and the inspector all read the block's *effective* values: the
  profile's when the source is Profile, the scene's otherwise.
- **Runtime changes** from script apply to what is effective: the scene's values, or the loaded
  profile (in memory, never saved; every scene of the run sharing it sees the change, as a shared
  resource does in Godot). A game wanting a scene-only change copies into the scene first.

### 3. Editing

- **Scene source**: as today.
- **Profile source**: the scene inspector's section names the profile its values come from (a
  "Values: profile 'Dusk'" row whose button opens it) and shows the profile's values; an edit there
  edits the loaded profile (undoable; every open scene using it sees it at once) and the scene's
  save writes it to the profile asset and cooks it, as a terrain sculpt's live edit is written. The
  block's own fields (`source`, `profile`, marked `sceneOnly`) stay the scene's.
- **"Make profile"**: saves the block's current values as a new profile asset and switches the
  source to it. **"Copy into scene"**: copies the profile's values into the block and switches the
  source to Scene (to branch off).

### 4. The profile page

A dedicated page per profile asset: the same rows as the scene inspector's section (enum dropdowns,
colour pickers, sliders from the reflected ranges), from one reusable settings grid, and a **preview
viewport**: a small lit scene (ground, spheres of a few materials, a cube, a sun) rendered with the
profile applied, updating as the fields change. An Environment Profile previews with the default
post; a Post Process Profile with the default environment.

### 5. Agents (MCP)

`asset_creators` lists both profiles; `asset_data_read` / `asset_data_write` edit their fields; a
scene's blocks take `source` and `profile` like any field (`scene_write`, `component_set` on the
settings). McpGuide says which values are effective.

## Data

- The blocks' data versions bump (environment 6, post 4) with a reader for the previous version
  (the source defaults to Scene).
- The profile records carry the value fields only (not `source` / `profile`); they are written by
  the same field serializer the blocks use, so a field added to a block reaches its profile.

## Tests

- Cook and load: a profile asset cooks to its record and loads to a product with its values and
  its texture references bound.
- Source: a block with source Profile reports the profile's values as effective (the renderer's
  extraction and post resolution use them), source Scene its own; a missing profile falls back to
  the scene's values.
- Scripts: the scene handle edits the effective values (the profile's in Profile).
- Serialization: the new fields round-trip; a previous-version block reads with source Scene.
- Editor: the reusable grid builds the same rows as before for a scene; a profile-mode edit writes
  the asset and undoes; Make profile / Copy into scene move values without loss; the profile page's
  preview scene takes the profile's values.

## Not in scope

Per-field overrides of a profile within a scene (a scene is either Scene or Profile); blending
between profiles by volume (Unity's local Volumes).
