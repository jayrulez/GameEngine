# Getting started (for developers)

Welcome. This is the orientation document: what the project is, how the code is
organized, the architecture at a bird's-eye level, how to build and run it, and
the working rules that get a change accepted. It stays shallow on purpose; each
section points to where to read further.

## What this is

A C++23 game engine plus a full editor, in one repository. Defining traits:

- **C++23 modules throughout.** Every library is a named module
  (`foundation.core`, `foundation.gui`, `engine.render`, ...). There are no
  public headers; consumers write `import foundation.gui;`.
- **Abstract RHI** with Vulkan 1.3 as the primary desktop backend and WebGPU as
  a first-class second backend (wgpu-native on desktop, the browser's WebGPU
  under Emscripten). A DX12 backend builds on Windows.
- **One editor executable** (`Tools.Editor`) with a project manager, scene
  editing, the asset pipeline and cook, play in editor, and export.
- **A real runtime**: scene/ECS, PBR renderer (clustered forward, CSM/spot/
  point shadows, IBL, SSR, decals, particles, post stack), physics (Jolt),
  audio (miniaudio), input, networking, game UI, and two scripting backends
  (AngelScript, Luau) behind one backend-neutral registry.
- **Web is a shipping target.** The same game exports to the browser
  (Emscripten + WebGPU), with per-target cooked asset variants.

Read next: [BUILDING.md](BUILDING.md) (requirements and build commands), and
`Documentation/Plans/backlog-0.1.md` (what is queued for the 0.1 release).

## Repository map

```
Code/            All engine, editor and tool sources (see the next section)
Documentation/   Tracked docs: this file, Systems/, Specs/, Process/, Guides/, ...
Data/            Engine data (shaders, fonts), the editor's interface, templates,
                 the samples' and tests' data, and the sample projects
ThirdParty/      Vendored dependencies (SDL, Jolt, DXC, doctest, imgui, ...)
Tools/           Host tooling support (the Emscripten patch)
cmake/           Build-system modules
Bin/             Build output: Bin/<Config>/<Platform>-<Compiler>/
build/           CMake build trees, one per preset (build/clang, build/gcc, ...)
KNOWN_ISSUES.md  Triaged open defects, newest first; read it before debugging
```

Do not delete things under `Bin/` wholesale; user projects can live beside the
binaries.

## How the code is organized

`Code/` is split by role, and within each role the rule is
**folder == CMake target == module name**, with tests as a sibling target:

```
Code/Foundation/GUI           -> target GUI,       module foundation.gui
Code/Foundation/GUI.Tests     -> target GUI.Tests  (doctest executable)
Code/Engine/Engine.Render     -> target Engine.Render, module engine.render
```

The roles, in dependency order (lower layers never know about higher ones):

