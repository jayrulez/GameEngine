<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="Data/Branding/assiduous-logo.svg">
    <img alt="AssiduousEngine" src="Data/Branding/assiduous-logo-light.svg" height="72">
  </picture>
</h1>

[![Discord](https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white)](https://discord.gg/WSvxW8mWH5)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A modern C++23 game engine and editor built entirely with C++ modules. It provides a complete game
development ecosystem, combining an integrated editor, asset pipeline, runtime, and toolchain with
advanced rendering, gameplay scripting, and a comprehensive UI framework. Its agentic workflow
enables AI agents to build entire games, from initial project setup to fully playable experiences.

![The editor](Documentation/Images/Editor.png)

## Features

- **C++23 modules throughout:** every library is a named module, with no public headers.
- **Rendering hardware interface (RHI)** with Vulkan 1.3 (primary), WebGPU (desktop through
  wgpu-native, and the browser) and Direct3D 12 backends.
- **Renderer:** a render graph driving forward PBR with a depth prepass and clustered lights;
  cascaded sun shadows and shadowed point and spot lights; image-based lighting and reflection
  probes; decals, sprites, particles, skinned and instanced meshes; render textures for in-game
  cameras.
- **Post-processing:** ambient occlusion, SSR, screen-space GI, bloom, TAA, MSAA, FXAA, auto
  exposure and colour grading, set once per game in shared environment and post profiles.
- **Shaders:** HLSL compiled through DXC, cross-compiled to WGSL for the web, cooked into packs,
  and custom shaders per project.
- **Scene system:** entities with hierarchical transforms, component managers per domain,
  prefabs with overrides, and scene serialization.
- **Gameplay systems:** physics (Jolt), navigation (Recast/Detour), audio (miniaudio, buses
  with effect chains), input maps, save data, splines, terrain and vegetation.
- **Animation:** skeletal clips, animation graphs, IK, root motion and property animation.
- **Networking** with state replication.
- **Scripting** in AngelScript or Luau over the engine's reflected facades: behaviours per entity,
  a level script per scene and a game script per run, with coroutines, events and an in-editor
  debugger. See [Documentation/Shipping/Scripting.md](Documentation/Shipping/Scripting.md).
- **UI:** a retained-mode GUI with flex, grid and stack layouts, `.xml` markup, CSS sheets
  (cascade, variables, transitions, animations, media queries), shaped bidirectional text,
  keyboard and gamepad navigation, data binding, world-space UI, and a game kit for screens and
  prompts, every widget reachable from scripts.
- **Vector graphics and fonts:** a vector graphics layer with SVG, and distance field and
  coverage fonts.
- **Editor** (`Tools.Editor`): project manager, scene hierarchy, viewport with gizmos and
  per-domain tools (terrain sculpting and painting, vegetation, splines), inspectors from
  reflection, an asset browser, undo and redo, play in editor (several instances side by side),
  and a page per asset type.
- **Asset pipeline:** importers for glTF, FBX, OBJ, images, audio and fonts; cooks per asset
  type; BC7 and ASTC texture compression.
- **Export** to the desktop player, the web (WebAssembly with WebGPU) and the Steam Deck, from
  per-target presets and player templates; headless tools do all of it from the command line.
- **Agent tooling:** an MCP host (`Tools.Mcp`) exposes the engine's reflection, the script API
  and project operations (import, cook, scene validation, export, health checks). The editor
  serves the same tools over HTTP for its open project, plus its pages and play in editor, where
  an agent plays the game with scripted input and reads back probes and screenshots.
  [AGENTS.md](AGENTS.md) is how an agent works on the engine itself.
- **Samples:** five sample games and a custom shaders gallery (below), and low-level C++ samples
  per engine area.

## Building

Requirements:

- CMake 3.28+ and Ninja.
- Linux x64: Clang 21 and/or GCC 15+ (both are kept green; Clang is the daily driver), the
  Vulkan development libraries, and SDL3's build dependencies. On Ubuntu or Debian:

  ```
  sudo apt install cmake ninja-build clang pkg-config \
      libvulkan-dev vulkan-tools vulkan-validationlayers mesa-vulkan-drivers \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
      libxtst-dev libwayland-dev wayland-protocols libxkbcommon-dev libasound2-dev
  ```

  Clang 21 is what development and CI use. Ubuntu 24.04's default Clang 18 does not work (it
  rejects flags the build passes under `-Werror`); 19 and 20 are untested. `build-clang.sh` picks
  the newest Clang installed.

