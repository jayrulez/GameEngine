# Import workflow - the review-and-commit import dialog

> 2026-11-01: continued by Archive/import-window-history.md (three panes, previews from a registry, settings
> an importer declares, reimport), agreed with the user over a mockup.

> STATUS 2026-08-31: P1 SHIPPED (37b44451) for the MODEL importer -
> DescribeImport seam + selection on ImportOptions + the dialog's per-kind
> resource list (check-all, per-row enable + rename) + prepare-first flow
> reusing the worker payload at commit. P2 SHIPPED (605ddf0f): ONE batch
> session per drop (BatchImportDialog - file list w/ enable + per-row
> importer dropdown replacing the modal-per-file chooser, per-file plan +
> toggles, shared destination, streaming worker prepares, Import gated on
> described). Also FIXED (48bf978a): LOD folding shifted manifest mesh
> slots - authored-LOD models lost meshes in generated prefabs/scenes.
> P3 SHIPPED (aca3a502): single-asset importers
> (texture/image/audio/heightfield/splatmap/font) describe + honor renames
> via a bare-ImportOptions carrier; a single dropped file always gets its
> one-row review (only option-less+plan-less importers import immediately).
> P4 SHIPPED (b353bef6): re-import memory for models - the manifest stores
> the committed selection (envelope v1), StoredSelection + merge pre-seed
> every describe site.
> P5 SHIPPED (f67efacc): single-asset re-import memory (the typed fileName
> back-reference finds a RENAMED asset again; re-import updates it instead
> of duplicating) + script/UI importers describe. All nine importers are
> now review-capable.
> NOT yet: previews, presets/inference. User visual verify owed on BOTH
> dialogs.
>
> TRAKTOR REVIEW RULINGS (user 2026-09-01, from reading MeshAssetEditor):
> - AGREED: a MANIFEST EDITOR PAGE for ModelManifestAsset (plan list +
>   settings + Reimport button) - Traktor's edit-any-time ergonomics on
>   top of our fan-out model. Future slice.
> - REJECTED: rasterized-image preview - previews stay the LIVE 3D
>   direction when built.
> - REJECTED: filter-expression / wiring-grid organization - keep our
>   list + checkboxes.
> - FIXED (defe87d4): batch file list used Buttons (active file
>   unreadable) -> BatchFileRow, a selectable row with accent highlight.

> DRAFT for discussion (user + Fable, 2026-08-31). Nothing here is scheduled;
> the user wants ONE well-planned improvement pass over import UX rather than
> incremental patches. Absorbs the open import seeds: merge-meshes (user
> 2026-08-31), skip-joint-nodes (week-2026-08-29 seed), and the
> importer-chooser modal queue (shipped 2026-09-01, absorbed here as a
> stopgap).

## The problem

Import today is single-shot and blind: drop a file, maybe pick an importer
(one modal per file), set a handful of pre-toggles you can't see the
consequences of, and the importer fans out everything with source-derived
names (`CUBezierCurve.000`, 19 `CharacterArmature_*` clips) into the group.
No preview, no per-resource choice, no renaming, and a 10-file drop is 10
disconnected sequences.

## The shape: prepare -> review -> commit

The pipeline already separates PREPARE from commit (`IFileImporter` has a
`prepared` object). The plan builds the missing middle: a review dialog fed
by prepare output, committing only what was approved.

1. **Prepare** (per source file, off the UI thread via the job service):
   parse the source, produce a MANIFEST of would-be resources - kind, source
   name, proposed target name, dependencies (material -> its textures) -
   with NO content-DB writes.
