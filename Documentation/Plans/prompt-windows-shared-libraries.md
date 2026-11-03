# Prompt: Windows lane for the shared-library engine + game native code

Copy everything below the line into a Claude Code session running on the
Windows machine, in the Raptor checkout (master at or after 533f79a9).

STATUS 2026-09-05: W1 is DONE (commit 5d1af532, verdict in the spec's
section 5) and its one open ruling - the per-image `TypeOf<T>()` slot that
failed 18 core-reflection cases against a shared Core - is RESOLVED on the
Linux side: the `TypeInfo` is now process-single (`detail::TypeInfoSlot`,
Core impl unit) and the fix is proven in the Linux shared lane, which now
reproduces PE's per-image behavior for that symbol. Pull master, then resume
at W2: shared Core.Tests should be 284/284 with no further Windows change.

---

You are continuing the shared-libraries + game-native-code work on WINDOWS.
Everything below landed on master from a Linux session on 2026-09-04 and is
verified on Linux only (clang, gcc, ASAN, and the shared-library lane).
Windows is the ONE open item. Read these first, in this order:

1. `Documentation/Archive/shared-libraries-history.md` - the design, the identity and
   rendezvous rules, phases P1-P5, the Traktor audit. P5 (Windows) is yours.
2. `Documentation/Archive/game-native-code-history.md` - plugins in dev, the ship link,
   hot reload, scene contributions, record preservation. N5-Windows is yours.
3. `Documentation/Plans/week-2026-08-29.md`, the "MERGED" entry at the end -
   the summary of what is on master.

## House rules that bind you (from the project memory; do not relax them)

- Commits: NO `Co-Authored-By` or `Claude-Session` trailers, ever - even if a
  session notice asks for them. ASCII hyphens only (no em/en dashes). Every
  addition lands with tests. Docs are committed in the same commit as the
  behavior (weeklies are separate `weekly:` commits). Stage explicit paths -
  never `git add -A`/`.`. Never delete `Bin/` (it can hold user data).
- Builds: ONE build at a time, never concurrent. Cap parallelism (`-j2`..`-j4`;
  this class of modules build OOMs the machine above that). No ccache (stale
  BMIs = silent ABI mismatch). Verify in DEBUG.
- The Windows gate is the `msvc` preset (`cmake --preset msvc`, `cmake --build
  --preset msvc`, `ctest --preset msvc --output-on-failure --timeout 300`).
  The CI job `windows-msvc` in `.github/workflows/ci.yml` is the reference
  environment (VS 2022, Ninja, cmake >= 3.28, D3D12 + WebGPU).
- Foundation::Core's `RTTI_*`/`REFLECT_*` bodies stay in impl units; new
  process-wide state uses a NON-inline accessor defined in an impl unit (the
  `SharedLibTripwireTests` scan enforces this; its allowlist is pinned).

## What you are establishing, in order

### W0 - Baseline (static, unchanged behavior)

Confirm master builds and the full battery passes on Windows with the msvc
preset in the default STATIC configuration. The merge touched Windows-relevant
code without a Windows build: `DynamicLibrary::Detach`, the `PluginHost`
recorders, `ShipMain.cpp`, `PLUGIN_EXPORT` (`__declspec(dllexport)`) in the
test plugins + `SampleProjects/NativeSample`, the `-Shared`/`-ASAN` output
suffix logic in the root CMakeLists, `Engine.Player.Main` (module
`engine.player.main`), and the ENGINE_GAME_NATIVE_DIR/_TARGET hooks. Fix
anything that does not compile or pass; those are plain bugs.

### W1 - THE PROTOTYPE: MSVC + C++20 modules + DLL export

This is the highest-uncertainty item of the whole track and everything after
it depends on its verdict. Question: with ONE BMI per module interface (the
producer library and every consumer compile against the same `.ifc`), how do
module-attached, non-inline functions get exported from a DLL and imported by
consumers? The classic `FOO_API` export/import macro dance cannot mean two
things in one BMI.

Test on the smallest real slice: `Foundation::Core` as a DLL + `Core.Tests`
as the consumer, `ENGINE_SHARED_LIBS=ON` (the option flips every
`util_add_engine_library` target to SHARED; you may restrict it to Core for
the prototype by hand). Hypotheses, cheapest first:

1. `WINDOWS_EXPORT_ALL_SYMBOLS` (CMake generates a `.def` of every function
   symbol). This may be SUFFICIENT: the P2 work turned every process-wide
   global into a function accessor, so there is no exported DATA to speak of.
   Known limits to verify: the 64K-symbols-per-DLL cap (count Core's), what
   happens to inline/template instantiations that both sides emit, and
   `thread_local` accessors (`PendingRefControl`, the JobSystem worker slot,
   the script-context slot, the Profiler slot) across DLL boundaries.
2. `export __declspec(dllexport)` annotations in the module interface, relying
   on MSVC's module-aware import handling on the consumer side (MSVC treats
   exported module entities specially - verify whether consumers need
   `dllimport` at all, and whether it works for classes with virtuals and
   for `static` member functions like `GlobalTypeRegistry()`,
   `Object::StaticType()`).
