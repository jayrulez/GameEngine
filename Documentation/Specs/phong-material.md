# The Phong material - a built-in for models authored with Phong, made by the import

> Status: phase 1 DONE 2026-10-27 (the built-in); proposed 2026-10-27 (user: "it makes sense to have a builtin phong shader, and
> investigate how our import pipeline could detect and create the right material"; "like how we
> have unlit"). Not built.
> Mirror: Sedulous ports each phase after it lands (told 2026-10-27).
> Read CONVENTIONS.md first.

## Why

A model authored with Phong comes in as a PBR material today. The FBX loader
(`Foundation/Model.FBX/FbxLoader.cppm`, the "Legacy FBX (Lambert/Phong)" branch) keeps the diffuse
colour, its texture, the normal map and the emission, and sets metallic 0 and roughness 0.8; the
specular colour, its texture and the shininess are never read. Under the PBR lighting such a model
looks other than it was made to: the JuviGods creature 300024 (Ed1, `Creatures/300024`), whose
material ufbx reads as `FBX_PHONG` (shading model "phong", written by the FBX SDK 2020.0.1): its
diffuse a texture (`300024.png`), specular 0.2 grey, shininess (exponent) 20, ambient 0.2 grey, no
emission. A classic OBJ `.mtl` (Kd, Ks, Ns, no PBR keys) takes the same branch and loses the same.

Nothing else in the engine can stand in: a surface shader only feeds PBR inputs to the fixed
lighting (`Data/Shaders/surface/forward.hlsli`, `lighting.hlsli`'s `EvaluateLight`), and a
low-level custom shader, which can own its lighting, is the project's to write, not the import's
to make.

## What is there to build on

- **Unlit, the built-in this one follows.** Its creator (`Pipeline/Materials.Pipeline/
  MaterialsCreatorsImpl.cpp`, "Unlit Material" beside "PBR Material"), its property table in C++
  (`Foundation/Materials/MaterialBuilder.cppm`, `CreateUnlit`: shader `unlit`, `BaseColor`,
  `AlbedoMap`, `MainSampler`), its HLSL in `Data/Shaders/unlit.*.hlsl` (the forward vertex path; its
  cbuffer written by hand to match the table), precompiled into the shader pack. The places that
  know a built-in by name: `BuiltinMaterialTemplate` (MaterialBuilder), `SetMaterialBuiltInShader`
  (`Materials.Pipeline/MaterialAsset.cppm`, a bool), the MCP material tool
  (`Editor.Mcp/MaterialShader.cppm`, "pbr" or "unlit") and the material page's built-in row
  (`Editor.Materials/MaterialPageImpl.cpp`).
- **The lights for a shader that shades them its own way.** `lighting.hlsli`'s `LightsAt`,
  `LightAt` and `ShadowOf` walk the lights that reach a pixel (its cluster's, or all) with their
  shadows; the Shader Gallery's Custom Lighting and Toon shaders shade with them.
- **What the loaders see.** ufbx gives each material its shading model (`shader_type`:
  `FBX_LAMBERT`, `FBX_PHONG`, `WAVEFRONT_MTL`, and `BLENDER_PHONG` for Blender's exports, which
  label every material Phong whatever it is) and its Phong values (`fbx.specular_color`,
  `specular_factor`, `specular_exponent`, each with a texture). cgltf parses
  `KHR_materials_unlit` (`material->unlit`), which nothing reads: an unlit glTF material comes in
  as PBR.
- **One path from the loaders to the material assets.** Every loader fills the model IR
  (`Foundation/Model/ModelMaterial.cppm`, metal-rough PBR only); the importer's material cook reads
  it and makes the assets, in two copies of the same code (`ModelImporter/FileImport.cppm`
  `ImportMaterials`, `ModelImporter/Cook.cppm` `CookMaterials`), each always `CreatePBR` and the
  shader `forward`.

## The design

### The material (phase 1)

A built-in named `phong`, as Unlit is: its HLSL `Data/Shaders/phong.vs.hlsl` (the forward vertex
path) and `phong.ps.hlsl`, its table `CreatePhong` in MaterialBuilder, a creator "Phong Material"
beside the other two.

- **Its lighting.** Each light that reaches the pixel (`LightsAt`, `LightAt`), shadowed
  (`ShadowOf`), shaded Blinn-Phong: the diffuse (the colour times its map) by N.L, plus the
  specular colour (times its map) by (N.H) to the shininess, normalised so a high exponent does not
  brighten the whole highlight. The ambient: the scene's diffuse ambient (its SH irradiance where
  there is one, else its flat ambient) times the diffuse; no PBR reflection. The emission added.
  The variants Unlit and forward have (ALPHA_TEST, GBUFFER, DITHER; SKINNED, INSTANCED and WIND
  through the vertex path), and under GBUFFER the five targets every forward shader writes, so SSR,
  SSGI and the rest go on working (written as a rough dielectric: Phong has no metal, and its
  highlight is not a mirror).
- **Its properties**, in this order (the cbuffer written to match, 64 bytes): `DiffuseColor`
  (colour), `SpecularColor` (colour; black, no highlight), `EmissiveColor` (HDR colour),
  `Shininess` (float, the highlight's exponent), `NormalScale`, `AlphaCutoff`; `DiffuseMap`,
  `SpecularMap`, `NormalMap`, `EmissiveMap`; `MainSampler`. No wind properties, as Unlit has none:
  a Phong material never selects the swaying vertex variant.
- **Where it is known by name.** `BuiltinMaterialTemplate` gains its branch; the built-in shader a
  material asset names becomes one of three (PBR, Unlit, Phong) in `SetMaterialBuiltInShader`, the
  MCP material tool ("phong") and the material page's built-in row. Documents in step: the MCP guide
  for the tool, the material docs.

### The import makes it (phase 2)

The information travels the one path there is:

1. **The loaders say what the file says.** The model IR's material gains a shading model (PBR,
   Phong, Lambert, Unlit; PBR when nothing sets it, so every other loader behaves as today) and the
   Phong values (specular colour, its texture, shininess). The FBX loader sets Phong for
   `FBX_PHONG` and `WAVEFRONT_MTL` without PBR keys, Lambert for `FBX_LAMBERT`, each with its
   values; `BLENDER_PHONG` and any material with PBR data stay PBR, as now. The glTF loader sets
   Unlit for `KHR_materials_unlit` (phase 3).
2. **The cook picks the built-in.** The two copies become one function in ModelImporter, which
   makes each material by its shading model: PBR through `CreatePBR` with the maps it wires today,
   Phong and Lambert (no specular) through `CreatePhong` with the diffuse, specular, normal and
   emissive maps, Unlit through `CreateUnlit`.
3. **What is imported already** keeps its PBR materials until it is imported again.

### glTF unlit (phase 3)

A glTF material with `KHR_materials_unlit` becomes the Unlit built-in (its base colour and texture,
its alpha mode). glTF 2.0 has no Phong: its spec-gloss materials stay PBR (the backlog's
"Spec-gloss and legacy FBX materials keep their specular" is their item).

## Rulings

1. **A material is imported as authored** (user, 2026-10-27): no import option keeps the old
   result. A model authored with Phong comes in as Phong for every format the importer supports; a
   project that wants PBR makes a PBR material after the import and assigns it.
2. **No importer case for one asset's convention** (user, 2026-10-27). Bistro's FBX binds its
   packed occlusion, roughness and metalness maps to Phong's specular slot (its README says so; the
   file cannot): imported as authored, those maps would be read as specular. The importer does not
   special-case it; Bistro's data is fixed outside the engine instead (a script that rebuilds its
   materials as PBR with the packed maps wired as such, for example in Blender, exporting a file
   that imports normally). The backlog item's import option to read that slot as packed ORM is
   dropped.

## Phases

Each a commit with its tests, built on clang and gcc; the user judges the look.

0. This spec.
1. The Phong built-in: the shader, its table, its creator, the three-way built-in in the material
   asset, the MCP tool and the material page; tests of the table and the template, and the shader
   pack cooking it (a variant flag it uses undeclared fails the pack).
2. The import: the IR's shading model and Phong values, the FBX loader (FBX Phong and Lambert, OBJ
   `.mtl`), the one material cook; tests from small text fixtures (an OBJ with its `.mtl`, an ASCII
   FBX with a Phong and a Lambert material), and one with a Blender export staying PBR.
3. glTF unlit: the loader and the cook; a test from a small glTF.
4. Ed1's creature imported again, for the user to look at.
