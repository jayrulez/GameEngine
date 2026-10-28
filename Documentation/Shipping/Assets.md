# Assets

How content flows through an engine project: **source assets** are authored/imported into
the project's source database, the **cook** compiles them into runtime products in the
cooked database, and the running game loads products by guid.

## Identity

Every asset has a **guid** - its stable identity. Names and groups are organizational and
safe to change; references (from other assets, scenes, and project settings) always bind by
guid, so renames and moves never break them. Deleting is the dangerous operation - query
first (see below).

## The workflow

1. **Import** - a source file (texture, model, audio, script, ...) is copied under the
   project's `Sources/` directory and a typed Asset envelope is created in the source
   database. MCP: `asset_import` (routed by file extension).
2. **Cook** - the incremental cook compares each buildable asset's recipe (source bytes,
   settings, dependency recipes, cook-logic version) against its last build and rebuilds
   only what changed, in dependency order. MCP: `asset_cook`; `project_health` reports the
   dirty count without building.
3. **Reference** - components and other assets hold guid references; the runtime resolves
   them against cooked products.

Scenes and prefabs are NOT cooked - they are XML text sources staged directly (see
Scenes.md).

Game GUI sources link like scripts: `.xml` documents, `.css` sheets and `.svg` vector images
import as assets whose text stays in `Sources/`. A document or sheet names images, vector
images, fonts, sheets and documents by asset guid (`{guid}`), each a dependency of its cook.

## Inspecting

- `asset_list` / `asset_info` - what exists (guid, name, type, group), in either database.
- `asset_uses` - REVERSE dependencies: every direct user of an asset, with the edge kind.
  Call it before deleting anything (renames and moves are guid-safe, but knowing the users
  still tells you the blast radius).
- `project_health` - one call: dangling references, sources that no longer load, dirty
  count, orphaned products, last-cook failures.

## Gotchas

- An asset that imports fine can still fail to cook (e.g. a missing companion file); cook
  failures are per-asset and reported in the cook stats and `project_health`.
- References to a deleted asset do not fail at delete time - they surface later as dangling
  refs. Make `asset_uses` (before) and `project_health` (after) the habit.
