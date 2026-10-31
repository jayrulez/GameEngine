# Getting started (for developers)

Welcome. This is the orientation document: what the project is, how the code is
organized, the architecture at a birds-eye level, how to build and run it, and
the working rules that get a change accepted. It deliberately stays shallow -
every section ends with pointers for drilling down.

## What this is

A C++23 game engine plus a full editor, in one repository. It began as a port
of the Sedulous engine (Beef) and has grown well past it. Defining traits:

- **C++23 modules throughout.** Every library is a named module
  (`foundation.core`, `foundation.ui`, `engine.render`, ...). There are no
  public headers; consumers write `import foundation.ui;`.
- **Abstract RHI** with Vulkan 1.3 as the primary desktop backend and WebGPU as
  a first-class second backend (wgpu-native on desktop, the browser's WebGPU
  under Emscripten). A DX12 backend builds on Windows.
- **One editor executable** (`Tools.Editor`) with Godot-style project
  management: scene editing, asset pipeline/cook, play-in-editor, export.
- **A real runtime**: scene/ECS, PBR renderer (clustered forward, CSM/spot/
  point shadows, IBL, SSR, decals, particles, post stack), physics (Jolt),
  audio (miniaudio), input, networking, game UI, and two scripting backends
  (AngelScript, Luau) behind one backend-neutral registry.
- **Web is a shipping target.** The same game exports to the browser
  (Emscripten + WebGPU), with per-target cooked asset variants.

Read next: the root `README.md` (requirements + build commands),
`Documentation/Archive/roadmap-history.md` (where the project is heading).

## Repository map

```
Code/            All engine/editor/tool sources (see next section)
Documentation/   Tracked docs: this file, Systems/, Specs/, Process/, Guides/...
Data/            Shared runtime data (shaders, bundled assets)
ThirdParty/      Vendored dependencies (SDL, Jolt, DXC, doctest, imgui, ...)
Tools/           Host tooling support (e.g. the Emscripten patch)
cmake/           Build-system modules
Bin/             Build OUTPUT: Bin/<Config>/<Platform>-<Compiler>/
build/           CMake build trees, one per preset (build/clang, build/gcc, ...)
KNOWN_ISSUES.md  Triaged open defects, newest first - read before debugging
```

Do not delete things under `Bin/` wholesale; user projects can live beside the
binaries.

## How the code is organized

`Code/` is split by ROLE, and within each role the rule is
**folder == CMake target == module name**, with tests as a sibling target:

```
Code/Foundation/GUI           -> target GUI,       module foundation.gui
Code/Foundation/GUI.Tests     -> target GUI.Tests  (doctest executable)
Code/Engine/Engine.Render     -> target Engine.Render, module engine.render
```

The roles, in dependency order (lower layers never know about higher ones):

