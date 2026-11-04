# Plugins

> STATUS: APPROVED 2026-11-03 (user), on branch `plugin`. Sized L, in phases X0 to X6, green after each.
> Origin: the 0.1 online system needs a Steam backend, which is the engine's first real plugin;
> the user asked for a plugin API that carries it and is shaped for arbitrary plugins on
> every platform we target (Linux, Windows, web, later Android, iOS and consoles). Rulings so far
> (user, 2026-11-03) are listed under "Decided". The online service itself (the `Online` domain,
> its Local backend and the Steam plugin) is a separate spec written after this one lands; it
> is this spec's first customer. Read CONVENTIONS.md first.

## Goal

A plugin is a source package that adds to the engine without editing the engine: runtime
code, asset types and their cooks, headless tooling, editor UI, project settings and what ships
on each platform. A project enables the plugins it uses, per platform. The editor, the CLI
tools and the player all see the same enabled set. The game's own native code is a plugin
like any other.

Not goals: binary plugins over a C ABI (ruled out, see Decided), a plugin marketplace or
downloader, per-platform settings overrides (later), and Android, iOS or console support (the
design leaves room for them; nothing for them is built here).

## Decided (user, 2026-11-03)

1. **Source plugins, two link modes.** Players link plugins statically everywhere. Loading
   from a shared library is a desktop development convenience only (the editor and the tools).
2. **Official plugins are compiled into the export templates**, dormant unless the project
   enables them. A project whose enabled set a template does not cover gets a player built for it
   by the exporter, as native game code does today. Requiring a toolchain at export "may be
   acceptable, but let's see".
3. **Layout:** `Code/Plugins/<Name>/<Name>.<Part>`. The parts follow the hosts that load them:
   `<Name>.Engine`, `<Name>.Pipeline`, `<Name>.Tooling`, `<Name>.Editor` (Decision 2).
4. **The game's native code is a plugin.** `ProjectSettings::nativeModule` goes away; the
   game's package is one entry of the project's plugin list.
5. **The manifest is XML.**
6. **Plugins are found in two places:** the engine's `Code/Plugins/` and the project's own
   folder (for the game's code and project-local plugins). Outside repos (the flagship game,
   console SDK wrappers) are reached through the project.
7. **A project enables a plugin per platform.**
8. **The CLI tools are shared builds** for the purpose of loading a project plugin's pipeline
   and tooling parts from shared libraries, and the release distribution ships them alongside the
   editor (today it ships static builds only).
9. **The word is plugin.** Types, files, folders, targets and options say Plugin, never
   Extension. The folder and Imgui's names were renamed before X0 (2026-11-03).
   A plugin's code starts its own top-level namespace (`imgui`, `steam`), not one under a
   `plugins` namespace.

