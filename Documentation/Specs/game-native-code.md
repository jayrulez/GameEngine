# Game native code

Status: SPEC (2026-09-04). Track branch: `shared-libraries` (builds directly on
that track; see shared-libraries.md sections 6-7 for the identity/rendezvous
foundation and the full Traktor audit this plan is grounded in).

Goal: a project can carry native C++ game code. In DEV it is a plugin the
shared-build editor/player loads (fast iteration, play-in-editor). For SHIPPING
the exporter links it statically into a branded player executable (full LTO,
single artifact, works on every platform incl. web). Same source both ways.

## The model (decided)

- Game native code is a MODULE like any engine library: declared through
  util_add_engine_library, following every house rule (tests, allocator
  idiom, tripwires). One artifact kind - the Traktor lesson.
- The game exposes an IRuntimePlugin (the existing Foundation::Runtime
  contract): OnLoad(Context&) performs ALL registration explicitly
  (components, script facades, subsystems). No static-init registration -
  this is what makes dual-mode linking trivial (no forced-reference chains,
  no whole-archive; Traktor's Module.cpp tax does not exist here).
- DEV (ENGINE_SHARED_LIBS builds): PluginHost::Load dlopens the game module
  (the ruled plugin model, proven by Runtime.CrossPlugin). SHIP: a generated
  stub passes CreateGamePlugin() into the launcher; PluginHost::Add - the
  static path that already exists - registers it on the same Context at the
  same point. The two modes converge on PluginHost.
- The SEAM (verified in code): the runtime Context is owned by the
  application host (IApplicationHost::Ctx()); PlayerApplication extends
  DefaultApplication; Main.cpp's header already names this exact evolution:
  "projects that outgrow scripts graduate to a native IApplication at the
  same seam".

## Phases

- N1 - launcher-as-library. Engine.Player's main() moves into a
  Engine.Player.Main STATIC library exposing
  PlayerMain(argc, argv, IRuntimePlugin* nativeGame /*nullable*/); the
  Engine.Player executable becomes a thin stub passing nullptr. Zero
  behavior change; verified in static + shared lanes. (The Traktor trick:
  Runtime.App builds as an executable in dev and a main()-carrying static
  library for the ship link.)
- N2 - dev loop (player half SHIPPED 2026-09-04). The manifest field
  ALREADY EXISTED (ProjectSettings.nativeModule, reserved + serialized -
  no version bump). Convention: a PROJECT-RELATIVE path to the built
  module (e.g. Native/libMyGame.so). PlayerApplication::OnStartup hosts a
  PluginHost after the base startup (engine subsystems resolvable in
  OnLoad) and before initial-scene resolution (plugin types deserialize):
  ship path = PluginHost::Add(options.nativeGame); dev path =
  PluginHost::Load(projectDir/nativeModule), failure reported and the run
  continues (scripts still work). OnShutdown unloads the game FIRST, while
  Ctx() lives. Proven end to end: shared player + a scratch project
  declaring Runtime.CrossPlugin -> "native game module ... loaded
  ('CrossPlugin')". N2 COMPLETE: the editor half loads the
  module against the embedded runtime context at project open (toast +
  console on failure), and the Project Settings dialog carries the
  free-text "Native module" row.
- N3 - ship link (mechanism SHIPPED 2026-09-04; exporter integration
  remaining). SIMPLIFICATION over the original plan: nothing is generated.
  The engine build HOSTS the game - ENGINE_GAME_NATIVE_DIR (cache var) adds
  the game's native directory as a subdirectory after all engine targets
  (full module/BMI access; util_add_engine_library gives the dev .so and
  the ship static lib from one source), and ENGINE_GAME_NATIVE_TARGET
  materializes Engine.GamePlayer: a CHECKED-IN ShipMain.cpp (extern "C"
  CreatePlugin - the same symbol PluginHost dlopens in dev - statically
  referenced, so the game archive's registration links with no
  whole-archive/forced-ref machinery) + Engine.Player.Main + the game
  target + runtime-dep staging + $ORIGIN. PROVEN: a scratch game module
  wired by hand builds Engine.GamePlayer with ZERO engine .so deps, and
  its plugin OnLoad runs through PluginHost::Add, taking precedence over
  the manifest's dlopen module. EXPORTER HALF SHIPPED
  2026-09-04: ExportOne detects nativeModule (non-web presets), invokes the
  BAKED cmake (BUILDSYSTEM_CMAKE_PATH) over the BAKED engine root with the
  BAKED compiler (the system default silently differed - pinned to the
  editor's own toolchain), builds Engine.GamePlayer in a persistent
  per-project dir (<project>/.cache/ship-<config>; incremental relinks;
  build type = preset.config), with a per-project Bin suffix
  (-Ship-<name>), and stages it as the dist player in place of the
  template binary. Convention: native source at <project>/Native; target
  name derived from the nativeModule basename. PROVEN end to end via
  Tools.Export on the scratch project: the dist's ScratchGame binary boots
  dist mode and runs the statically-linked plugin. Speed note: the
  target-scoped build compiles only the player's dependency closure (~a
  quarter of the tree). Tests: target-derivation + suffix helpers.
  Remaining: a committed fixture project for CI-able E2E; editor-side
  export UI needs no change (same ExportOne).
- N4 - authoring (SHIPPED 2026-09-04 as GRADUATION, not a new-project
  checkbox - projects start scripts-only and graduate, matching the
  Main.cpp seam comment). Three Project-menu actions complete the loop:
  * Add Native Code... - editor::ScaffoldNativeModule generates the
    NativeSample reference shape parameterized by the project name
    (NativeTargetNameFromProjectName sanitizes; the generated CMakeLists
    sets LIBRARY_OUTPUT_DIRECTORY so the dev .so lands AT the manifest
    path), wires nativeModule, saves the manifest; refuses (AlreadyExists)
    when Native/ exists or a module is declared - never overwrites.
  * Build Native Module - editor::BuildDevNativeModule (background job):
    a persistent per-project SHARED-engine build (.cache/native-dev,
    -Dev-<name> Bin suffix, pinned compiler) building just the game
    target; success = the .so at the manifest path.
  * Reload Native Module - the N6 flow.
  PROVEN: scaffold tests (shape, manifest wiring, sanitization, refusal)
  + the GENERATED code compile-proven in the shared lane with its .so
  landing at the manifest path. The NativeSample fixture carries the same
  output-dir rule so fixture and generator stay one shape.
- N5 - platforms. **WINDOWS SHIPPED 2026-09-05** (was: waits on the MSVC
  modules/dllexport prototype). The dev loop works: the module builds as
  `Native/<Target>.dll` through the same `util_add_engine_library` seam, the
  editor loads it via `LoadLibraryW`, and `CreatePlugin` exports with no
  annotation beyond the existing `PLUGIN_EXPORT` (verified on the NativeSample
  reference module against a shared engine). Four Windows-specific things had
  to change, all small:

  - **The manifest path is platform-shaped.** MSVC names the module
    `<Target>.dll` (no `lib` prefix), ELF `lib<Target>.so`, so
    `ScaffoldNativeModule` writes the local platform's spelling. But a project
    is checked into source control and opened on BOTH platforms while the
    manifest holds ONE string - so the editor now RESOLVES: it takes the
    declared path when the file exists, and otherwise falls back to this
    platform's spelling of the same target (`NativeTargetFromModulePath`
    already parsed both shapes). A Linux-authored project opens on Windows.
  - **`RUNTIME_OUTPUT_DIRECTORY` as well as `LIBRARY_OUTPUT_DIRECTORY`.** On
    Windows the DLL is governed by RUNTIME_*; with only LIBRARY_* set the .dll
    lands in the default bin dir and the manifest points at nothing. Fixed in
    the scaffold template and in NativeSample's CMakeLists.
  - **THE HOT-RELOAD WRINKLE, and it is real.** Measured on this machine:
    with the module mapped by `LoadLibraryW`, overwriting that file is
    **BLOCKED**; with only a staged copy mapped, overwriting the original
    **SUCCEEDS**. Reload already loaded a versioned copy (dlopen refcounts by
    path), but the FIRST load at project open mapped the declared path
    directly - so "Build Native Module" would fail to relink for as long as
    the project stayed open. Now EVERY load goes through
    `StageNativeModuleCopy()`; the declared path is only ever read. No change
    to the build tool was needed, which is the outcome the plan hoped for.
  - **Dependent-DLL search resolves favourably.** The module links the engine
    DLLs, and Windows searches the loading process's exe directory - which is
    `Bin/<cfg>/Win64-MSVC-Shared`, where those DLLs already live. Nothing to
    stage, no PATH setup. (A bare `LoadLibraryW` from a process elsewhere does
    fail; that is a property of the caller, not of the module.)

  Ship path (static, no export macros) is unchanged. Both the ship and dev
  builds shell out to cmake, and when the pinned toolchain is **MSVC cl.exe**
  they need the Visual Studio environment in the editor/CLI process. RULING:
  require it and say so - a preflight check on `VCToolsInstallDir` fails with
  one clear message, exactly as the web lane already does with `EMSDK`.
  Discovering `vcvarsall.bat` and running everything through
  `cmd /c "vcvarsall && cmake ..."` was rejected: `RunProcess` takes an exe
  path with no shell, so it buys a guess at the VS install, a second quoting
  layer around every argument, and an exit code that could come from either
  command. The check is narrow on purpose - **Windows hosts clang too** (the
  `clang` preset drives clang++ at the MSVC ABI with lld-link), and clang finds
  an MSVC installation itself, so it fires only when the baked
  `BUILDSYSTEM_CXX_COMPILER` basename is exactly `cl` (deliberately not
  matching `clang-cl` or `clang++`). Measured: configuring the whole tree with
  clang++ and NO vcvars succeeds.
  WEB SHIPPED 2026-09-04 (ship-only by design - no dlopen in browsers):
  the wasm lane hosts the game the same way (the Emscripten branch of the
  root CMakeLists return()s early, so it carries its own game-native
  hook), and Engine.GamePlayer (web) = WebMain.cpp + ENGINE_SHIP_NATIVE_GAME
  (MakeOptions passes CreatePlugin()) + the game target, as
  Engine.GamePlayer.{html,js,wasm} - its OWN basename (emscripten bakes the
  js name into the html and the wasm name into the js, and the template
  player shares the build, so the template's name cannot be reused).
  Exporter: a Web preset + nativeModule -> BuildShipPlayer(web=true):
  Emscripten toolchain from $EMSDK (clear error when unset: launch from a
  shell with emsdk_env.sh sourced), <project>/.cache/ship-web-<config>,
  -j2 (wasm-ld + ASYNCIFY is memory-hungry - an earlier -j4 wasm link
  OOM-killed the machine), stages the trio and SKIPS the template's
  js/wasm sidecars so they never clobber it. PROVEN end to end via
  Tools.Export: the web dist carries Engine.GamePlayer.{html,js,wasm}
  (plugin inside the wasm), WGSL shaders.dpak, per-family paks, serve.py.
  Found + fixed on the way: WebMain.cpp had been broken since the I5
  allocator sweep (a mangled EnumerateAdapters call + a missing
  CreateBackend allocator arg - the wasm lane is not in the commit gates),
  and both web players now restate -lwebsocket.js (Net.WebSocket's
  INTERFACE link option does not cross static private deps).

- N6 - hot reload (BUILT 2026-09-04 in 2e695c7b5 as Project > Reload Native
  Module, the scene-bracketed flow below; the gnarly part). What the
  plugin left behind decides everything - the inventory on unload:
  subsystems in Context (OnUnload removes - existing contract), TypeInfo*
  in GlobalTypeRegistry pointing INTO the .so, factory/function pointers in
  SerializableRegistry + script facade registries + resource factories,
  live RefCounted objects whose vtables + destroy fns live in the .so,
  script bindings + live script objects over plugin types, scene components
  from plugin managers.

  RULING - RUN-BRACKETED RELOAD ONLY: reload happens between runs, never
  during one. Editor flow: stop the PIE run (scenes + script contexts are
  transient and die with it - plugin-created objects go with them) ->
  scope-reversed unregistration -> dlclose -> dlopen the rebuilt module ->
  OnLoad -> restart the run (scene state reloads from disk). NO live-state
  migration/reinstancing - out of scope BY DESIGN (that is the Unreal
  reinstancing swamp; run-transient PIE makes it unnecessary).

  EDITING SCENES (the MyFancyComponent case - a plugin component authored
  in a scene that is OPEN FOR EDITING when the reload happens): the run
  bracket extends to a SCENE BRACKET riding the existing Simulate-snapshot
  machinery. (1) SNAPSHOT every open scene to memory through the wire
  format (unsaved edits included - the wire format is the contract, not
  the memory layout); (2) DESTROY the scenes while the OLD .so is still
  loaded (component + manager teardown runs against the code that created
  them); (3) scope-reverse, liveness-guard (now also: no scene alive holds
  module types), dlclose, dlopen, OnLoad; (4) RESTORE from snapshots -
  components resolve by name/id through the NEW module's registrations,
  managers are recreated by its scene-manager contribution, inspectors
  rebuild via TypeId. Serialization IS the migration layer: a layout
  change goes through the normal DataVersion gates; an ungated change
  fails that payload LOUDLY (the strict-versioning rule working as
  intended during native iteration - a signal, never corruption). Raw
  pointers into component storage across the reload are already illegal
  (resolve-per-use rule; pools swap-remove even without reloads).

  MECHANISM - RegistrationScope (SHIPPED 2026-09-04 for the type +
  serializable registries): PluginHost arms AMBIENT registration observers
  for the duration of OnLoad - the plugin registers through the normal
  global calls, everything actually INSERTED is recorded per entry, and
  UnloadAll reverses the recording (reverse order) after OnUnload, before
  dlclose. Observers fire only on real inserts, so a type another party
  already owns is never recorded or torn down. TypeRegistry +
  SerializableRegistry gained Unregister + SetRegistrationObserver
  (remove-by-id - the P1 groundwork). PROVEN in the shared lane: the
  cross-boundary case now runs the full round-trip (load -> registered ->
  unload -> GONE from the registry -> reload -> re-registered + OnLoad
  resolves the host subsystem again). EDITOR RELOAD FLOW SHIPPED
  (v1): Project > Reload Native Module - stops any game run
  (StopGameRun), scope-reversed UnloadAll(closeLibraries=false) so the OLD
  mapping stays alive for the process lifetime (leak-on-purpose:
  DynamicLibrary::Detach - never free pages under a pointer teardown
  missed), then loads a FRESH VERSIONED COPY from .cache/native-hot/
  (dlopen refcounts by path; only a new file yields a new module), OnLoad
  re-registers; toasts the outcome. SCENE BRACKET SHIPPED with S1
  (2026-09-04): Reload snapshots every live scene on the embedded runtime
  (SceneSnapshot::Capture over the SceneSubsystem registry) BEFORE the
  unload withdraws contributed managers/systems, and restores AFTER the
  rebuild re-contributes them - plugin components rehydrate by name; the
  restore runs even when the reload fails (records stay preserved as
  unresolved). PROVEN at the runtime level by the cross-boundary test's
  MyFancyComponent walkthrough (live scene -> plugin contributes its
  manager -> component authored -> snapshot -> unload strips the manager
  -> rebuilt copy loads -> restore -> the value survives). Still open: script-facade registries (die
  with per-run script contexts; audit when dynamic rebinding lands).

  SAFETY - liveness guard before dlclose: verify the run is stopped and the
  module's types have no live instances; on ANY doubt, SKIP dlclose (leak
  the old .so deliberately - unreachable stale code is harmless, a freed
  page under a live vtable is not) and still load + register the new
  module (the scope removed the old registry entries, so the new ones
  take by id).

  CACHE RULE (extends the P1 identity rule): long-lived caches store
  TypeId and re-resolve TypeInfo* per use via FindById/Canonical - a
  cached TypeInfo* does not survive a reload.

