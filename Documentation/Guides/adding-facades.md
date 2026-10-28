# Adding a script facade

A *facade* is a reflected class scripts call as a static surface (`Scene.spawn(...)`,
`Net.startServer(...)`). Both backends (Wren, AngelScript) bind
facades by reflection, so one C++ definition serves both languages.

There are **two homes** for a facade. Pick by what the facade is *about*.

## 1. Core facades (rare) - `draconic.script.facades` (Foundation)

The neutral curated surface every behavior gets: **Entity, Log, Time, Random, Scene**.
These are engine-agnostic language primitives. Adding one here is unusual - only do it if
it is a genuinely core, subsystem-free primitive. They live on the shared
`ScriptRuntimeBinding` (service key `script.runtime`) and are registered by
`RegisterScriptFacadeReflection()`.

## 2. Out-of-tree facades (the default) - owned by the subsystem's project

Anything tied to a subsystem (networking, UI, level loading, audio, input, physics) is
owned by **that subsystem's module**, NOT Foundation. Foundation must not gain a dependency
on, or knowledge of, an engine subsystem. All six current facades share ONE architecture
(reflected `Object` + static methods + a per-context service key resolved through
`CurrentScriptContext()->GetService(key)` + no-op-when-unwired guards + dual registration).
They differ only in two orthogonal, motivated axes: the **binding payload** and the **wiring
shape**. Reference implementations:

| Facade        | Home module                    | Service key            | Binding payload            | Wiring shape |
|---------------|--------------------------------|------------------------|----------------------------|--------------|
| `Net`         | `draconic.net.manager`         | `net.runtime`          | controller interface ptr   | Install/Clear helpers |
| `SceneLoader` | `draconic.engine.gameinstance` | `sceneloader.runtime`  | host-filled function ptrs  | Install/Clear helpers |
| `Physics`     | `draconic.engine.physics`      | `physics.runtime`      | binding struct (subsystem) | subsystem `ExposeToScript` |
| `Audio`       | `draconic.engine.audio`        | `audio.runtime`        | binding struct (+resources)| subsystem `ExposeToScript` |
| `Input`       | `draconic.engine.input`        | `input.runtime`        | bare `ActionRuntime*`      | subsystem `ExposeToScript` |

**Binding payload** = what the per-context service holds. Function pointers when the host owns
the state (Ui/SceneLoader), a controller interface when a stable object implements it (Net), a
subsystem-owned binding struct (Physics/Audio), or the runtime object directly (Input). Pick
whatever the facade actually needs to reach.

**Wiring shape** = how the binding gets onto a context. Two blessed shapes:
- **Install/Clear free functions** (`Install<X>ScriptService(ctx, binding)` / `Clear...`): use
  when the binding is host-owned and re-bound per instance/context (the app fills it, then
  installs). This is the recipe template below.
- **Subsystem `ExposeToScript(ctx)` method**: use when the SUBSYSTEM owns the binding as a
  member and simply hands itself to the context; it inlines `SetService`. No `Clear` is needed
  because a per-context service dies with the context. Physics/Input/Audio use this.

Conventions that hold across BOTH shapes (keep them uniform): service-key string is
`<name>.runtime`; the key constant is `k<Name>ScriptService`; the boot-time registrar is
`Register<Name>ScriptFacade()` and registers BOTH the type and the prelude name (see below).

Copy the closest match. The recipe (using `Ui` as the Install/Clear template):

### a. Binding struct + service key (interface unit, `.cppm`)

```cpp
inline constexpr StringView kUiScriptService = u8"ui.runtime"; // distinct per facade

// Installed as a per-context service; the facade resolves it. Holds either host-installed
// Function pointers (Ui/SceneLoader) OR a controller interface pointer (Net). The app fills
// it; null pointers = safe no-ops.
struct UiScriptBinding
{
    core::Function<i32(const core::Guid&)> pushOverlay;
    // ... one field per method ...
};

inline void InstallUiScriptService(script::IScriptContext& ctx, UiScriptBinding& b)
{ ctx.SetService(kUiScriptService, &b); }
inline void ClearUiScriptService(script::IScriptContext& ctx)
{ ctx.SetService(kUiScriptService, nullptr); }

void RegisterUiScriptFacade(); // decl; body in the impl unit
```

### b. Facade class (interface unit)

