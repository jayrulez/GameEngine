# Shader Gallery - the custom shaders sample, a lesson per shader

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: phases 0 to 2 BUILT 2026-10-12 (the hall, the camera, the list, all eleven lessons);
> phase 3 (polish and docs) next. The outline was agreed with the user (eleven shaders, a
> flythrough with a user-controlled camera and a list that flies to each). Replaces the custom shader showcase that
> was to go in the fifth sample (`Archive/custom-shaders-history.md`, phase 7); the fifth sample stays a
> networking and crowds game. Read CONVENTIONS.md first.

## What it is for

A new user learning to write shaders for the engine opens this project and walks a gallery: each
shader stands on its own plinth, and its `.hlsl` is a short, commented lesson that builds on the
one before. Both kinds are here, surface shaders first (the kind to start with) and the low-level
kind after. Read in order, the files are a course; opened alone, each is a working example to copy.

## Where it lives and how it is built

- **Project**: `Data/SampleProjects/ShaderGallery` (its `Cooked/`, `.cache/`, `Editor/` and
  `Dist/` generated and ignored, as the other samples'). Authoring scripts in its `Tools/`, as
  Snowline's and Lamplight's: an MCP client and the scene generator.
- **Through the tools, not by hand.** Shaders by `asset_create` (the Lit/Unlit Surface Shader
  and Low-Level Shader creators) with their files written and checked by `shader_validate`,
  materials by `asset_data_write`, the hall by `scene_write`, the UI and its script by the UI and
  script tools, playtests by `pie_run`. A tool that is missing or wrong is a finding: fix it in
  the engine with a test, on its own commit, then carry on.
- **Integration.Mcp** checks it reads at the current data versions and cooks, as the other
  samples.

## The gallery

One hall, the shaders in a row of plinths in teaching order, each with its object (a sphere,
a rock, a flag, a statue, whatever shows the lesson best) and a sign with its number and name.
Lit by a sun through high windows and a few point lights, so the lit shaders have something to
answer to; a reflection probe for the metal and the water.

| # | Shader | Kind | What it teaches |
|---|---|---|---|
| 1 | Unlit Colour | unlit surface | The smallest shader: a properties block and `Surface()` |
| 2 | Textured | lit surface | Sampling a texture; uv; the vertex colour |
| 3 | Lava | lit surface | `Time()`, scrolling, emission (and bloom) |
| 4 | Hologram | unlit surface | `viewDir` and a fresnel rim; `screenPos` scanlines; a blended material |
| 5 | Triplanar Rock | lit surface | Mapping by `worldPos`, no uv; a normal from a texture |
| 6 | Dissolve | lit surface | `alpha` on a Masked material, the shadow cut too; a glowing edge |
| 7 | Waves | lit surface | `Vertex()`: displacement, the shadow following |
| 8 | Wind | lit surface | `Vertex()` with `localPosition`: roots still, tips swaying (a flag) |
| 9 | Built-in PBR | lit surface | The engine's own material, written as a surface: the reference |
| 10 | Toon | low-level | Lighting of its own: bands, a shadow colour, a rim (the toon) |
| 11 | Custom Lighting | low-level | Walking the lights itself (`LightsAt`, `LightAt`, `ShadowOf`): half-Lambert |

Each file opens with what it teaches and which lesson it builds on, and comments only what is
new in it. Materials expose the lesson's parameters with sensible ranges, so the material page
is part of the lesson (drag a slider, see it change).

## Playing it

- **A free camera**: WASD (or a stick) to fly, the mouse with the right button (or the other
  stick) to look, Shift to go faster.
- **The list**: a panel down one side, a row per shader (number, name, kind). Clicking a row flies
  the camera to that plinth's viewpoint, a smooth move over about a second, and shows the lesson's
  one-line summary and its file name in a caption at the foot of the view. Flying the camera by hand again leaves the
  list alone; the next click flies from wherever it is.
- A key hides the panel (screenshots, a clean look).

## Assets

Textures for the textured, triplanar and dissolve lessons (a ground and rock set, a noise for the
dissolve) from CC0 sources (Poly Haven, ambientCG), each with its licence text in `Licenses/` and
named in `CREDITS.md`, as Sky Hopper keeps them. Lessons that can be procedural are (lava, waves,
hologram), so the shader shows everything. The hall from the engine's primitives, and a Blender
model where a lesson asks for one (the flag's cloth, a statue for the toon), on the shared kit.

## Engine gaps to check in P0

- A script flying the camera to a point and orientation over time, and reading free-look input
  (mouse delta under a held button) beside it: what Sky Hopper's and Snowline's cameras have.
- A UI list built from data (a row per shader) with a click handler in the startup script.
- A blended (transparent) surface material: the hologram is the first in a sample.
Each gap is fixed in the engine with a test, then the gallery carries on.

## Phases

0. **The hall and the first three.** The project, the hall, the camera and the list, with
   lessons 1 to 3; runs in the editor, on the desktop and in the browser.
1. **The surface lessons**, 4 to 9, with their textures, credits and licences.
2. **The low-level lessons**, 10 and 11.
3. **Polish and docs**: the lessons' summaries in the UI, a README with pictures, the docs site's
   Custom shaders page pointing at each lesson's file, Integration.Mcp's check, the demos site
   and the Deck.