Not decided (the user's open thought, no effect on this plan unless ruled): building the editor
and tools as shared libraries by default, with static builds only for players.

## What exists (checked)

- **The plugin contract** (`Code/Foundation/Runtime/Plugin.cppm`): `IRuntimePlugin` with
  `Name()`, `OnLoad(SubsystemManager&)`, `OnUnload(SubsystemManager&)`, and the exported C factory
  `CreatePlugin` (`CreatePluginSymbol`).
- **The plugin host** (`Code/Foundation/Runtime/PluginHost.cppm`, class `PluginHost`): `Add` (static, caller owns the
  object) and `Load(path)` (shared library). During `OnLoad` it records every type and
  serializable the plugin registers, plus whatever extra `IRegistrationRecorder`s see (the scene
  contribution recorder), and reverses them on unload. `UnloadAll(closeLibraries=false)` is the
  hot-reload mode. This is kept, renamed `PluginLoader` in X1, and becomes the lower half of the
  new `PluginHost` (Decision 4).
- **The ship stub** (`Code/Engine/Engine.Player/ShipMain.cpp`): references one
  `extern "C" CreatePlugin` so a game archive's registration links without whole-archive or
  forced references. Generalised by the generated registry (Decision 3).
- **Native code today** (`Documentation/Archive/game-native-code-history.md`):
  `ProjectSettings::nativeModule` (`Code/Engine/Engine.Project/ProjectModule.cppm:117`) is one
  project-relative library path. The editor loads it through `PluginHost` from a staged copy
  (`Code/Editor/Editor.App/EditorNativeModule.cppm:47-70`), and only a shared editor can.
  The exporter builds `Engine.GamePlayer` with the baked CMake and compiler over the baked engine
  root, adding the game through `ENGINE_GAME_NATIVE_DIR` / `ENGINE_GAME_NATIVE_TARGET`
  (`CMakeLists.txt:40-43, 885, 903`), in `<project>/.cache/ship-<config>`. Web presets skip it.
  The editor's dev builds use a separate shared engine tree in `<project>/.cache/native-dev`.
- **The composition is a fixed list**: `kAllDomains` in
  `Code/Engine/Engine.Composition/CompositionImpl.cpp:54-63` (15 entries), built once into a
  function-local static by `FullComposition()`. A `DomainModule`
  (`Code/Engine/Engine.Domain/Domain.cppm:34`) carries an id, `dependsOn`, the scene installer,
  the reflection registrar, a script facade registrar and resource modules.
- **Script services are found by type** (since 2026-11-03): a facade asks
  `script::CurrentService<T>()`, and the run host answers with its runtime binding, then the
  run's services (`foundation.run`: `RunBase::Register`/`Add`), then the application's
  (`SubsystemManager::RegisterService`, which each subsystem calls for its own). A plugin offers
  per-run state on each run, from a listener on the run manager, and app-wide state on the
  manager; no list names either. The `Http` facade is the model for an asynchronous facade
  (request id, handler delegate, `cancel`).
- **Pipeline registration is a fixed list called by four hosts**:
  `Code/Pipeline/Pipeline.Registration/Registration.cppm` (`RegisterPipelineTypes`,
  `RegisterAllBuilders`, `RegisterAllImporters`, `RegisterAllCreators`), called from
  `Tools.Editor`, `Tools.Cook`, `Tools.Export` and `Tools.Mcp` `Main.cpp`.
- **MCP tools** have a hand-bumped count, `kEngineToolCount = 33`
  (`Code/Editor/Editor.Mcp/ProjectTools.cppm:749`).
- **The one plugin today**: `Code/Plugins/Imgui` (module `plugins.imgui`, target
  `Plugins::Imgui`, namespace `imgui`; renamed from Extensions on 2026-11-03), behind `OPTION_PLUGIN_IMGUI`
  (`CMakeLists.txt:140, 542`; the define `OPTION_HAS_PLUGIN_IMGUI`, `:235`) and linked directly by ten framework samples
  (`Code/Samples/CMakeLists.txt:18-30` gates them on the option). TerrainPlayground and WebScene
  link it in their web builds too. No manifest, no project enablement.
- **Export templates** (`Code/Editor/Editor.Project/ExportTemplate.cppm:44-63`, `template.xml`):
  id, name, platform (`Linux64`, `Win64`, `Web`), config, compiler, engine version, player binary,
  sidecars, symbols. Nothing about plugins.
- **Export presets** (`Code/Editor/Editor.Project/ExportPreset.cppm:43-63`): per platform, with
  `additionalFiles`.

## Prior art (surveyed 2026-11-03)

Traktor (`code/*`, the Features in `data/Source/System/Platforms/Features`), ezEngine (`ezPlugin`,
`ezStartup`, `*.ezPluginBundle`, `ezPlatformProfile`) and LumixEngine (`ISystem`, studio plugins,
the generated `plugins.inl`) were read for this spec. What we take:

- **Traktor:** a Feature's per-platform blocks make availability explicit and carry what ships;
  `exclusiveIdentifier` models "choose one backend"; runtime, editor and pipeline settings are
  kept apart.
- **ezEngine:** editor code and runtime code in separate binaries; required and exclusive
  bundles; per-platform config as reflected types edited by the generic property grid; game code
  as one more bundle.
- **Lumix:** the same source builds static or dynamic, with a generated registration list rather
  than linker side effects; runtime and editor code side by side per plugin.

What none of them does, and this design must: one manifest from which everything else derives
(Traktor writes a module's identity in about eight places, ezEngine in four); explicit
registration that cannot be silently dropped by the linker; typed settings per plugin; a
project that still opens when a plugin is missing (Lumix refuses with `PLUGIN_NOT_FOUND`);
export rules per platform.

## Decision 1 - one manifest per package

`<package>/Plugin.xml` is the only place a plugin's identity, parts, dependencies and
platforms are written. It is a reflected `PluginManifest` read with Xml.Serialization, like
every other data file:

```xml
<root>
	<array name="dataVersions" count="1">...</array>
	<string name="id">steam</string>
	<string name="name">Steam</string>
	<string name="version">0.1.0</string>
	<string name="engineVersion">0.1</string>
	<array name="dependsOn" count="1">
		<object><string name="id">online</string><string name="version">0.1</string></object>
	</array>
	<array name="provides" count="1"><string>online.backend</string></array>
	<array name="parts" count="3">
		<object><string name="kind">Engine</string></object>
		<object><string name="kind">Tooling</string></object>
		<object><string name="kind">Editor</string></object>
	</array>
	<array name="platforms" count="2">
		<object>
			<string name="platform">Linux64</string>
			<array name="deploy" count="1"><string>ThirdParty/steam/linux64/libsteam_api.so</string></array>
		</object>
		<object>
			<string name="platform">Win64</string>
			<array name="deploy" count="1"><string>ThirdParty/steam/win64/steam_api64.dll</string></array>
		</object>
	</array>
</root>
```

- **id** is lower case and stable; it keys the project's entry, the settings block and the
  generated entry symbols. **name** is for display.
- **version** and **engineVersion**: the engine refuses a manifest whose engine version it does
  not satisfy, with a message naming both. Versions are `major.minor[.patch]`. An engine satisfies
  a plugin of its own major at or below its own version; before 1.0 the minor must match as well,
  since a 0.x minor release may break plugins (`EngineSatisfies`, settled in X0).
- **dependsOn**: other plugin ids with a minimum version. Loading is in dependency order; a
  missing or refused dependency disables its dependents, each with a message.
- **provides**: named slots, at most one enabled provider per slot per platform (Traktor's
  exclusive group). `online.backend` is the first.
- **parts**: which of the four parts exist. The module names are derived: `<Name>.<Kind>`.
- **platforms**: the platforms the plugin supports, each with the files it deploys there
  (paths relative to the package). A platform not listed is unsupported. Android, iOS and console
  entries later add their non-C++ pieces (Java, Objective-C, manifest entries) as more fields of
  the same record.

The CMake helper `util_add_plugin(<dir>)` (`cmake/Plugins.cmake`) reads the manifest (a line
reader over the writer's shape; see Gotchas) and adds each part's folder in load order. The part's
CMakeLists starts with `util_add_plugin_part()`, which declares `${PLUGIN_PART}` as an engine
library through `util_add_engine_library`, linked to the package's previous part and to the same
part of each plugin it depends on (which must be added first), so the part names, their link to
each other and the dependency order are never written twice. The part then adds its sources and
its engine links.

## Decision 2 - parts follow the hosts that load them

| Part | Contributes | Loaded by |
|---|---|---|
| `<Name>.Engine` | resource types, domains, subsystems, per-run services, script facades, its settings type | player, editor (its embedded runtime), Cook, Export, Mcp |
| `<Name>.Pipeline` | asset types, importers, builders, creators, cook steps | editor, Cook, Export, Mcp |
| `<Name>.Tooling` | MCP tools, export hooks and checks, settings validation (headless) | editor, Export, Mcp |
| `<Name>.Editor` | pages, actions, inspectors, play-in-editor hooks | editor |

The same split the editor already made between `Editor.Project` (headless) and `Editor.Core`
(UI) is applied to plugins: a plugin never puts an asset type in its editor part, because
the CLI cook and the standalone MCP host would not see it. Every part links only what its hosts
have: `Engine` links Engine and Foundation; `Pipeline` links its `Engine` part and Pipeline;
`Tooling` links `Pipeline` and `Editor.Project` (never `Editor.Core`); `Editor` links `Tooling` and
`Editor.Core`.

Each host has a **role** (player, editor, cook, export, mcp) and loads, for every enabled
plugin, the parts the table gives that role.

## Decision 3 - registration is explicit, and the build generates the list

Each part has exactly one entry point, a C function named from the manifest:

```cpp
extern "C" const foundation::runtime::PluginPart* PluginPart_steam_Engine();
```

It returns a static part object (the `IRuntimePlugin` successor) whose `OnLoad` performs every
registration explicitly. There is no registration by static initialiser, which is already the
rule.

- **Static (players, and the static editor and tools):** `util_link_plugins(<target> ROLE <role>
  PLUGINS <id>...)` links the parts that role loads of the named plugins and their dependencies,
  and generates `<target>_PluginRegistry.cpp`, which defines `LinkedPlugins` (declared by
  foundation.runtime with C linkage): for each plugin, its manifest's text and its linked parts'
  entry points. Referencing the entries pulls each part's archive in, exactly as `ShipMain.cpp`
  does for one `CreatePlugin` today, with no whole-archive or forced references; carrying the
  manifest's text lets a player with no package files on disk still check versions, dependencies
  and slots. `ShipMain.cpp` becomes the generated registry plus `PlayerMain`. A part's source
  writes its entry as `PLUGIN_PART(&Load, &Unload)` (`Runtime/PluginEntry.h`); the build supplies
  the symbol, id and kind from the manifest.
- **Shared (desktop development):** the host loads a part's library and resolves the same symbol.
- **Linked is not enabled.** The registry lists what is compiled in; the plugin host activates
  only the plugins the project enables for the running platform. A dormant plugin registers
  nothing, so it costs nothing at run time.

## Decision 4 - the plugin host and the load context

`PluginHost` (Foundation.Runtime, over `PluginLoader`) owns the enabled set for one process:

1. Reads the project's enabled list for the running platform and the role.
2. Resolves each id to a part: from the static registry first, then from a package's shared
   library (development).
3. Checks versions, dependencies and `provides` slots; refuses with a message, never a crash.
4. Calls `OnLoad(PluginContext&)` in dependency order, under `PluginLoader`'s recording, and
   unloads in reverse.

`PluginContext` replaces `OnLoad(SubsystemManager&)`. It offers what each role allows:

- **every role:** the subsystem manager, the type and serializable registries, the plugin's
  settings object (Decision 5), logging.
- **Engine part:** `EnginePluginServices` (`AddDomain`, `AddSceneManager`), found by type.
  Per-run state goes on the runs themselves and app-wide state on the subsystem manager, where
  facades find it (see "What exists").
- **Pipeline part:** the builder, importer and creator registries.
- **Tooling part:** the MCP tool registry and export hooks.
- **Editor part:** the action registry, page factories, the editor context.

**The composition becomes engine domains plus plugin domains.** `kAllDomains` stays as the
engine's list; `FullComposition()` builds from it plus the domains the enabled plugins added,
and is built after the plugins load (a host that composes before loading them is a bug the
tests catch). Hot reload in the editor rebuilds it.

**Every host loads through one path.** The four hand-written pipeline calls in the tools' `Main.cpp`
become: register the engine's surface (`Pipeline.Registration`, unchanged), then
`PluginHost::Load(role)`. The player and the editor do the same for their roles.

## Decision 5 - typed settings per plugin, and the project's list

The project file gains a `plugins` list, appended (a manifest saved before it reads none). The
data version bumps in X5, with the migration that turns a non-empty `nativeModule` into the
game's package entry, when native code becomes a package (ruling 2026-11-04: until then the
editor, the player and export still read `nativeModule`):

```xml
<array name="plugins" count="2">
	<object>
		<string name="id">steam</string>
		<array name="platforms" count="2"><string>Linux64</string><string>Win64</string></array>
		<object name="settings">...</object>
	</object>
	<object>
		<string name="id">paperkid</string>
		<string name="path">Native</string>
		<array name="platforms" count="0"></array>
		<object name="settings"></object>
	</object>
</array>
```

- **id** names the plugin; **path** locates a project-local package (relative to the
  project); engine packages need none.
- **platforms**: the platforms it is enabled on; empty means every platform its manifest
  supports. A platform the manifest does not support is reported, not silently kept.
- **settings**: a versioned payload of the plugin's reflected settings type, labelled as the
  project's settings are. The entry always keeps them as the XML elements it read (ruling
  2026-11-04), through the archive's `RawRemainder`, which captures a section's remaining
  elements on read and re-injects them on write (and through the binary serializer, a copy,
  carries the same bytes in a framed region). The project is read before any plugin loads, so
  one form serves every host: the `Engine` part reads them into its type at load
  (`PluginContext::Settings<T>()`, the type's defaults when the entry has none), the host holds
  that object while the plugin is loaded, and Project Settings and `plugin_settings_set` edit it
  and write it back into the entry (`WritePluginSettings`). Settings that do not read as the type
  give the defaults, marked unread, and nothing writes over them.
- **A missing plugin keeps its entry.** When an id resolves to nothing (not in the build, or
  refused), the entry and its settings are kept unread and written back unchanged, the project
  opens, and the user is told which plugin did not load and why.

## Decision 6 - export

- **The template records what it carries**: `template.xml` gains `plugins` (id and version
  for each plugin compiled in). The release distribution builds templates with every official
  plugin that supports the template's platform.
- **Choosing the player:** if the template carries every plugin the project enables for the
  preset's platform, at satisfied versions, the template player is used. Otherwise the exporter
  builds a player for the project (the existing `.cache/ship-<config>` build, extended from one
  native target to the enabled package list), and reports why.
- **Deploy:** each enabled plugin's `deploy` files for the platform are staged beside the
  template's sidecars. The `Tooling` part's export hook runs after staging, for what data cannot
  express.
- **Platforms:** a plugin enabled for a platform its manifest does not support is refused at
  export with the reason. A plugin not enabled for the preset's platform is simply absent;
  game code that needs a capability asks for it (the online spec's `supports`), never for a
  plugin by name.
- **Web:** static only. A web export with an enabled plugin that is not in the web template
  needs the per-project wasm build, which does not exist yet (today's native-code export skips
  web). Built in X6.

## Phases

- **X0 - manifest and package layout** (built 2026-11-03). `PluginManifest` (reflected, versioned) and its reader;
  `util_add_plugin`; Imgui moves from `Code/Plugins/Imgui` to `Code/Plugins/Imgui/Imgui.Engine`
  with a manifest listing Linux64, Win64 and Web; the part's target is `Imgui.Engine`
  (`Plugins::Imgui.Engine`) and its module `imgui.engine`, after the engine's rule that a module
  is its target in lower case. The framework samples are not
  projects: the ten that use Imgui keep linking the part directly, and the web builds of
  TerrainPlayground and WebScene (WebScene's tweak UI, `Code/Samples/WebScene/CMakeLists.txt:40`)
  must still build and show the panel, so X0 runs the wasm build. Tests: manifest round trip,
  refusals (bad id, unknown part kind, engine version unmet), the CMake helper declares the part.
- **X1 - plugin host and context** (built 2026-11-03). Today's `PluginHost` renamed `PluginLoader` (file and call
  sites); `PluginPart`, `PluginContext`, the new `PluginHost` over `PluginLoader`; dependency
  order, version and slot checks; the generated static registry. Imgui's `Imgui.Engine` gets its
  entry, whose load registers nothing yet (X2). Native code keeps `IRuntimePlugin` and
  `CreatePlugin` until X5, which retires them (ruling 2026-11-03: native code has no id or
  manifest before X5, and the entry symbol is named from the id). Tests: order, missing dependency
  disables dependents, slot conflict refused, dormant plugin registers nothing, unload reverses
  everything.
- **X2 - Engine part contributions** (built 2026-11-03). Typed services on the host and context
  (`PluginHost::Provide`, `PluginContext::Find<T>`); `EnginePluginServices`: `AddDomain` (a
  plugin domain's resource modules, reflection and facade join the full composition, rebuilt in
  place; a domain with a scene installer is refused) and `AddSceneManager` (through
  `SceneModuleContributions`, which reaches live scenes); script services found by type through
  the run and the application, so a plugin's facade works as the engine's do. Where the role has a
  window (player, editor), the context offers the client side (graphics device, shell, the data
  mount), so a plugin such as Imgui sets itself up on load instead of each app adding its
  subsystem by hand (the user's suggestion, 2026-11-03). Settled when built: one plugin host per
  process, over the engine runtime's subsystem manager (every process runs one `DefaultApplication`;
  in the editor that is the embedded app over `m_runtimeContext`, never the outer host's manager);
  `DefaultApplication::CreatePluginHost(role)` makes it with the client services (`ClientServices`:
  the app's host and data mount) and `EnginePluginServices`, and the app's `OnShutdown` unloads it.
  Tests: a plugin's domain and scene manager arrive and leave
  with it; services resolve run first, then the application.
- **X3 - project list and settings.** The `plugins` list (appended); the entry type
  (`EnabledPlugin`, in `foundation.runtime` since the host takes it) with its settings kept as
  XML; `PluginHost::Load` takes the project's entries and loads those enabled on its platform
  (absent elsewhere, refused on a platform the plugin does not support); typed settings through
  the context; each entry's state in the host (`PluginStatusOf`); missing plugins kept and
  reported at project open; Project Settings rows, a group per entry; `plugin_list`,
  `plugin_enable`, `plugin_disable` and `plugin_settings_set` (`McpGuide.md` and the tool count
  in the same commit). The editor loads no
  plugins until X4, so for a real project every entry shows as not loaded; the dialog and the
  tools are tested over a fixture host. A wasm build's platform tag becomes "Web", the tag
  export and the manifests use. Tests: round trip of an unknown entry, typed settings read and
  written, per-platform enablement.
- **X4 - every host by role**, in three layers (user ruling, 2026-11-04). The hosts link one
  list of plugins (`UTIL_HOST_PLUGINS`, the engine's official ones; Imgui, the framework samples'
  debug UI, is in none), and loading a project package's parts from shared libraries moves to X5,
  where project packages first exist and are built.
  - **X4a** (built 2026-11-04): the player at startup, the editor at each project open (through
    the embedded app, which owns the process's plugin host and replaces it per project), and
    Cook, Export and Mcp over a subsystem manager of their own (`ToolPlugins`; Mcp at each
    `project_open`), each loading the parts its role takes; an Editor part is given the editor's
    context, and an Engine part the engine's service in every host, the tools included, so the
    scene managers and domains it adds join a tool's scenes (the standalone MCP host's scene tools
    know a plugin's components). Tests: each tool's role loads exactly its parts; a project's plugins load with it in
    the editor and unload as it closes; the app's host is replaced.
  - **X4b** (built 2026-11-04): the `Pipeline` contributions through `PipelinePluginServices`
    (builders, importers, creators into the host's registries, which gain keyed removal), recorded
    and reversed with the plugin's unload; the tools pass their registries to `ToolPlugins`, and the
    editor attaches the service over its own to each project's host. Test: through the standalone
    MCP host, a project that enables a plugin creates its asset with its creator and cooks it with
    its builder, and a project that does not has neither.
  - **X4c** (built 2026-11-04): the `Tooling` contributions through `McpPluginServices`: a
    plugin's MCP tools join every server the service serves (the standalone host's, there before
    the project opens, and the editor's, started after its plugins load) and leave each with their
    plugin; none takes a name a server holds. `McpServer` gains tool removal, which ends a call in
    flight to the tool. Export hooks, the other Tooling contribution, come with export in X6.
    Test: a plugin's tool is served by a server there before it loads and one attached after, and
    leaves both.
  Tests run each host's load step against a linked four-part fixture; the real tools link no
    fixture.
- **X5 - native code is a plugin** (built 2026-11-04; rulings that day: a project's packages live
  in `Plugins/<Name>/`, the migration converts the setting only, Build and Reload act on all the
  project's packages, the shared CLI tools ship with X6). NativeSample is the package
  `Plugins/NativeSample` (`Plugin.xml`, `NativeSample.Engine`); the project data version is 11,
  reading a 9 or 10 `nativeModule` as the game's entry (`Plugins/<Game>`, its id in lower case), so
  an old game's code is ported by hand to a part entry; `ENGINE_PROJECT_PLUGINS` replaces the
  game-native variables, adding the packages after every engine target, building them as one
  target (`ProjectPlugins`), listing each part's library (`plugin-libraries.txt`) and linking them
  into `Engine.GamePlayer` through the generated registry (the ship stubs are gone); the editor, a
  player in a project directory and the tools add a project's packages from that list
  (`AddProjectPackages`), the editor from fresh copies, and only a shared build loads them; Build
  Project Plugins builds them in `.cache/plugins-dev`, and Reload Project Plugins remakes the
  plugin host inside the scene bracket, the old libraries kept mapped; the scaffold generates the
  game's package with an Engine part. `IRuntimePlugin`, `CreatePlugin` and `PluginLoader`'s
  `Add` and `Load` are deleted, and the cross-boundary shared-lane test loads a part. Tests:
  NativeSample's package, the scaffold's output, the migration of version 10 projects.
- **X6 - export** (built 2026-11-04; ruling that day: the editor ships as a shared build, with the
  CLI tools beside it). In layers:
  - **X6a**: a web build declares only a plugin's Engine part (no editor or tool runs there), which
    lets a project package with Tooling or Editor parts configure for the web.
  - **X6b**: `util_link_plugins` writes `<binary>.plugins` (each linked plugin's id and package
    folder); a created template records each plugin's id and version in `template.xml` and carries
    its Plugin.xml and deploy files for the platform under `plugins/<id>/`.
  - **X6c**: per preset, the template's player when it carries every plugin the project enables on
    the platform at a version at least the one known and no project package is enabled there; a
    player built for the project otherwise (`ENGINE_GAME_PLAYER` for one without packages); a
    refusal for an entry enabled on a platform its plugin does not support or a plugin in no
    package; each enabled plugin's deploy files staged beside the player; the result says which
    player and why. The web per-project build is the existing wasm ship build.
  - **X6d**: export hooks, the other `Tooling` contribution (`ExportPluginServices`), run over the
    staged dist by the exporter, the editor and both MCP hosts; one that fails fails the export.
  - **X6e**: the editor distribution is a shared build (`clang-reldbg-shared`,
    `msvc-reldbg-shared`) shipping the CLI tools beside the editor with the engine's libraries; the
    templates stay static. A released editor loads a project's packages built against the same
    engine, but builds them only with an engine checkout: shipping an engine SDK for that is beyond
    this spec.
  Tests: a template records and carries its plugins; every branch of the player choice; an export
  stages an enabled plugin's deploy files and refuses what cannot ship; hooks run in order after
  staging and a failing one fails the export; NativeSample's export hook in Integration.Plugins.

## Gotchas

- **Reading the manifest from CMake** (settled in X0). CMake has no XML reader, and a host tool
  at configure time would not exist on the first configure, so `cmake/Plugins.cmake` reads the
  shape the engine's XML writer emits, one element per line, tracking the array and object nesting
  to find the top-level id, the part kinds and the dependency ids. Every manifest a configure reads
  is recorded in `<build>/plugin-manifests-read.txt`, and Runtime.Tests reads each one again with
  the C++ reader and compares; a four-part fixture with dependencies is always among them, and a
  second test proves the writer emits that fixture, so a written manifest is one the build reads.
- **Scripts and absent plugins.** A script that calls a plugin's facade compiles only
  where the plugin is enabled. Per-platform enablement therefore means per-platform script
  compile results. Platform services should be reached through engine facades with providers
  (Online's shape), and a plugin-specific facade used only from code that runs where it is
  enabled; the cook reports a script that uses a facade absent on the platform it cooks for.
- **The composition was a function-local static.** Everything that cached `FullComposition()`
  before plugins load must be found and fixed in X2.
- **Type id collisions** between plugins are refused at load with both names, not asserted (built
  2026-11-04): a part whose load offers a type under an id another type holds, in the type or the
  serializable registry, is refused naming both types and the holder (another plugin, itself or the
  engine), and the plugin's parts that loaded are undone. A held id keeps its type and factory, so
  a plugin registering the engine's type again no longer swaps in a factory of its own library.
- **The template's static registry lists every official plugin**, so dormancy must be real:
  a plugin's static objects may not do work at static-initialisation time.
- **Hot reload** rebuilds the composition and reinstalls run services; a running play-in-editor
  session is stopped first, as the native module reload does today.
- **The shared CLI tools** (Decided 8) are a new distribution item: X4 adds them to the release
  distribution job beside the editor, with the engine's shared libraries they load against.

## Tests (the contract)

Each phase lands with its tests (listed per phase). Across the spec: a plugin with all four
parts, kept under the tests' fixtures, proves every host sees exactly the parts its role loads,
in both link modes, on the clang, gcc, clang-shared, wasm (where it applies) and ASAN lanes.
