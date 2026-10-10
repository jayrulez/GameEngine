# Export

> Status: CURRENT
> Verified: 2026-08-12 @ b5d0418b
> Track: [[settings-and-export-track]]

Turning an authored project into a shippable per-platform dist, driven uniformly from the CLI and the
editor (the same way cook is uniform). Shipped end to end: the template/preset architecture, template
creation + the config axis, reachability pruning, the uniform driver, and both surfaces' UIs. Lives in
`editor.project` (`:export_pipeline` + `:export_preset` + `:export_template` + `:export_roots` partitions);
the CLI is `Tools.Export`.

## Templates vs presets (the core split)

Separate the portable prebuilt bundle from the project-local config:

- **`ExportTemplate`** (`ExportPreset.cppm` / `ExportTemplate.cppm`) - a per-platform, per-CONFIG
  prebuilt player + its runtime sidecars + a `template.xml` manifest, living in a templates ROOT outside
  any project (shared across projects, referenced by id, never by absolute path). Fields: `{ id, name,
  platform, config, compiler, engineVersion, playerBinary, sidecars[], notes }`. A `TemplateRegistry`
  (Refresh / FindById / `FindBy(platform, config)` / Resolve) indexes them, and a HOST implicit template
  is synthesized from the running tool's own `Bin/<Config>/<Platform>-<Compiler>/` dir (id
  `host-<platform>-<config>`) so a dev export works with zero setup.
- **`ExportPreset`** (project-local, committed to `<project>/export_presets.xml`): references a template
  by id + game choices `{ name, platform, templateId, playerName, outputSubdir, additionalFiles[],
  pruneToReachable }`. No machine paths, so it is safe to commit.

## Template creation + the config axis

A template is `platform x config`: Debug / Release / RelWithDebInfo are SEPARATE templates (the host
template carries the config/compiler that built the running tool). `CreateTemplate(configDir, destRoot,
mode)` synthesizes + materializes a template bundle from a `Bin/<Config>/<Platform>-<Compiler>` build
dir (reusing `SynthesizeHostTemplate` to read the platform + sidecars), either installing it into the
templates root or writing it to a chosen folder. CLI:
`Tools.Export --template create <configDir> [--install | --out <folder>]`, plus `--template list|import`.
Config-driven sidecars: `draconic_copy_runtime_deps` emits `<player>.runtime-libs` at build time; the
host template reads that manifest (authoritative, replacing a hardcoded known-sidecars list).

## Reachability pruning

Opt-in per preset (`ExportPreset::pruneToReachable`, wire v3; default off = pack everything). When on, a
dist ships only the CLOSURE of its entry points instead of the whole cooked dir. Detail:
`Documentation/Systems/export-reachability.md`.

## Uniform driver + surfaces

`ExportContent` (cook-less pack/stage) is factored out of `ExportProject` (cook + content); `ExportOne`
/ `ExportAll` take a `cook` flag, an `ExportProgress` sink, and return `ExportResult { content,
filesStaged, outputDir, engineVersionWarning }`. A resolved template whose `engineVersion` differs
warns and still exports (soft-match). Scene/prefab source streams pre-transcode to binary before packing
(`CollectSceneStreams` / `TranscodeSceneStreamToBinary`); file ops use native
`core::FileCopyPreserving` / `CreateDirectories`. Every export also cooks the engine shaders for the
target platform from the caller's data root (`ExportOne` / `ExportAll` take `dataRoot`; the editor,
`Tools.Export` and the MCP tool resolve it the one way - `Documentation/Systems/data-root.md`) and
stages `Data/.dataroot` + `Data/Shaders/shaders.dpak` beside the player (`StageShaderPack` -
Vulkan/DXIL for desktop, WGSL for Web): the data-root layout the player's discovery walk finds, so a
dist renders with no external dependency (engine shaders are cooked `.hlsl`, not baked into the binary).

- **CLI** (`Tools.Export`): `<project> [--out --preset | --all --rebuild]`, `--template list|import|
  create`; host-preset fallback when no `export_presets.xml`.
- **Editor**: `Project > Export...` (`ExportDialog`) lists the project's presets beside the selected
  one's settings in tabs (General, Content, Display) under a card naming the template it resolves to
  here; edits save as they go. An export runs NON-BLOCKING (cook via `EditorCookService`, then
  pack/stage on `EditorJobService`), with status-bar progress and a sticky success toast + Open Folder
  (native reveal). `Edit > Export Templates...` (`TemplatesDialog`; the templates are the editor's,
  shared by every project) lists the registry's templates by the icon each carries, with their details,
  Install from Folder, Create from Build, Reveal and Remove. The editor mutation lock covers background
  jobs (closes the export-vs-UI DB race).

Templates root resolution: `ResolveTemplatesRoot(override -> $ENV_TEMPLATES_DIR ->
<user-data>/templates)`, the override being `EditorExportSettings.templatesRoot` (edited in
`File > Preferences...`).

## Companion docs

- Template config axis + creation detail: `Documentation/Systems/export-templates.md`.
- Reachability closure + Always-Export roots: `Documentation/Systems/export-reachability.md`.

Non-goal: cross-compiling players at export time - we stage PREBUILT player binaries.

---

The original problem framing + the historical design sections are in
`Documentation/Archive/export-design-history.md`.