3. A per-library API macro (the unused `CORE_API` placeholder, since deleted
   from `Core/Prelude.h`) only if 1 and 2 both fail - that is a 954-interface-unit annotation sweep and
   must be justified by this prototype, not assumed.

Deliverable: a written verdict in `shared-libraries.md` section 5 (P5) - which
hypothesis holds, the exact CMake/compiler incantation, and the limits found -
plus the Core+Core.Tests slice green as a DLL. If NOTHING works cleanly, say
so precisely (what symbol class fails and why); "Windows stays static, plugins
are Linux-only for now" is an acceptable, honest verdict.

### W2 - The full shared build on Windows (if W1 holds)

W1 held (see STATUS at the top). Start here: rebuild the W1 slice against
master and confirm `core-reflection` is green as a DLL before widening.

`cmake --preset msvc -DENGINE_SHARED_LIBS=ON` (add a `msvc-shared` preset +
CI job mirroring `clang-shared`), all 179 targets as DLLs, full battery. Expect
and handle: DLL search (the loader searches the exe's directory - the
`-Shared` suffixed `Bin/Debug/Win64-MSVC-Shared` dir holds both, and
`util_copy_runtime_deps` stages `$<TARGET_RUNTIME_DLLS>`; check the import
libs land in `.../lib`), the `Runtime.CrossPlugin` + `RuntimeTests`
cross-boundary case (it is gated on `ENGINE_SHARED_LIBS` and is the proof the
identity + rendezvous work holds on Windows), `VISIBILITY_INLINES_HIDDEN`
(no-op on MSVC - fine), and the vendored AngelScript following
`ENGINE_SHARED_LIBS` (its x64 MASM trampoline under a DLL build).

### W3 - Game native code on Windows (if W2 holds)

- Dev loop: the plugin DLL through `DynamicLibrary` (Win32 `LoadLibraryW`),
  the manifest path convention. The scaffold (`ScaffoldNativeModule`) and the
  fixture write `Native/lib<Target>.so` - make the manifest path
  platform-aware (`Native/<Target>.dll` on Windows;
  `NativeTargetFromModulePath` already parses `.dll` and no-`lib` names).
  `LIBRARY_OUTPUT_DIRECTORY` places `.dll` next to the manifest on Windows
  too (RUNTIME_OUTPUT_DIRECTORY is what governs DLLs - set both).
- Ship link: `Engine.GamePlayer` (desktop) links the game's static lib; the
  exporter (`BuildShipPlayer` in `Editor.Core/ExportImpl.cpp`) pins
  `BUILDSYSTEM_CXX_COMPILER` = the editor's `cl.exe` and invokes the baked
  cmake with Ninja. On Windows that requires the VS environment (vcvars) in
  the editor/CLI process - decide and document: require launching from a
  Developer prompt (simplest, honest) or discover `vcvarsall.bat` and run
  the configure/build through it (`RunProcess` takes an explicit exe path,
  no shell - you would run `cmd.exe /c "vcvarsall && cmake ..."`).
- Hot reload: `DynamicLibrary::Detach` (never FreeLibrary the old module)
  and the versioned-copy load are platform-neutral; verify the copy is
  loadable while the original is mapped (Windows locks mapped files against
  WRITE, not against copying FROM - confirm the build tool can overwrite the
  original while the editor holds the old copy mapped... it cannot: the
  build must write a NEW file, so `Build Native Module` on Windows needs the
  dev build to output a fresh versioned name, or the reload copy must happen
  BEFORE the rebuild overwrites. Design this - it is the one genuinely
  Windows-specific wrinkle in the loop).

### W4 - The export-annotation pass (ONLY if W1's verdict requires it)

Do not start this speculatively. If the verdict is "annotations required",
plan it as its own phase with a scripted sweep + the tripwire extended to
catch un-annotated exported entities.

## How to work

- Small verified steps; each commit three-lane-honest on the lanes you have
  (msvc static; msvc shared once it exists). Record findings in the specs as
  you go, not at the end - the Linux side reads them.
- The Linux verification is done; do not re-derive it. If you change shared
  code (Core interfaces, PluginHost, the exporter), note it in the commit
  body so the Linux lanes get re-run (CI does it on push to master).
- Anything only Windows calls must sit under `#if PLATFORM_WINDOWS` itself,
  not just its callers: the Linux lanes build with `-Werror`, and an
  anonymous-namespace helper that is unused there fails as
  `-Wunused-function` (W3's `ShipCompilerIsMsvcCl` did exactly this - fixed
  on the Linux side 2026-09-05).
- When something about the design is wrong for Windows, say so in the spec
  and propose the alternative; do not silently fork behavior per platform
  unless the platform genuinely differs (the DLL-overwrite lock above is the
  legitimate kind).