- S1 - scene-manager/system contributions (SHIPPED 2026-09-04). The seam
  was already the right shape: every scene - runtime (SceneSubsystem's
  installer) and headless (AddAllSceneManagers) - instantiates from a
  SceneComposition, so SceneModuleContributions (foundation.scene, one per
  process) holds runtime-added modules and Instantiate/RegisterReflection
  append them. A plugin contributes from OnLoad: {id, install fn,
  reflection fn, systemType = TypeOf<TheSystem>().id}; that covers
  component MANAGERS, plain SCENE SYSTEMS, and their SETTINGS blocks alike
  (a contributed system is a SceneSystem with SettingsType/SettingsId).
  Live scenes: the SceneSubsystem installs itself as the registry's
  live-scene sink while registered, so Add applies to scenes already alive
  (project-open ordering, hot reload) and Remove withdraws via the new
  Scene::RemoveSystem(TypeId) - the system dies while its code is mapped.
  Recording: PluginHost gained pluggable IRegistrationRecorders;
  Engine.Scene's SceneContributionRecorder (player + editor hosts add it)
  records contributions per plugin and reverses them FIRST on unload.
- S3 - unresolved-record preservation (SHIPPED 2026-09-04). The reader
  used to SKIP records of unknown component types / system settings
  (data loss on the next save). Now both are kept verbatim on the Scene
  (UnresolvedComponent / UnresolvedSettings: owner or system id + payload
  + which encoding captured it) via the serializer's existing
  RawRemainder passthrough (hoisted to ISerializer; binary = the v2
  length-prefixed blob, text = the captured XML element children), written
  back untouched in the SAME encoding (a cross-encoding write drops them
  LOUDLY - an unknown type cannot be re-encoded), and RESOLVED into real
  components/settings the moment their manager/system arrives
  (ResolveAllUnresolvedRecords - the live-install hook). Proven by
  round-trip tests in both encodings incl. late-arrival resolve.
  Prefab instances too (2026-09-04): component ops (instance overrides)
  of an absent type are stashed on the PrefabInstanceState
  (unresolvedComponentOps), re-emitted by ComputeInstanceDeltas on save,
  and applied by ResolveAllUnresolvedRecords when the manager arrives.

  SCRIPT-FACADE AUDIT (2026-09-04, closed - no work needed): the script
  manager is PER RUN - ScriptSubsystem::EnsureContext creates it lazily at
  a run's first script use and binds every type then registered
  (RegisterReflectedTypes over GlobalTypeRegistry().All()); the run host's
  Teardown (GameInstance stop) destroys it. Reload is run-bracketed
  (StopGameRun first), so a reloaded module's facades bind fresh at the
  next run and AngelScript's inability to unregister types never matters.
  The only rule: never reload during a run (already enforced).

## Rules established

- Native game code never registers via static initializers; OnLoad is the
  one registration entry.
- The ship path never invokes a linker directly; it generates CMake.
- Dev-loop native loading exists only in shared builds; a static
  editor/player with a project declaring a native module reports it
  clearly and runs without it (scripts still work).