```cpp
class Ui final : public Object
{
    DRACONIC_OBJECT(Ui, Object)                    // needs Reflect.h in the module fragment
public:
    [[nodiscard]] static UiScriptBinding* Resolve()
    {
        script::IScriptContext* ctx = script::CurrentScriptContext();
        return ctx ? static_cast<UiScriptBinding*>(ctx->GetService(kUiScriptService)) : nullptr;
    }
    [[nodiscard]] static i32 pushOverlay(core::Guid doc)
    {
        UiScriptBinding* b = Resolve();
        return (b && b->pushOverlay && !doc.IsNil()) ? b->pushOverlay(doc) : 0; // guard!
    }
    // ... one static method per surface entry, each Resolve()-guarded ...
};
```

### c. Reflection body + registration (IMPLEMENTATION unit, `.cpp`)

Kept out of the interface unit - the GCC gcm-cluster rule (`DRACONIC_REFLECT_*` bodies never
sit in a `.cppm`).

```cpp
DRACONIC_REFLECT(Ui, "draconic::ui")
{
    builder.Method<&Ui::pushOverlay>("pushOverlay");
    // ... one Method<> per surface entry ...
    builder.Constructor(); // Wren only materializes constructible foreign classes
}

void RegisterUiScriptFacade()
{
    static const bool once = []()
    {
        GlobalTypeRegistry().Register(Ui::StaticType());
        draconic::script::RegisterExtraFacadeName(u8"Ui"); // Wren behavior-prelude import hook
        return true;
    }();
    (void)once;
}
```

### d. CMake

Add `Draconic::Script Draconic::Script.Facades` to the module's `target_link_libraries`
(Script.Facades only for `RegisterExtraFacadeName`).

### e. Wire it into the run (app + instance)

- **Register the type** at app startup: call `RegisterUiScriptFacade()` in
  `DefaultApplication::Configure` beside `RegisterNetScriptFacade()` et al.
- **Fill the binding**: the app fills the binding's pointers (capturing whatever host state
  they need - content DB, subsystem, instance). Store the binding as a *stable member* of the
  owner (e.g. a `GameInstance` member) so it never dangles.
- **Install per context**: call `InstallUiScriptService(context, binding)` when the run
  context exists (for per-instance facades, alongside the instance's other service installs
  in `StartScript`).

## How the backends see it

`RegisterReflectedTypes(manager)` emits every registered type as a foreign class into the
Wren `"main"` module and registers it with the AngelScript engine. Then:

- **Wren**: behavior modules import the facade via a prelude line built from the facade-name
  list - `RegisterExtraFacadeName(u8"Ui")` adds `Ui` to it. A top-level `"main"` script
  reaches it directly (already in `"main"`). Statics: `Ui.pushOverlay(...)`.
- **AngelScript**: statics become global functions in a namespace named after the class:
  `Ui::pushOverlay(...)`. No prelude/import needed; binding is by registry.

## Rules & gotchas (learned the hard way)

- **Reserved contract-class names: `Game` and `Level`.** These are names a user's own class
  *mandatorily* takes - the game orchestrator class is `Game` (`StartScript` does
  `CreateInstance("Game")`), and the scene-scripting tier's class is `Level` (instantiated once
  per scene from `SceneScriptSettings::script`). A facade sharing either name is a hard
  AngelScript error (`Name conflict. '<name>' is an extended data type`) and a Wren
  `import ... for <name>` clash - the user's class can't compile. This is why the level-load
  facade is `SceneLoader`, not `Game`. `RegisterExtraFacadeName` **refuses** either name (logs
  an error, adds nothing) so the user's class always wins; the guard is covered by
  `script.facades: reserved contract-class names ...` in `Draconic.Engine.GameInstance.Tests`.
  Avoid any other name a user's own class is likely to take.
