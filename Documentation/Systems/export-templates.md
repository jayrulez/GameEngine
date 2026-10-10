# Export Templates

> Status: CURRENT
> Verified: 2026-08-12 @ b5d0418b
> Track: [[settings-and-export-track]]

An export template is a portable, prebuilt RUNTIME bundle for one `(platform, config)`: a player
executable, that build's runtime sidecars, optional symbols, and a `template.xml` manifest. A project's
preset references a template and adds the game half (cooked content + extras + naming). Template
(runtime) + preset (game) -> dist. Shipped. Overview: `Documentation/Systems/export.md`.

The line held: a template is the RUNTIME you ship against - never game content, never editor code (the
player links zero editor), never a toolchain (the bundle is prebuilt, so using it needs no compiler -
the whole point vs an SDK-toolset model).

## The config axis

A template is `(platform, config)`: Debug / Release / RelWithDebInfo are SEPARATE templates, not
profiles inside one. Rationale: it is what the build already produces (each `Bin/<Config>/<Platform>-
<Compiler>` dir is a self-contained runtime with its own `<player>.runtime-libs`); sidecars are
config-specific (debug vs release link different native runtimes); and size (tripling a player+deps for
three configs is wasted disk when most projects ship Release only). Product stance: config is
first-class so "release template" and "debug template" are both expressible, but the product defaults to
and optimizes for RELEASE - Debug/RelWithDebInfo are opt-in dev/power-user variants.

The PUBLISHED desktop templates (the release downloads for Linux and Windows) are the exception since
2026-10-10: they are RelWithDebInfo, built from the same tree as the editor download, so a release
builds the engine once and a crash in a game shipped on them can be resolved. The player is optimised
all the same; its debug info is split off before packaging (the Linux template's player is stripped,
the Windows PDB never enters the template) into a `-symbols` archive published beside the template.
Their ids say so (`gameengine-linux64-relwithdebinfo-<version>`), and a preset asking for Release
finds them by the platform fallback below.

The host implicit template inherits the running tool's config (develop the editor in Debug -> the
zero-setup host template is a Debug player), so shipping requires an explicit Release template; the
config axis makes this a non-special-case (the host template carries whatever config built it).

## Identity + resolution

Identity = `platform x config x engineVersion` (e.g. `draconic-win64-release-0.1.0`); `compiler` is
METADATA for traceability, not a selector; `arch` folds into the platform tag. A preset selects
`(platform, config)` (config defaulting to Release), resolved by `TemplateRegistry::FindBy(platform,
config)` with an explicit `templateId` winning and a platform-only fallback (nearest config, preferring
Release) so old presets resolve. `EffectiveConfig()` treats an unstamped (v1) template as Release.

## Payload + manifest

`template.xml` (shipped): `id`, `name`, `platform`, `config`, `compiler`, `engineVersion`,
`playerBinary`, categorized runtime files - `sidecars` (REQUIRED, always staged, read from the build's
`runtime-libs` manifest) + `symbols` (optional PDB/DWARF, staged only when the preset opts in) - and
`notes`, and `icon`, the SVG in the bundle the editor shows the template by (`icon.svg`; a manifest
from before it shows its platform's built-in icon), plus runtime-resolved, non-serialized `directory` /
`isHost`. Symbols do not justify a
separate template; they are an optional group on Debug/RelWithDebInfo templates, and export defaults to
NOT staging them into the shipped dist (ship stripped, retain symbols beside the template for
symbolication). If the player runtime-compiles shaders, its compiler (e.g. `dxcompiler`) is just another
required runtime-lib.

## Create / Import / Resolve

- **Create** - `CreateTemplate(configDir, destRoot, mode)` synthesizes the descriptor from a build dir
  (`SynthesizeHostTemplate` reads platform + `runtime-libs`), stamps config/compiler/engineVersion,
  copies the player + sidecars (`FileCopyPreserving`, keeps +x), writes the icon (`icon.svg`: a
  built-in one by name, `--icon handheld` for the Steam Deck build, or a given `.svg`; else the
  platform's built-in desktop, windows, linux or web icon) and `template.xml`. Two modes:
  install into the templates root (usable immediately) or export to a folder (for zip + distribution).
  A Web build synthesizes a "Web" template (the player is the `.html`, sidecars from the manifest, and
  export stages page + sidecars + `Content.pak` + `player.xml` + the WGSL `Data/Shaders/shaders.dpak`
  with its `Data/.dataroot` marker).
- **Import** - `ImportTemplate` (validate `template.xml` -> recursive copy under `<id>`).
- **Resolve** - as above.

## Baseline engine content (shipped)

The shaders-out-of-C++ track shipped, so the player boots against a COOKED shader pack, not baked-in
HLSL: engine shaders are `.hlsl` source cooked by `foundation.shaders`' `ShaderPackCooker`, and export
stages `Data/Shaders/shaders.dpak` (+ `Data/.dataroot`) beside the player for the preset's platform
(`StageShaderPack`, cooked from the resolved data root's `Shaders/`) so the dist renders with no external
dependency (Web stages the WGSL variant) - the same `Data/` layout every executable discovers
(`Documentation/Systems/data-root.md`). This baseline runtime content is
produced by EXPORT per platform rather than carried inside the template bundle - so the template stays
just the player + sidecars, and the render-pass shaders are dist content. Fonts/fallback textures the
player needs to boot are similarly minimal today.

## Deferred

- **`capabilities`** manifest field (compiled-in RHI backends so a preset can require one) - forward-
  looking, not built.
- **Downloadable templates** (fetch a versioned archive; same install path as import) - future.
- **Baseline content INSIDE the template** vs produced by export. Today the shader pack is export-
  produced per platform; folding a prebuilt baseline pak into the template bundle is an option only if
  a real need appears (open, low priority).
