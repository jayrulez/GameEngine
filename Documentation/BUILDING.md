# Building

## Requirements

- CMake 3.28+ and Ninja.
- Linux x64: Clang 21 and/or GCC 15 (both are kept green; Clang is the daily driver), the Vulkan
  development libraries, and SDL3's build dependencies. On Ubuntu or Debian:

  ```
  sudo apt install cmake ninja-build clang pkg-config \
      libvulkan-dev vulkan-tools vulkan-validationlayers mesa-vulkan-drivers \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
      libxtst-dev libwayland-dev wayland-protocols libxkbcommon-dev libasound2-dev
  ```

  Clang 21 is what development and CI use. Ubuntu 24.04's default Clang 18 does not work (it
  rejects flags the build passes under `-Werror`); 19 and 20 are untested. `build-clang.sh` picks
  the newest Clang installed.

  The GCC that works is Ubuntu's packaged GCC 15 (`g++-15` on 24.04 backports or 26.04 LTS). The
  upstream 15.2.0 release cannot resolve transitive module CMIs from CMake's module maps ("Bad
  import dependency"), and the toolchain-r noble backport miscompiles module interfaces (defaulted
  constructors diagnosed as overloading themselves). CI's gcc job runs in an `ubuntu:26.04`
  container for this reason.

- Windows x64: Clang 21 (LLVM) or MSVC, and the Windows SDK. The MSVC presets run from a Developer
  Command Prompt (vcvars64), so `cl.exe` and the SDK are on the path.
- A Vulkan 1.3 (or, on Windows, D3D12) capable GPU to run anything that draws.
- For web builds, emsdk 6.0.5 with the project's response file patch applied; see
  [Guides/emscripten-windows.md](Guides/emscripten-windows.md).

## Presets

Everything goes through CMake presets. Outputs (executables, cooked shader packs, runtime
sidecars) land in `Bin/<Config>/<Platform>-<Compiler>/`, for example `Bin/Debug/Linux64-Clang/`.

```
cmake --preset clang                  # configure once; or gcc, clang-release, wasm, ...
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

## Notes

- **Debug is the development configuration.** A change must build green on both clang and gcc on
  Linux before it is done.
- Each library has a doctest sibling (`GUI.Tests`, `Engine.Render.Tests`, ...) you can build and
  run on its own, which iterates much faster than the full suite. New test targets appear after
  a re-configure.
- The web build (`--preset wasm`) needs the emsdk environment sourced first and the project's
  emsdk patch applied. New shaders must translate to WGSL (through naga) to run on the web.
- Sanitizer trees: configure with `-DBUILDSYSTEM_OUTPUT_SUFFIX=-ASAN` so output lands in
  `...-ASAN/` and never overwrites the regular `Bin/` tree.

## Running the editor

```
Bin/Debug/Linux64-Clang/Tools.Editor               # the project manager
Bin/Debug/Linux64-Clang/Tools.Editor <projectDir>  # open a project
```

Add `--mcp` to serve the open project to an agent over MCP while you work in the editor.
