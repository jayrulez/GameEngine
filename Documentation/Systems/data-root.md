# Data root - the one way an executable finds engine data

> Status: SHIPPED 2026-09-15. Replaces every compile-time path (`BUILTIN_*`, `SAMPLE_*_DIR`,
> `TEST_*` defines), every `"Shaders"`-beside-the-exe fallback, and the per-app font path
> presets. There is exactly one mechanism; the Beef port (Sedulous) ports this one.

## The layout

A **data root** is a directory named `Data` that holds a `.dataroot` marker file. Everything an
executable needs at runtime that is not a project asset lives under it, at fixed relative paths:

    Data/.dataroot                         the marker (foundation.vfs kDataRootMarker)
    Data/Engine/Shaders/*.hlsl, *.hlsli    the engine shader corpus (dev: compiled on demand)
    Data/Engine/Shaders/shaders.dpak       the cooked pack (dist/web: no compiler)
    Data/Engine/Fonts/roboto               the GUI's built-in default font
    Data/Editor/...                        the editor's pages, sheets, icons, fonts (Fonts) and code
                                           languages (Code)
    Data/Branding                          the engine's logo, splash and window icons
    Data/Templates                         what a new project starts with (its sky)
    Data/Samples/...                       the code samples' content, by kind (Models, Environment,
                                           Audio, Images, Shaders, UI, Fonts)
    Data/Tests/...                         test fixtures and the files only tests read
    Data/SampleProjects/<Project>          the sample games (projects, opened like any other)
    Data/Output/<Sample>                   per-sample cooked-output databases (written, not shipped)

The folders say who reads them (Specs/data-layout.md): an editor distribution copies Engine,
Editor (less its design mock), Branding and Templates whole; an exported game stages only its
shader pack; Samples, Tests, SampleProjects and Output ship nowhere.

The source tree's `Data/` IS a data root. A shipped dist stages one beside the player (see below).
The paths under it are spelled in exactly one place each: `foundation.shaders.system` exports
`kShaderFolder` / `kShaderPackFile` / `kShaderPackPath` ("Engine/Shaders/shaders.dpak"); the GUI's
default font is the one constant in `GuiSubsystem::OnInit` (`kDefaultFont`).

## Discovery (`foundation.vfs`, `DataRoot.cppm`)

- `FindDataRoot()`: `<exeDir>/Data` then walk up from the executable's directory, then
  `<cwd>/Data` and walk up from the working directory; the first `Data/.dataroot` wins. Anchored
  at the executable because the working directory is unreliable (menus, `.desktop` files); the
  walk covers a dev checkout (`Bin/Debug/<lane>/app` -> `<repo>/Data`) and a relocated dist
  (`Data/` beside the exe) with the same code.
- `ResolveDataRoot(overrideDir)`: an explicit `--data-root <dir>` (or `--data-root=<dir>`,
  parsed by `DataRootFromArguments(argc, argv)`) wins, and is VALIDATED - a directory without
  the marker is refused with an error, not used. Empty override = `FindDataRoot()`.
- No root = a loud error naming what was searched and the fix (`Data/` beside the exe or
  `--data-root`). There is deliberately NO compile-time fallback: a build that cannot find its
  data must fail where the user can see it, not read the build machine's tree.

## Who resolves it, and what consumers get

The APPLICATION layer resolves the root once and mounts a `NativeFileSystem` over it. Nothing
below it knows where the root is - only what it reads relative to it:

- `DefaultApplication::Configure`: `ResolveDataRoot(m_dataRootOverride)` (preset via
  `SetDataRoot` - `PlayerMain` fills it from `--data-root`), owns the mount, exposes
  `DataRoot()` / `DataFileSystem()`, and hands the mount to `RenderSubsystem`, `UISubsystem`
  (constructor arguments). Missing root: error + `host.RequestExit(1)`; the mount still points at
  `<exeDir>/Data` so every miss on the way out names the place a dist would need. The
  application host now honors an exit requested during `Configure`/`OnStartup` (it used to be
  overwritten when `Start` completed).
- `Tools.Editor` main: `ResolveDataRoot(argc, argv)` or exit 1; `EditorAppConfig::dataRoot`
  feeds the editor's own mount (the `UIHost`'s VG shaders), the fonts, the baseline assets seeded
  into new projects, the embedded runtime (`SetDataRoot`, no re-walk), and the export.
- `Tools.Export` / `Tools.Mcp`: `ResolveDataRoot(argc, argv)` for the export's shader cook
  (`ExportOne` / `ExportAll` take `dataRoot`; `RegisterProjectExportTool` too).
- `SampleApp::Run` (RHI samples): resolves from argv, exposes `DataRoot()` / `DataFileSystem()`
  / `DataPath(relative)`. `DefaultApplication` samples use the base class's; bare `IApplication`
  samples (`UISandbox`, `InputActions`, no argv) call `FindDataRoot()` themselves.
- Tests: `FindDataRoot()` from the test binary (under `Bin/`) finds the repo's `Data/`;
  `foundation.rhi.testsupport` exports `DataRoot()` / `DataFileSystem()` / `DataPath()` for the
  GPU probe tests; other tests keep a two-line static fixture.

Consumers take `vfs::IFileSystem&` (the data mount), never a path:

- `ShaderSystemHost::Initialize(device, dataFileSystem, policy)`: dev mode when a compiler
  exists and `Shaders/` holds stage sources (`.hlsl`/`.hlsli` - a `FileShaderSourceProvider`
  over the mount, hot reload through the mount's change source), else the cooked
  `Shaders/shaders.dpak` read through the mount. The folder alone is not "sources": a dist
  stages only the pack under it, and an editor dist ships DXC too (2026-09-21 fix). No
  executable-dir or cwd probing for the pack any more.
- `FileShaderSourceProvider::Initialize(fileSystem, folder)` is also the compiler's
  `IShaderIncludeResolver`: `#include`s resolve through the same mount (a custom DXC include
  handler in `foundation.shaders`; `CompileOptions::includeResolver` replaces the native `-I`
  paths, so a pak-backed mount works too). A first-level include arrives bare and is looked up
  in the folder; nested includes carry the includer's directory.
- `UISubsystem`: VG shaders via the host + the built-in default font read from
  `Engine/Fonts/roboto/Roboto-Regular.ttf` (bytes -> `LoadFontFromMemory`); a dist without it
  logs once and relies on the project's cooked default font, as before.
- `ImguiSubsystem(device, framesInFlight, dataFileSystem)`, `UIHost(..., dataFileSystem)`.

## Shipped layouts

- Desktop dist (`StageShaderPack` in `Editor.Core` export): `<out>/Engine.Player`, sidecars,
  `Content.pak`, `player.xml`, `Data/.dataroot`, `Data/Engine/Shaders/shaders.dpak` (cooked from
  `<dataRoot>/Shaders` for the preset's platform). `FindDataRoot()` from the player's directory
  finds `<out>/Data`.
- Web dist: the same tree in the serving folder. `WebMain` fetches `Data/.dataroot` and
  `Data/Engine/Shaders/shaders.dpak` into MEMFS (after `player.xml` / the content pak), so the walk
  from `/` finds `/Data`. The sample web builds (`WebScene`, `TerrainPlayground`) preload the
  repo's `Data/.dataroot` + the cooked pack at the same paths.

## Rules

- Never bake a path at build time and never probe "beside the exe" for one file: put it under
  the data root and read it through the mount. The engine `Context` knows nothing about data
  roots; the mount is an application-layer object passed down explicitly.
- A new consumer of engine data takes `vfs::IFileSystem&` in its constructor or `Initialize`.
- The layout under the root is spelled once (the constants above); the export and the web
  preloads write to the same names.
