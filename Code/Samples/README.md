# Samples

Low-level C++ samples: each one exercises a part of the engine directly, without a project or
the editor. Games made the usual way, as projects in the editor, are under
`Data/SampleProjects/`. Every sample builds with the rest of the tree and lands in
`Bin/<Config>/<Platform>-<Compiler>/`.

## Engine

| Sample | What it shows |
|---|---|
| `HelloWindow` | The minimal app: a window and the frame loop, from `APP_MAIN` through the runtime host. |
| `MultiWindow` | The runtime host with a shared graphics device: a second window opened at run time, each with its own render loop. |
| `Sandbox` | The development harness on `DefaultApplication`: a scene of spinning cube grids (instanced and distinct), with a debug panel for the renderer's settings. |
| `RenderStressTest` | A worst-case renderer benchmark: a grid of spheres the camera sees all at once, grown 8000 at a time. |
| `AnimStressTest` | A skinned-animation benchmark: one cooked character replicated across a grid, each with its own animation player. |
| `AnimatedCrowd` | An animated crowd: characters sharing a few pose palettes, with the pose chosen per instance by policy. |
| `ParticleFX` | Sixteen particle effects built in code, one per cell of a grid: a fountain, mesh shards (opaque and glowing), embers that each carry a point light, soft-particle haze, trail ribbons, rain that bounces off a sphere and the ground, a local-space puff, smoke, fire, a campfire, fireworks, a tornado, a flipbook explosion, a magic circle and fireflies. |
| `PhysicsPlayground` | Physics as components: falling crates, a kinematic sweeper, a trigger volume and raycast shoves. |
| `AudioPlayground` | Audio as components: an ambient loop and four 3D emitters, heard through a listener on the camera. |
| `InputActions` | Named input actions on keys, pads and touch, an exclusive menu set, smoothing, time scale and rebinding. |
| `ScriptPlayground` | Script behaviours on entities, in AngelScript or Luau (`--script=<lang>`), from the same scene. |
| `TerrainPlayground` | Terrain and vegetation from an in-memory heightfield, on the desktop and in the browser. |
| `GameUiSandbox` | The game UI kit's widgets on a screen of the UI subsystem, driven by keys or a pad. |
| `NetEcho` | The network stack end to end in a console: a reliable-UDP client and server echoing lines over localhost. |

## Web

| Sample | What it shows |
|---|---|
| `WebTriangle` | The first app that ran in a browser: the runtime host on a web shell, drawing through WebGPU. |
| `WebScene` | One scene exercising the whole renderer, on Vulkan, desktop WebGPU and the browser, for comparing backends side by side ([Documentation/Guides/webscene.md](../../Documentation/Guides/webscene.md)). From before the editor could export a web build, when it was the way to test the web. |

Both desktop entries take `--vulkan` or `--webgpu`; `OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1`
runs the cooked WGSL shaders the browser uses, on desktop WebGPU.

## UI and vector graphics

| Sample | What it shows |
|---|---|
| `VG/VGSandbox` | The vector graphics stack (paths, strokes, gradients, SVG, text) in a NanoVG-style demo. |
| `UI/UISandbox` | The UI framework: a themed view tree laid out and drawn through the vector graphics renderer. |

The experimental retained-mode UI framework has its sandbox beside it, in
`Code/Experimental/GUI.Sandbox`.

## RHI

`RHI/Sample001_Triangle` to `RHI/Sample030_RenderBundles` each exercise one feature of the
rendering hardware interface on its own (textures, compute, MSAA, MRT, queries, bindless, mesh
shaders, ray tracing, render bundles and more). They take `--vulkan` or `--webgpu` (`--dx12` on
Windows). `RHI/Smoketest` is a tour of the low-level API against the Vulkan backend directly, with
no framework, for debugging the RHI itself. `Framework/` is the small app framework the numbered
samples share, and `Common/` the fly camera several samples use.
