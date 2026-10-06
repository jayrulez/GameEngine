# Game Engine

[![Discord](https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white)](https://discord.gg/WSvxW8mWH5)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A game engine in C++23, built on modules throughout: a layered runtime over an abstract RHI with
Vulkan, WebGPU and Direct3D 12 backends, a scene editor with play in editor, an asset pipeline,
AngelScript and Luau gameplay scripting, and a CSS styled UI framework. It runs on Linux,
Windows and, through Emscripten, in the browser, and ships games to the Steam Deck.

![The editor](Documentation/Images/Editor.png)

## What is here

**Rendering.** A render graph over the RHI: forward PBR with a depth prepass, cascaded and
local shadows, IBL and reflection probes, decals, sprites, particles, skinned meshes, terrain
and vegetation, and a post stack with ambient occlusion, SSR, SSGI (experimental), bloom, TAA,
MSAA, auto exposure, grading and FXAA. Vulkan 1.3 is the primary desktop backend; WebGPU runs on
the desktop through wgpu-native and in the browser as itself; Direct3D 12 runs on Windows.
Shaders are HLSL, compiled through DXC and cross compiled to WGSL for the web, and cooked into
packs.

**Scene and engine.** Entities with hierarchical transforms, component managers per domain,
prefabs with overrides, scene serialization, and the subsystems a game needs: physics (Jolt),
navigation (Recast/Detour), audio (miniaudio, four fixed buses plus custom ones with effect
chains), animation (skeletal, graphs, property animation), particles, splines, terrain, input
maps, networking with state replication, and world space UI.

**Scripting.** Gameplay code in AngelScript or Luau over the engine's reflected script facades:
behaviours per entity, a level script per scene, a game script per run, coroutines and events.
See [Documentation/Shipping/Scripting.md](Documentation/Shipping/Scripting.md).

**Editor.** A project manager, scene hierarchy, a viewport with gizmos and per domain tools
(terrain sculpting and painting, vegetation, spline editing), inspectors from reflection, an
asset browser over the import and cook pipeline, undo and redo, play in editor (several
instances side by side), and a page per asset type: materials, meshes, textures, fonts, audio,
animation clips and graphs, particles, input maps, UI documents and themes, scripts with a
debugger.

**Pipeline.** Importers (glTF, FBX and OBJ, images, audio, fonts), cooks per asset type, texture
compression (BC7, ASTC), an export that packages a project for the desktop player, the web or a
Steam Deck, and headless tools for all of it.

**UI.** A retained view tree with flex, dock, grid and flow layouts, `.sml` markup and `.sss`
stylesheets with a cascade, transitions and themes, keyboard and gamepad navigation, a vector
graphics layer with SVG, distance field and coverage fonts, and an editor toolkit (docking,
property grids, colour pickers, curve and gradient editors, a node graph canvas).

**Agent tooling.** An MCP host (`Tools.Mcp`) exposes the engine's reflection, the script API and
project operations (import, cook, scene validation, export, health checks); the editor serves
the same tools over HTTP for its open project, plus pages and play in editor, so an agent can
build and playtest a game. [AGENTS.md](AGENTS.md) is how an agent works on the engine itself.

## Building

Requirements:

- CMake 3.28+ and Ninja.
- Linux x64: Clang 17+ and/or GCC 15+ (both are kept green; Clang is the daily driver), the
  Vulkan development libraries, and SDL3's build dependencies. On Ubuntu or Debian:

  ```
  sudo apt install cmake ninja-build clang pkg-config \
      libvulkan-dev vulkan-tools vulkan-validationlayers mesa-vulkan-drivers \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
      libxtst-dev libwayland-dev wayland-protocols libxkbcommon-dev libasound2-dev
  ```

- Windows x64: Clang 17+ (LLVM; not MSVC) and the Windows SDK.
- A Vulkan 1.3 (or, on Windows, D3D12) capable GPU to run anything that draws.
- For web builds, emsdk 6.0.5 with the project's response file patch applied; see
  [Documentation/Guides/emscripten-windows.md](Documentation/Guides/emscripten-windows.md).

Everything goes through CMake presets; outputs (executables, cooked shader packs, runtime
sidecars) land in `Bin/<Config>/<Platform>-<Compiler>/`, e.g. `Bin/Debug/Linux64-Clang/`.

```
cmake --preset clang                  # or gcc; clang-release, clang-shipping, wasm, ...
cmake --build --preset clang
ctest --preset clang                  # the unit and integration tests
```

| Preset | Meaning |
|---|---|
| `clang` / `gcc` | Debug, the development configuration |
| `clang-shared` | Debug with shared libraries |
| `clang-reldbg` | RelWithDebInfo, for performance work |
| `clang-release` / `gcc-release` | Release |
| `clang-shipping` / `gcc-shipping` | Shipping (no asserts, stripped) |
| `wasm` / `wasm-shipping` | Emscripten wasm32 |

Running the editor:

```
Bin/Debug/Linux64-Clang/Tools.Editor               # the project manager
Bin/Debug/Linux64-Clang/Tools.Editor <projectDir>  # open a project
```

Add `--mcp` to serve the open project to an agent over MCP while you work in the editor.

## Sample projects

Game projects under `Data/SampleProjects/`, opened from the editor's project manager.

**Sky Hopper** (`PlatformerGame`) is a small 3D platformer built entirely through the engine's
MCP tools by an AI agent, played through to the end over the same tools, and shipped to a Steam
Deck: five levels, coins, enemies and hazards, three lives with stars and best scores saved,
menus with volume settings, music and effects, and gamepad support throughout.

| Title | Level 1 | Settings |
|:---:|:---:|:---:|
| ![Title](Documentation/Images/SkyHopper-Title.png) | ![Playing](Documentation/Images/SkyHopper-Play.png) | ![Settings](Documentation/Images/SkyHopper-Settings.png) |

**PaperKid** is an arcade paper-route game, also built entirely through the MCP tools: five
town blocks on a difficulty ramp, papers thrown with a soft auto-aim, traffic and pedestrians on
the navmesh, lives, a live minimap drawn by a top-down camera into a render texture, particle
effects, a newsprint UI theme, and music that speeds up when the clock runs low. The kid and
his bike, the houses, cars, people and animals are modelled, rigged and animated by Blender
scripts.

| Title | Riding a block | Block cleared |
|:---:|:---:|:---:|
| ![Title](Documentation/Images/PaperKid-Title.png) | ![Playing](Documentation/Images/PaperKid-Play.png) | ![Cleared](Documentation/Images/PaperKid-Cleared.png) |

Watch it played on a Steam Deck: [PaperKid gameplay video](https://youtu.be/syJlmIirg_o).

**Snowline** is a snowboard time trial with tricks, built the same way and chosen for the engine
features the other two do not use: generated terrain with splat-painted snow, rock and forest,
scattered vegetation whose trees are solid, splines for the course line and the medal ghosts, an
animation graph the rider drives by parameters, decals for its tracks, and jointed slalom flags.
Three courses opened by medals: Meadow, Forest with its shortcut through the trees, and Ridge with
a run of kickers, a gap over a crevasse and an avalanche chasing down the last stretch. Spins and
grabs score on a combo, your best run rides beside you as a ghost, and the rider is modelled,
rigged and animated by Blender scripts.

| Courses | Ridge, past the gap | A run's results |
|:---:|:---:|:---:|
| ![Courses](Documentation/Images/Snowline-Title.png) | ![Ridge](Documentation/Images/Snowline-Ridge.png) | ![Results](Documentation/Images/Snowline-Results.png) |

**NativeSample** is the reference for a game with native C++ code beside its scripts.

## Repository layout

```
Code/
  Foundation/     Core, RHI and backends, Shell, Resource, VFS, Scene, Render, UI, VG,
                  Fonts, Audio, Physics, Navigation, Net, Script, Shaders, Mcp, ...
  Engine/         The subsystems over Foundation, the composition root, the default
                  application, the game instance, the player
  Pipeline/       Importers and cooks per asset type, the per-language script cooks
  Editor/         Editor core, the project half, the app and one module per domain;
                  the MCP tools
  Tools/          Editor, Cook, Export, ShaderPack and Mcp executables
  Integration/    Flows that cross collections
  Samples/        Engine samples and the RHI samples
  Extensions/     Dear ImGui as a debug UI extension
Data/             Engine data (shaders, fonts, themes) and the sample projects
ThirdParty/       Vendored dependencies
scripts/          Distribution, export template and Steam Deck builds
Documentation/    Shipping documentation (served to agents through the MCP host), systems,
                  guides and plans
```

Each Foundation and Engine module has a sibling `.Tests` target; `Code/Integration/` holds the
flows that cross collections. The RHI samples exercise one backend feature each and take
`--vulkan` / `--webgpu` (`--dx12` on Windows).

## Platform support

Linux and Windows are the development platforms. WebGPU runs on the desktop through wgpu-native
and in the browser through a wasm build; `WebScene` renders the same scene on every backend for
side by side comparison ([Documentation/Guides/webscene.md](Documentation/Guides/webscene.md)),
and the export produces a web player. A Steam Deck player builds in a container
(`scripts/build-steamdeck.sh`, glibc 2.35, below SteamOS) and installs as an export template, so
the export packages a game for the Deck. macOS has no backend yet.

## Dependencies

Vendored under `ThirdParty/`: SDL3 (prebuilt on Windows, the system's on Linux), wgpu-native,
DXC, naga and tint, JoltPhysics, recastnavigation, miniaudio, AngelScript and Luau, cgltf and
ufbx, meshoptimizer, msdfgen, stb, astcenc, bc7enc and bcdec, Dear ImGui, and doctest. The Vulkan
SDK is a system install.

## Community

Join the [Discord](https://discord.gg/WSvxW8mWH5) for discussion and support.

## Inspiration

The engine draws inspiration from [ezEngine](https://github.com/ezEngine/ezEngine),
[LumixEngine](https://github.com/nem0/LumixEngine) and [Traktor](https://github.com/apistol78/traktor).

## License

MIT. See [LICENSE](LICENSE).