- Windows x64: Clang 21 (LLVM) or MSVC, and the Windows SDK. The MSVC presets run from a
  Developer Command Prompt (vcvars64), so `cl.exe` and the SDK are on the path.
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
| `clang` / `gcc` / `msvc` | Debug, the development configuration |
| `clang-shared` / `msvc-shared` | Debug with shared libraries |
| `clang-reldbg` / `msvc-reldbg` | RelWithDebInfo, for performance work and the editor download |
| `clang-release` / `gcc-release` / `msvc-release` | Release |
| `clang-shipping` / `gcc-shipping` / `msvc-shipping` | Shipping (no asserts, stripped) |
| `wasm` / `wasm-release` / `wasm-shipping` | Emscripten wasm32: Debug, Release with the profiler, Shipping |

Running the editor:

```
Bin/Debug/Linux64-Clang/Tools.Editor               # the project manager
Bin/Debug/Linux64-Clang/Tools.Editor <projectDir>  # open a project
```

Add `--mcp` to serve the open project to an agent over MCP while you work in the editor.

## Sample projects

Game projects under `Data/SampleProjects/`, opened from the editor's project manager. The five
games and the Shader Gallery were built through the engine's MCP tools.
[The website](https://assiduousengine.github.io/#games) plays them in a browser and has their
Windows, Linux and Steam Deck downloads (the Shader Gallery has no Steam Deck build).

**Sky Hopper** (`PlatformerGame`) is a small 3D platformer, played through to the end over the
MCP tools by an AI agent while it was built: five levels, coins, enemies and hazards, three lives
with stars and best scores saved, menus with volume settings, music and effects, and gamepad
support throughout.

| Title | Level 1 | Settings |
|:---:|:---:|:---:|
| ![Title](Documentation/Images/SkyHopper-Title.png) | ![Playing](Documentation/Images/SkyHopper-Play.png) | ![Settings](Documentation/Images/SkyHopper-Settings.png) |

**PaperKid** is an arcade paper-route game: five town blocks on a difficulty ramp, papers thrown
with a soft auto-aim, traffic and pedestrians on the navmesh, lives, a live minimap drawn by a
top-down camera into a render texture, particle effects, a newsprint UI theme, and music that
speeds up when the clock runs low. Its kid, bike, town and people are modelled, rigged and
animated by Blender scripts.

| Title | Riding a block | Block cleared |
|:---:|:---:|:---:|
| ![Title](Documentation/Images/PaperKid-Title.png) | ![Playing](Documentation/Images/PaperKid-Play.png) | ![Cleared](Documentation/Images/PaperKid-Cleared.png) |

Watch it played on a Steam Deck: [PaperKid gameplay video](https://youtu.be/syJlmIirg_o).

**Snowline** is a snowboard time trial with tricks. It shows generated terrain with splat-painted
snow, rock and forest, scattered vegetation whose trees are solid, splines for the course line and
the medal ghosts, an animation graph the rider drives by parameters, decals for its tracks, and
jointed slalom flags. Three courses opened by medals: Meadow, Forest with its shortcut through the
trees, and Ridge with a run of kickers, a gap over a crevasse and an avalanche chasing down the last
stretch. Spins and grabs score on a combo, and your best run rides beside you as a ghost. The rider
is modelled, rigged and animated by Blender scripts.

| Courses | Ridge, past the gap | A run's results |
|:---:|:---:|:---:|
| ![Courses](Documentation/Images/Snowline-Title.png) | ![Ridge](Documentation/Images/Snowline-Ridge.png) | ![Results](Documentation/Images/Snowline-Results.png) |

Watch it played on a Steam Deck: [Snowline gameplay video](https://youtu.be/vlxxLiMEzp0).

**Lamplight** is a night-time manor heist in pure stealth. It shows lamplight and lantern shadows, a
light meter that reads the renderer's own lights (a guard's eye reads the same), guards on
navigation rounds who see by their lantern's cone, hear footsteps on each surface and come to a
thrown pebble, doors and gates to pick, walls that dither away between the camera and the thief,
rain, reverb per room, and auto exposure from moonlit grounds into torch-lit cellars. Five levels,
from the gardens to the vault on the upper floor, with checkpoints, loot, an alarm, and a ghost
medal for never being seen. Everything in it, the thief and the guards included, is built by
scripts: Blender for the models, a level generator for the scenes, generated sounds.

| The Gardens | The Ground Floor | Away like a ghost |
|:---:|:---:|:---:|
| ![Gardens](Documentation/Images/Lamplight-Gardens.png) | ![Ground Floor](Documentation/Images/Lamplight-GroundFloor.png) | ![Results](Documentation/Images/Lamplight-Results.png) |

**Masquerade** is social stealth at a masked ball, alone against bots or with up to four players
over the network, chosen to show the network stack and crowds: an authoritative host that plays
and players joining it natively, replicated script state, and some 150 guests on navigation drawn
as instances of six costumes. Each hunter is given a mark, a guest in one of those costumes, and is
someone else's mark; guests stroll, gather and dance to the orchestra's cues, and a hunter who
sprints or stands still through a dance stands out. Three maps: the Palace, the Canal with its
gondolas, and the Gardens. Masks and silk on custom shaders, music the crowd follows, and the
models, the music and the sounds all made by its scripts. More in
[its README](Data/SampleProjects/Masquerade/README.md).

| The title | On the Canal | The music stops |
|:---:|:---:|:---:|
| ![The title](Documentation/Images/Masquerade-Title.png) | ![On the Canal](Documentation/Images/Masquerade-Canal.png) | ![Results](Documentation/Images/Masquerade-Results.png) |

**Shader Gallery** (`ShaderGallery`) is the custom shaders sample: a hall of lesson shaders to
fly through, each on its own plinth, each `.hlsl` a short commented lesson that builds on the one
before. Surface shaders first (unlit colour, textures, lava, a hologram, triplanar rock, dissolve,
waves, wind and the engine's own PBR written as a surface), then two low-level shaders that light
themselves: toon bands, and lighting that walks the scene's lights. Its lessons are listed in
[its README](Data/SampleProjects/ShaderGallery/README.md).

| The hall | Hologram | Dissolve | Toon |
|:---:|:---:|:---:|:---:|
| ![The hall](Documentation/Images/ShaderGallery-Hall.png) | ![Hologram](Documentation/Images/ShaderGallery-Hologram.png) | ![Dissolve](Documentation/Images/ShaderGallery-Dissolve.png) | ![Toon](Documentation/Images/ShaderGallery-Toon.png) |

**NativeSample** is the reference for a game with native C++ code beside its scripts.

## Repository layout

```
Code/
  Foundation/     Core, RHI and backends, Shell, Resource, VFS, Scene, Render, GUI, VG,
                  Fonts, Audio, Physics, Navigation, Net, Script, Shaders, Mcp, ...
  Engine/         The subsystems over Foundation, the composition root, the default
                  application, the game instance, the player
  Pipeline/       Importers and cooks per asset type, the per-language script cooks
  Editor/         Editor core, the project half, the app and one module per domain;
                  the MCP tools
  Tools/          Editor, Cook, Export, ShaderPack, Mcp and ModelViewer executables
  Integration/    Flows that cross collections
  Samples/        Low-level C++ samples, one per engine area, and the RHI samples
                  (Code/Samples/README.md)
  Extensions/     Dear ImGui as a debug UI extension
  Deprecated/     The old UI framework and its sandbox, built only on request
Data/             The engine's shaders and fonts, the editor's and the model viewer's interface,
                  branding, project templates, the code samples' and tests' data, and the
                  sample projects
ThirdParty/       Vendored dependencies
scripts/          Distribution, export template and Steam Deck builds
Documentation/    Shipping documentation (served to agents through the MCP host), systems,
                  guides and plans
```

Each Foundation and Engine module has a sibling `.Tests` target; `Code/Integration/` holds the
flows that cross collections.

## Platform support

Linux and Windows are the development platforms. WebGPU runs on the desktop through wgpu-native
and in the browser through a wasm build, and the export produces a web player. A Steam Deck player
builds in a container (`scripts/build-steamdeck.sh`, glibc 2.35, below SteamOS) and installs as an
export template, so the export packages a game for the Deck. macOS has no backend yet.

## Dependencies

Vendored under `ThirdParty/`: SDL3 (its source, built statically into the executables; set
`ENGINE_SYSTEM_SDL` to use a system SDL3 instead), wgpu-native,
DXC, naga and tint, JoltPhysics, recastnavigation, miniaudio, AngelScript and Luau, cgltf and
ufbx, meshoptimizer, msdfgen, HarfBuzz and SheenBidi, stb, astcenc, bc7enc and bcdec, Mbed TLS, Dear
ImGui, and doctest. Each keeps its licence beside it, and the build gathers those that ship in a
binary into `THIRD-PARTY-NOTICES.txt` beside every player, editor and export. The Vulkan SDK is a
system install.

## Community

Join the [Discord](https://discord.gg/WSvxW8mWH5) for discussion and support.

## Inspiration

The engine draws inspiration from [ezEngine](https://github.com/ezEngine/ezEngine),
[LumixEngine](https://github.com/nem0/LumixEngine) and [Traktor](https://github.com/apistol78/traktor).

## License

MIT. See [LICENSE](LICENSE). The AssiduousEngine name and logo are not: they stay the project's
own, under the terms in [Data/Branding/LICENSE.txt](Data/Branding/LICENSE.txt).