- **Register the TYPE *and* the NAME.** A foreign facade needs both
  `GlobalTypeRegistry().Register(X::StaticType())` (binds the type) and
  `RegisterExtraFacadeName(u8"X")` (publishes the name to the Wren behavior/Level prelude,
  `import "main" for ... X`). Type-only makes the facade reachable from top-level `main`/Game
  scripts but INVISIBLE to Wren behaviors + Level scripts (compile error "Variable 'X' is not
  defined"); AngelScript is unaffected (binds by registry). Do BOTH in the same
  `Register<X>ScriptFacade` (idempotent). Physics/Input/Audio shipped type-only by mistake and
  only worked from `main` until fixed (b535d707, 2026-08-03). The impl unit needs
  `import draconic.script.facades` + the `Draconic::Script.Facades` CMake link dep.
- **Every method must guard.** An unwired binding (bare cook VM, no host, or pre-install) must
  be a safe no-op. For a poll that gates a loop (`while (!complete) yield`), the unwired /
  unknown-ticket case returns **true** so the loop never hangs.
- **Facade numerics.** Method-path args narrow to the reflected C++ type, so write natural
  types: handles/tickets `i32`, progress/ratios `f64`. (Globals still surface as `f64` - a
  Wren floor - but method params do not.)
- **Null-scene safety.** A facade may be called from the game orchestrator's `launch()` before
  any scene exists (boot reorder, task #123). Guard anything scene-dependent
  (`currentScene == nullptr` -> safe default), and add a pre-scene battery.
- **Callbacks = `RefPtr<IScriptDelegate>`.** A script fn parameter is spelled
  `core::RefPtr<script::IScriptDelegate>`; both backends marshal a script function into one,
  and holding the RefPtr keeps it GC-alive. `Invoke(args)` calls back into script, marshalling
  against the handler's ACTUAL arity - so the native side fires with exactly the args the
  callback carries (a click fires with none), and a handler of any shape adapts. Handler
  authoring per backend:
  - **Wren**: `Fn.new { ... }` (any arity) - `Ui.onClick(h, "id", Fn.new { ... })`.
  - **AngelScript**: the delegate param is `?&in`, so pass a funcdef handle of ANY signature.
    The engine provides `Action` (void()) for the common case and `ScriptDelegate` (double(double))
    for an args+return callback; declare your own `funcdef` for other shapes:
    `void onCancel() { ... } ... Ui::onClick(h, "id", Action(@onCancel));`.
- **AngelScript reflected/resource properties are HANDLES (`Type@`), never value members.** An
  editor property whose type is a reflected reference type - `Color`, `Float3`, `Entity`, or an
  `asset:<Type>`-tagged `Guid` - MUST be declared as a handle in an AngelScript behavior:
  `Color@ tint;`, `Guid@ clip;` (idiomatic AngelScript for a reference type). Every reflected type
  is registered `asOBJ_REF`, so a plain VALUE member (`Color tint;`) is owned by AngelScript, which
  lazily reconstructs it and SILENTLY DROPS the applied value at runtime. The AngelScript cook
  ENFORCES this - a value member of a reflected property is a cook error with the exact fix
  ("declare it `Guid@ mesh`"); a runtime warning backs it up for non-cooked paths. Scalars
  (`float`/`int`/`bool`), `string`, and enums are real value types and stay plain members. (Wren has
  no such split - its `name=(v)` setter runs in-VM. See game-ready-scripting.md Section 16.)
- **Distinct service key** per facade (`<name>.runtime`), separate from `script.runtime`.
- **Editor-only facades** register from the editor `TypeDomain` (see the reflection track), so
  they surface only in the editor's script context.

## Testing (required)

Battery lives in the **owner's** test project, raw-context Net-style, on **BOTH** backends
(`Draconic.Net.Manager.Tests/FacadeScriptTests.cpp`, `Integration.ScriptFacades/
AudioFacadeScriptTests.cpp`, and the `SceneLoader` cases in `Draconic.Engine.GameInstance.Tests`
are the templates):

```cpp
RegisterCoreTypes();
ui::RegisterUiScriptFacade();
RefPtr<IScriptManager> mgr = wren::CreateScriptManager();   // or angelscript::
RegisterReflectedTypes(*mgr);
RefPtr<IScriptContext> ctx = mgr->CreateContext();
UiFake fake; ui::InstallUiScriptService(*ctx, fake.binding); // fake records host calls
REQUIRE(ctx->Load(u8"...Ui.pushOverlay(...)...", u8"main").IsOk());
// assert fake recorded the calls; for callbacks, fire fake.clickFn->Invoke(...) and check.
```

Wren uses top-level statements; AngelScript uses a `void main()` (auto-run on Load) + globals
read back with `GetGlobal`. Cover: every method routes, numerics round-trip, an unwired
binding no-ops, and (if any) a delegate callback fires.