2. **Review** (the dialog, one session for the whole drop):
   - RULING (user + Fable 2026-08-31): ONE dialog per drop, ALWAYS - no
     per-type dialogs, no tabs, no no-dialog fast path for single files
     (almost any file can import as several asset types, so the type choice
     is part of every import; a single file is just a one-row session, and
     "import with defaults" keeps it one click).
   - Left: source-file list (the batch), grouped into SECTIONS by resolved
     importer ("Models (2) / Textures (5) / Audio (1)"). A mixed-type drop
     is therefore the same dialog as a homogeneous one. Per-file importer
     dropdown where the extension is ambiguous - this REPLACES the
     modal-per-file chooser; changing it moves the row into that importer's
     section. Selecting a section header (or multi-select) sets the
     importer / applies a preset for all rows at once.
   - The DETAIL panes (center + right) adapt to the selected file's
     importer - model shows the resource tree + 3D preview, texture its
     options + flat preview, audio its options. One dialog, one selection
     model, one commit; per-importer detail panes (the inspector's
     per-component-editor idiom).
   - Center: resource tree for the selected source, grouped by kind
     (Meshes / Materials / Textures / Skeleton / Clips / Generated:
     prefab, scene, collision, LODs). Checkbox per item AND per group
     (today's blind toggles become the group checkboxes). Editable target
     name per item. Inline collision warning when a name exists in the
     target group.
   - Right: preview pane for the selection - 3D orbit for model/meshes
     (PreviewViewport, the bespoke-page machinery), flat view for textures,
     clips playable on the model (per-scene simulation gate already allows
     this).
   - Batch rename rules over a selection: strip prefix, find/replace,
     prefix/suffix. This is the real fix for 19 clips - per-item editing is
     the fallback, not the workflow.
3. **Commit**: import the checked set under the chosen names into the target
   group. Progress per file; failures leave the rest of the batch alone.

## Batch efficiency (many models at once)

- ONE dialog session for an N-file drop; per-file review is optional, not
  forced. "Import all with defaults" stays one click - the dialog must never
  make the simple case slower.
- **Presets are RULE SETS, not flat settings** (ruling, user + Fable
  2026-08-31, from the terrain-set case: a drop of diffuse + displacement +
  heightmap + masks needs DIFFERENT settings per file, so "apply one
  setting to all" is the failure mode). A preset = named rules (name-token
  -> configuration) + group toggles + rename rules ("Character",
  "Static prop", "Terrain set"). Applying one runs the rules per file.
- **Batch application is SELECTION-scoped, never drop-scoped**: select the
  four masks -> set Data Mask once; per-file editing stays the fallback.
- **Name-token inference pre-configures each file at prepare time**:
  pipeline::InferTextureUsage (shipped with the texture-page rework) maps
  _diffuse/_normal/_mask/_height tokens to usage -> color space ->
  compression, so a texture set arrives correctly configured and review is
  a glance. Inference may also suggest the IMPORTER per file (a heightmap
  likely wants the heightfield importer, not texture) - shown in the row's
  dropdown, overridable.
- Previews load LAZILY (selection-driven) - a 100-file drop must open
  instantly.
- Shared settings edit = apply to all selected files; per-file overrides
  win.

## Re-import

The committed choices (checked set, target names, preset, toggles) persist
on the manifest/import options so re-import and prefab regeneration reuse
them without re-asking. Re-import shows the dialog only when the source
grew NEW resources (they arrive checked-or-not per the preset, flagged as
new).

## Model-import options that ride this

- **Merge meshes** (user 2026-08-31): collapse same-skin skinned parts /
  static submeshes into one mesh with submesh material slots. Off by
  default (loses per-part show/hide). Checkbox in the Generated group.
- **Skip joint nodes** (week-2026-08-29 seed): drop pure-joint GLTF nodes
  from generated prefabs/scenes - they are dead weight (the skeleton
  resource carries its own hierarchy). Likely default ON.
- Existing toggles (textures/materials/animations/prefab/scene/collision/
  convex/LODs) migrate into the group checkboxes unchanged.

## Hosting: modal first, utility window later

The dialog content is a HOST-AGNOSTIC view. Phase 1 hosts it in a large
centered modal layer inside the main window (cheap, no new windowing).
A standalone utility window (OS window + own root view, non-dockable) is a
plausible later host - the docking floats prove the shell machinery exists -
but it is a separate windowing deliverable and must not gate the import UI.
(Wayland verification for OS-chromed floats is still owed; don't stack new
platform risk on the import track.)

## Open questions (decide at planning)

- Does prepare load full source content for previews, or a cheap header pass
  first with the preview loaded on selection? (Recommend: cheap manifest at
  open; full load lazily per selection.)
- Where do presets live - per-project settings (per-project store exists) or
  user-global?
- Target-group choice in the dialog, or always the drop-target group?
- Clip preview: play on the imported model in the preview scene - needs the
  prepare-stage model wired to a private preview scene before any assets
  exist. Feasible (runtime model spawn path exists) but the most involved
  preview; ship texture/mesh previews first?