- **Foundation/**: engine-agnostic libraries. `Core` (containers, UTF-8
  strings, math, reflection and RTTI, jobs), `RHI` with `RHI.Vulkan`,
  `RHI.WebGPU` and `RHI.DX12`, `Render` and `RenderGraph`, `Scene` (ECS), `VG`
  (vector graphics), `Fonts.*`, `GUI` and `GUI.Toolkit` (the retained-mode GUI
  the editor and games are built with), `Script.*` (the scripting backends),
  the `Content`, `VFS` and resource stack, `Image`, `Geometry`, `Texture`,
  `Model`, `Audio`, `Input`, `Physics`, `Net`, and their `.Resource`
  (cooked-format) siblings.
- **Engine/**: the runtime that composes Foundation into a game: subsystems
  (`Engine.Render`, `Engine.Physics`, `Engine.Audio`, `Engine.Script`,
  `Engine.GUI`, `Engine.Animation`, ...), `Engine.Scene`, `Engine.GameInstance`
  (a running game as a first-class object), `Engine.DefaultApp` (the standard
  application that both the player and the editor's embedded runtime use), and
  `Engine.Player` (the shipped game executable).
- **Pipeline/**: importers, asset builders, and the cook. Its composition root
  is `Pipeline.Registration`: all builder, importer and type registration goes
  through it. Pipeline targets never depend on the GUI.
- **Editor/**: the editor application (`Editor.App`), scene editor, inspector,
  asset pages, viewport tools, per-domain editor libraries (`Editor.Script`,
  `Editor.PropertyAnimation`, ...).
- **Tools/**: executables. `Tools.Editor`, `Tools.Cook`, `Tools.Export`,
  `Tools.ShaderPack`, `Tools.Mcp` (agent access via MCP) and
  `Tools.ModelViewer` (a standalone viewer for curating model assets).
- **Samples/**: runnable samples; `WebScene` is the full-renderer comparison
  scene (desktop + browser), `Framework/` holds the shared sample scaffolding.
- **Integration/**: cross-cutting integration test suites (script facades,
  texture compression probes, ...).
- **Extensions/**: optional integrations (Dear ImGui).
- **Deprecated/**: code kept compiling on its way out: `UI` holds the old UI
  framework (`foundation.ui` and its satellites) and its UISandbox. Nothing
  outside `Deprecated/` links it, and it builds only with
  `-DOPTION_BUILD_DEPRECATED=ON` (off by default).

Layering rules worth internalizing early: Foundation never imports Engine;
Pipeline (and its tests) never import the GUI; editor-only state never lives in
runtime/resource wire structs; a domain declares what it contributes (scene
managers, reflection, its script facade, its resource modules) once, on its
`DomainModule`, and `Engine.Composition` is the one root every host composes
from (engine-composition.md).

## Architecture in one pass

- **Core**: UTF-8 everywhere (`char8_t` String/StringView; UTF-16 only at the
  Win32/DXGI edge). Hand-rolled reflection + RTTI drives serialization, the
  inspector, and script binding from the same metadata.
- **Graphics**: RHI abstracts the GPU; `RenderGraph` schedules passes;
  `Foundation/Render` is the scene-agnostic renderer (clustered forward
  lighting, shadow atlas + CSM, IBL/sky, SSR, decals, sprites, instancing,
  post-processing); `Engine.Render` extracts the ECS scene into it per view.
  Shaders are HLSL source assets cooked to per-backend bytecode (SPIR-V /
  DXIL / WGSL via DXC + naga).
- **Assets**: source assets live in a project's content database; builders cook
  them into product resources; export packs them (`Content.pak`, per-target
  variants for platform capabilities like BC vs ASTC textures). The VFS
  abstracts sources at runtime. Scene/prefab sources are XML text; export
  stages binary.
- **Scene/ECS**: value-pool component managers on a `Scene` that is pure data;
  subsystems attach per-scene state via scene-created hooks. Prefabs support
  nesting and overrides.
- **Runtime**: `Shell` is the platform layer (windows/input/loop);
  `DefaultApplication` (`engine.defaultapp`) owns subsystem setup; a
  `GameInstance` bundles the scenes, networking and input of one running game.
  The player runs one; the editor hosts them for play in editor.
- **Scripting**: one registry describes the reflected surface; each backend
  (AngelScript, Luau) emits its bindings from it. Behaviors attach to
  entities; a scene-level script tier exists; capability flags gate per-backend
  features. The AngelScript and Luau backends have in-editor debuggers.
- **UI**: `foundation.gui` is the retained-mode GUI (`.xml` markup, CSS
  sheets, layout, text, events, the `gui` script surface), with satellites for
  the editor's widgets, docking, the code editor and a game's screens
  (`GUI.Gamekit`). The editor and in-game UI both use it. See `Systems/gui-port.md`.
  The older `foundation.ui` is deprecated, in `Code/Deprecated/UI` with its
  UISandbox (`Systems/ui-framework.md`).

Further reading: every subsystem has a doc in `Documentation/Systems/`:
`renderer.md`, `scripting.md`, `physics.md`, `audio.md`,
`gui-port.md`, `networking.md`, `web-platform.md`, `editor.md`, ...

## Building

See [BUILDING.md](BUILDING.md) for the requirements, the presets and the build notes.

## Running things

```bash
# The editor starts on the project manager screen; create a project there.
Bin/Debug/Linux64-Clang/Tools.Editor
Bin/Debug/Linux64-Clang/Tools.Editor <dir>             # open a project directly

# The player runs a project directly.
Bin/Debug/Linux64-Clang/Engine.Player <projectDir> [--scene <source-db-path>]

# The renderer comparison scene, desktop Vulkan vs WebGPU:
Bin/Debug/Linux64-Clang/WebScene --vulkan
Bin/Debug/Linux64-Clang/WebScene --webgpu

# The GUI sandbox (the GUI's widgets and features, a page each):
Bin/Debug/Linux64-Clang/GUISandbox
```

A good first session: build, run `Tools.Editor`, create a project, drop some
assets in, press Play. Then run `WebScene --vulkan` to see the renderer
exercised end to end.

## Working on the code

Read `Documentation/Process/CONVENTIONS.md` before your first change: it is
the binding rule set, and reviews enforce it. The headlines:

- **Every addition lands with tests** (doctest, in the module's `.Tests`
  target). GPU-visible behavior prefers semantic pixel probes on real devices
  over image goldens.
- **Both compilers green** (clang and gcc, Debug) before a change is done.
  Which GCC works is in [BUILDING.md](BUILDING.md).
- **Style**: Allman braces, the root `.clang-format`, PascalCase methods,
  full descriptive names, no em or en dashes anywhere (ASCII hyphen only).
- **Module hygiene**: heavy third-party headers and reflection bodies go in
  implementation units, never module interfaces (GCC build-time blowup).
- **Docs move with behavior**: if you change how a system works, update its
  `Systems/` doc in the same commit.
- Recurring architecture rules (wire-format symmetry, cache invalidation by
  generation not pointer, UI mutation deferral, inspector dispatch entries for
  new `Ref<T>` fields, ...) are all listed in CONVENTIONS.md; skim them once
  so the review does not have to teach them.

## Documentation map

```
Documentation/
  GETTING-STARTED.md   This file.
  BUILDING.md          Requirements, presets and build notes.
  Systems/             The reference: one doc per shipped subsystem.
  Specs/               Feature specs: what is being built and why. A spec is
                       stamped BUILT when it ships and carries its review rulings.
  Process/             CONVENTIONS.md (binding rules incl. code style),
                       HANDOFF.md (review pass records).
  Guides/              How-tos (terrain authoring, adding a script facade, Emscripten setup).
  Plans/               The release backlogs (backlog-0.1.md, backlog-0.2.md),
                       design plans and weekly plans.
  UAT/                 Manual smoke-test checklists.
  Archive/             Historical/superseded documents.
```

Suggested first reads after this file: `Process/CONVENTIONS.md`,
`Systems/editor.md`, `Systems/renderer.md`, and then whichever `Systems/` doc
covers the area you want to work in. `KNOWN_ISSUES.md` at the root tells you
what is already known to be broken, so you do not re-diagnose it.
