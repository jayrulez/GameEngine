# WebScene: the renderer comparison scene

`Code/Samples/WebScene` is the full-renderer exercise scene, built for BOTH desktop and the
browser so the backends can be compared side by side: analytic sky -> IBL, CSM sun + shadowed
spot + orbiting point light, PBR sphere grid, SSR floor, reflection probe + chrome sphere,
instanced ring, decal, sprites, particles, debug draw, and an ImGui tweak panel.

## Desktop

```bash
cmake --build --preset clang --target WebScene Tools.ShaderPack

Bin/Debug/Linux64-Clang/WebScene --vulkan     # Vulkan reference
Bin/Debug/Linux64-Clang/WebScene --webgpu     # WebGPU via wgpu-native (SPIR-V ingestion)
```

To run the EXACT browser shaders (cooked WGSL) on desktop - the fast local repro for
web-render bugs - cook a WGSL pack beside the exe and force the WGSL path:

```bash
# Linux
Bin/Debug/Linux64-Clang/Tools.ShaderPack Data/Engine/Shaders Bin/Debug/Linux64-Clang/shaders.dpak wgsl spirv
OPTION_USE_SHADER_PACK=1 ENV_WEBGPU_WGSL=1 Bin/Debug/Linux64-Clang/WebScene --webgpu
```

```powershell
# Windows (PowerShell env syntax - `set X=1` is cmd-only and silently does nothing here)
Bin\Debug\Win64-Clang\Tools.ShaderPack.exe Data\Engine\Shaders Bin\Debug\Win64-Clang\shaders.dpak wgsl spirv
$env:OPTION_USE_SHADER_PACK="1"
$env:ENV_WEBGPU_WGSL="1"
Bin\Debug\Win64-Clang\WebScene.exe --webgpu
```

On `--webgpu` the backend logs every GPU adapter at startup and prefers a discrete GPU; on
multi-adapter machines where the pick is wrong, override it with
`ENV_WEBGPU_ADAPTER=<index from the logged list>`.

## Browser

```bash
cmake --build --preset wasm --target WebScene
cd Bin/Debug/Emscripten-Clang && python3 -m http.server 8080
# open http://localhost:8080/WebScene.html
```

If the plain server causes MIME/caching trouble, `Code/Engine/Engine.Player/serve.py`
serves a folder with the correct wasm MIME and no-store headers.

