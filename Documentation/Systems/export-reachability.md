# Export Reachability

> Status: CURRENT
> Verified: 2026-08-12 @ b5d0418b
> Track: [[settings-and-export-track]]

Pruning a dist to what the game actually needs: with `ExportPreset::pruneToReachable` on (opt-in, wire
v3, default off = pack everything), a dist ships only the CLOSURE of its entry points instead of the
whole cooked dir. Phases 1 + 2 shipped. Overview: `Documentation/Systems/export.md`.

## Phase 1 - mechanics (shipped)

`pruneToReachable` seeds roots from `defaultSceneId` + the startup script's own asset
(`CollectExportRoots`, the Phase-2 seam); a `SceneReferenceScanner` bridges the scene->asset edges the
`CookDriver`'s plan cannot see; the closure drives BOTH cook and pack; an `export-report.txt` (kept
roots + reasons, dropped list) is written beside the dist. Proven on a real project: 139 MB vs 719 MB,
179 unreferenced instances dropped.

The CLI (`Tools.Export`) supplies the scanner (`MakeSceneScanner`: loads a scene over the manager set,
`CollectUnresolved`, reads parked prefab instances). The EDITOR prunes too, via a main-thread pre-scan:
the scanner must LOAD scenes to enumerate refs, but the editor runs export on the editor task pool's build lane
background thread where scene loading is not safe; so a `SceneRefScanner` hook on `EditorContext` (set by
the scene editor plugin, loading through the SceneSubsystem's full manager set) is driven on the MAIN
thread up front, and the precomputed reachable-root guid set is passed into the background job.

## Phase 2 - Always Export (shipped)

A project-level `ExportRootsSet` (`:export_roots` partition): flagged instance GUIDs (rename/move-proof)
+ flagged group PATHS (subtree-as-root, dynamic membership), persisted as a committable
`export_roots.xml` beside the project (mirroring `export_presets.xml`), loaded by `EditorProject::Open`,
CLI-loadable. `CollectExportRoots` appends `Flag` (instances) + `Group` (every instance under a flagged
subtree via `CollectGroupInstances`) roots, deduped by guid (highest-priority reason wins). Editor UX:
Asset Browser right-click "Always Export" (asset) / "Always export contents" (group) in the row + group-
tree menus, saved immediately with a notice + refresh; a Sedulous-style corner-dot badge (`AssetCell`
green dot) marks flagged items.

## Deferred

- **Phase 3 - typed `AssetRef<T>`** (literal reference edges the scanner reads directly instead of a
  bridging scan). Not built (no `AssetRef` type exists).
- **Phase 4 - the script-load lint** (a STRETCH goal, user-prioritized 2026-07-20): a LANGUAGE-NEUTRAL
  safety lint over genuinely-dynamic `Load` calls that survive - warn that a dynamically-loaded asset is
  not in the closure, escalatable warn->error in shipping mode. Must be backend-agnostic (Wren +
  AngelScript + Luau), behind a per-language cook hook. Not built. Explicit "Always Export" is the real
  answer; the lint is a convenience that catches misses.
- **Phase 5 - runtime capture** ("loaded but not in closure" diagnostics from a running build). Not
  built.
- **Minor UI follow-ups**: an undoable Always-Export toggle command (immediate-save mirrors favorites
  today), an Export-Roots audit panel (the pruning report + badges already give auditability), a left
  content-tree group badge, and a future "Never Export" flag.