- **Foundation/** - engine-agnostic libraries: `Core` (containers, UTF-8
  strings, math, reflection/RTTI, jobs), `RHI` + `RHI.Vulkan`/`RHI.WebGPU`/
  `RHI.DX12`, `Render` + `RenderGraph`, `Scene` (ECS), `VG` (vector graphics),
  `Fonts.*`, `UI` + `UI.Toolkit` (the retained-mode UI framework the editor is
  built with), `Script.*` (the scripting backends), `Content`/`VFS`/resource
  stack, `Image`/`Geometry`/`Texture`/`Model`, `Audio`, `Input`, `Physics`,
  `Net`, and their `.Resource` (cooked-format) siblings.
- **Engine/** - the runtime that composes Foundation into a game: subsystems
  (`Engine.Render`, `Engine.Physics`, `Engine.Audio`, `Engine.Script`,
  `Engine.GUI`, `Engine.Animation`, ...), `Engine.Scene`, `Engine.GameInstance`
  (a running game as a first-class object), `Engine.DefaultApp` (the standard
  application that both the player and the editor's embedded runtime use), and
  `Engine.Player` (the shipped game executable).
- **Pipeline/** - importers, asset builders, and the cook. Composition root:
  `Pipeline.Registration` (ALL builder/importer/type registration goes through
  it). Pipeline targets are UI-free by rule.
- **Editor/** - the editor application (`Editor.App`), scene editor, inspector,
  asset pages, viewport tools, per-domain editor libraries (`Editor.Script`,
  `Editor.PropertyAnimation`, ...).
- **Tools/** - executables: `Tools.Editor`, `Tools.Cook`, `Tools.Export`,
  `Tools.ShaderPack`, `Tools.Mcp` (agent access via MCP).
- **Samples/** - runnable samples; `WebScene` is the full-renderer comparison
  scene (desktop + browser), `Framework/` holds the shared sample scaffolding.
- **Integration/** - cross-cutting integration test suites (script facades,
  texture compression probes, ...).
- **Extensions/** - optional integrations (Dear ImGui).
- **Deprecated/** - code kept compiling on its way out: `UI` holds the old UI
  framework (`foundation.ui` and its satellites) and its UISandbox. Nothing
  outside `Deprecated/` links it, and it builds only with
  `-DOPTION_BUILD_DEPRECATED=ON` (off by default).

Layering rules worth internalizing early: Foundation never imports Engine;
Pipeline (and its tests) never import UI; editor-only state never lives in
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
  `engine.runtime.DefaultApplication` owns subsystem setup; a `GameInstance`
  bundles scenes + networking + input for one running game - the player runs
  one; the editor hosts them for play-in-editor.
- **Scripting**: one registry describes the reflected surface; each backend
  (AngelScript, Luau) emits its bindings from it. Behaviors attach to
  entities; a scene-level script tier exists; capability flags gate per-backend
  features. The AngelScript and Luau backends have in-editor debuggers.
- **UI**: `foundation.gui` is the retained-mode GUI (`.xml` markup, CSS
  sheets, layout, text, events, the `gui` script surface), with satellites for
  the editor's widgets, docking, the code editor and a game's screens
  (`GUI.Gamekit`). The editor AND in-game UI use it. See `Systems/gui-port.md`.
  The older `foundation.ui` is deprecated, in `Code/Deprecated/UI` with its
  UISandbox (`Systems/ui-framework.md`).

Drill-down: every subsystem has a doc in `Documentation/Systems/` (35 and
counting) - `renderer.md`, `scripting.md`, `physics.md`, `audio.md`,
`gui-port.md`, `networking.md`, `web-platform.md`, `editor.md`, ...

## Building

Requirements and per-platform package lists are in the root `README.md`. The
short version: CMake 3.28+, Ninja, Vulkan SDK, and Clang 21 (Linux also keeps
GCC 15+ green; Windows builds with Clang or MSVC, through the `msvc` presets,
which CI builds and tests).

Everything goes through CMake presets:

```bash
cmake --preset clang              # configure (once); also: gcc, clang-reldbg,
                                  # clang-release, *-shipping, wasm
cmake --build --preset clang      # build
ctest --preset clang              # run the unit-test suites
```

Outputs land in `Bin/<Config>/<Platform>-<Compiler>/`, e.g.
`Bin/Debug/Linux64-Clang/`. Practical notes:

- **Debug is the development configuration.** Changes must build green on BOTH
  clang and gcc on Linux before they are done.
- Each library has a doctest sibling (`UI.Tests`, `Engine.Render.Tests`, ...)
  you can build and run individually - much faster iteration than the full
  suite. New test targets appear after a re-configure.
- The web build (`--preset wasm`) needs the emsdk environment sourced first and
  the project's emsdk patch applied; see `Guides/emscripten-windows.md` and the
  README's Web section. New shaders must translate to WGSL (naga) to be
  web-legal.
- Sanitizer trees: configure with `-DBUILDSYSTEM_OUTPUT_SUFFIX=-ASAN` so output
  lands in `...-ASAN/` and never clobbers the regular `Bin/` tree.

## Running things

```bash
# The editor - starts on the project-manager screen; create a project there.
Bin/Debug/Linux64-Clang/Tools.Editor
Bin/Debug/Linux64-Clang/Tools.Editor --project <dir>   # open directly

# The player runs a project directly.
Bin/Debug/Linux64-Clang/Engine.Player <projectDir> [--scene <source-db-path>]

# The renderer comparison scene, desktop Vulkan vs WebGPU:
Bin/Debug/Linux64-Clang/WebScene --vulkan
Bin/Debug/Linux64-Clang/WebScene --webgpu

# UI framework sandbox (every control, all themes):
Bin/Debug/Linux64-Clang/UISandbox
```

A good first session: build, run `Tools.Editor`, create a project, drop some
assets in, press Play. Then run `WebScene --vulkan` to see the renderer
exercised end to end.

## Working on the code

Read `Documentation/Process/CONVENTIONS.md` before your first change - it is
the binding rule set and reviews enforce it. The headlines:

- **Every addition lands with tests** (doctest, in the module's `.Tests`
  target). GPU-visible behavior prefers semantic pixel probes on real devices
  over image goldens.
- **Both compilers green** (clang + gcc, Debug) before a change is done.
  The GCC that works is Ubuntu's packaged GCC 15 (`g++-15` on 24.04+ backports /
  26.04 LTS): the upstream 15.2.0 release cannot resolve transitive module CMIs
  from CMake's module maps ("Bad import dependency"), and the toolchain-r noble
  backport miscompiles module interfaces (defaulted constructors diagnosed as
  overloading themselves). Clang is the portable lane; CI's gcc job runs in an
  `ubuntu:26.04` container for exactly this reason.
- **Style**: Allman braces, per-module `.clang-format`, PascalCase methods,
  full descriptive names, no em/en-dashes anywhere (ASCII hyphen only).
- **Module hygiene**: heavy third-party headers and reflection bodies go in
  implementation units, never module interfaces (GCC build-time blowup).
- **Docs move with behavior**: if you change how a system works, update its
  `Systems/` doc in the same commit.
- Recurring architecture rules (wire-format symmetry, cache invalidation by
  generation not pointer, UI mutation deferral, inspector dispatch entries for
  new `Ref<T>` fields, ...) are all listed in CONVENTIONS.md - skim them once
  so the review doesn't have to teach them.

## Documentation map

```
Documentation/
  GETTING-STARTED.md   This file.
  Systems/             THE reference: one doc per shipped subsystem.
  Specs/               Feature specs - what is being built and why; specs are
                       stamped BUILT when they ship and carry review rulings.
  Process/             CONVENTIONS.md (binding rules incl. code style),
                       HANDOFF.md (review pass records).
  Guides/              How-tos (terrain authoring, adding a script facade, Emscripten setup).
  Plans/               Roadmaps and design plans (roadmap.md, weekly plans).
  UAT/                 Manual smoke-test checklists.
  Archive/             Historical/superseded documents.
```

Suggested first reads after this file: `Process/CONVENTIONS.md`,
`Systems/editor.md`, `Systems/renderer.md`, and then whichever `Systems/` doc
covers the area you want to work in. `KNOWN_ISSUES.md` at the root tells you
what is already known to be broken, so you do not re-diagnose it.
