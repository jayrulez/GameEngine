# Adding a script facade

A *facade* is a reflected class scripts call as a static surface (`Scene.spawn(...)`,
`Net.startServer(...)`). Both backends (AngelScript, Luau) bind
facades by reflection, so one C++ definition serves both languages.

There are **two homes** for a facade. Pick by what the facade is *about*.

## 1. Core facades (rare) - `foundation.script.facades` (Foundation)

The neutral curated surface every behavior gets: **Entity, Log, Time, Random, Scene**.
These are engine-agnostic language primitives. Adding one here is unusual - only do it if
it is a genuinely core, subsystem-free primitive. They live on the shared
`ScriptRuntimeBinding` (the run host's own service) and are registered by
`RegisterScriptFacadeReflection()`.

## 2. Out-of-tree facades (the default) - owned by the subsystem's project

Anything tied to a subsystem (networking, UI, level loading, audio, input, physics) is
owned by **that subsystem's module**, NOT Foundation. Foundation must not gain a dependency
on, or knowledge of, an engine subsystem. Every facade shares ONE architecture: a reflected
`Object` with static methods, each resolving its **binding** by type with
`script::CurrentService<Binding>()`, guarded to do nothing when there is none, and registered
twice (the type and the prelude name).

### Where a binding comes from: the run, or the application

A script context asks its run host for a service by type, and the host answers in this order:

1. its own `ScriptRuntimeBinding` (time, the RNG, `entity.send`, the resource manager, the run);
2. the services of the **run** the context belongs to (`IRun::Get<T>`), for per-run state;
3. the **application's** services (`SubsystemManager::GetService<T>`), for app-wide state.

So a binding is offered in one of two places, and nothing lists them:

- **Per-run** (the run's save, HTTP, networking, its own input runtime): the run holds it.
  `GameInstance` registers its members on itself (`Register<RunSave>(m_save)`); a plugin adds
  one to each run from a run listener (`run.Add<T>(...)` in `OnRunCreated`).
- **App-wide** (audio, render, GUI, the shared input runtime): the subsystem registers it with
  its manager while it is registered:

```cpp
void OnRegister(foundation::runtime::SubsystemManager* manager) override
{
    Subsystem::OnRegister(manager);
    manager->RegisterService(m_scriptBinding);
}
void OnUnregister() override
{
    if (foundation::runtime::SubsystemManager* manager = Manager())
        manager->UnregisterService<UiScriptBinding>();
    Subsystem::OnUnregister();
}
```

The run comes first, so a run can hold its own copy of an app-wide type: each `GameInstance`
holds an `ActionRuntime`, and its scripts read that one rather than the shared runtime. A
context outside every run (the editor's scenes) sees the application's services alone.

**Binding payload** = what the service holds: function pointers when the host owns the state,
a controller interface when a stable object implements it (`NetScriptBinding`), a pointer to the
subsystem (`AudioScriptBinding`, `RenderScriptBinding`), or the runtime object itself
(`ActionRuntime`). Pick whatever the facade actually needs to reach.

The boot-time registrar is `Register<Name>ScriptFacade()` and registers BOTH the type and the
prelude name (see below).

### a. Binding struct (interface unit, `.cppm`)

```cpp
// The facade's service, found by type. The owner fills it; null pointers are safe no-ops.
struct UiScriptBinding
{
    core::Function<i32(const core::Guid&)> pushOverlay;
    // ... one field per method ...
};

void RegisterUiScriptFacade(); // decl; body in the impl unit
```

### b. Facade class (interface unit)

```cpp
class Ui final : public Object
{
    RTTI_OBJECT(Ui, Object)                        // needs Reflect.h in the module fragment
public:
    [[nodiscard]] static UiScriptBinding* Resolve() { return script::CurrentService<UiScriptBinding>(); }
    [[nodiscard]] static i32 pushOverlay(core::Guid doc)
    {
        UiScriptBinding* b = Resolve();
        return (b && b->pushOverlay && !doc.IsNil()) ? b->pushOverlay(doc) : 0; // guard!
    }
    // ... one static method per surface entry, each Resolve()-guarded ...
};
```

### c. Reflection body + registration (IMPLEMENTATION unit, `.cpp`)

Kept out of the interface unit - the GCC gcm-cluster rule (`REFLECT_*` bodies never sit in a
`.cppm`).

```cpp
REFLECT_MEMBERS(Ui, "rtti::ui")
{
    builder.Method<&Ui::pushOverlay>("pushOverlay");
    // ... one Method<> per surface entry ...
    builder.Constructor(); // the bound surface starts from constructible types (CollectEmittableTypes)
}

void RegisterUiScriptFacade()
{
    static const bool once = []()
    {
        GlobalTypeRegistry().Register(Ui::StaticType());
        foundation::script::RegisterExtraFacadeName(u8"Ui"); // the facade list (reserved-name guard)
        return true;
    }();
    (void)once;
}
```

### d. CMake

Add `Foundation::Script Foundation::Script.Facades` to the module's `target_link_libraries`
(Script.Facades for `RegisterExtraFacadeName`).

### e. Wire it into the run (app + instance)

- **Register the type** at app startup: call `RegisterUiScriptFacade()` where the domain
  registers its facades (its `DomainModule`'s `registerScriptFacade`).
- **Offer the binding**: per-run state on the run, app-wide state on the subsystem manager (see
  "Where a binding comes from" above). Store the binding as a *stable member* of its owner so it
  never dangles. Nothing is installed per script context.

## How the backends see it

`RegisterReflectedTypes(manager)` binds every emittable registered type (the shared
`CollectEmittableTypes` closure, seeded by the constructible types) into the backend. Then:

- **Luau**: each type is a global table holding `new` and its static methods:
  `Ui.pushOverlay(...)`. No import needed.
- **AngelScript**: statics become global functions in a namespace named after the class:
  `Ui::pushOverlay(...)`. No import needed; binding is by registry.

## Rules & gotchas (learned the hard way)

- **Reserved contract-class names: `Game` and `Level`.** These are names a user's own class
  *mandatorily* takes - the game orchestrator class is `Game` (`StartScript` does
  `CreateInstance("Game")`), and the scene-scripting tier's class is `Level` (instantiated once
  per scene from `SceneScriptSettings::script`). A facade sharing either name is a hard
  AngelScript error (`Name conflict. '<name>' is an extended data type`) - the user's class
  can't compile. This is why the level-load facade is `run` (the `Run` class's script name), not
  `Game`. `RegisterExtraFacadeName` **refuses** either name (logs an error, adds nothing) so the
  user's class always wins; the guard is covered by `script.facades: reserved contract-class
  names ...` in `Engine.GameInstance.Tests`. Avoid any other name a user's own class is likely
  to take.
- **Register the TYPE *and* the NAME.** `GlobalTypeRegistry().Register(X::StaticType())` binds
  the type. `RegisterExtraFacadeName(u8"X")` adds it to the facade list, which the reserved-name
  guard and the facade surface tests check (no backend reads the list to bind). Do both in the
  same `Register<X>ScriptFacade` (idempotent). The impl unit needs
  `import foundation.script.facades` + the `Foundation::Script.Facades` CMake link dep.
- **Every method must guard.** An unwired binding (bare cook VM, no host, or pre-install) must
  be a safe no-op. For a poll that gates a loop (`while (!complete) yield`), the unwired /
  unknown-ticket case returns **true** so the loop never hangs.
- **Facade numerics.** Method-path args narrow to the reflected C++ type, so write natural
  types: handles/tickets `i32`, progress/ratios `f64`. (A Luau global still reads back as `f64`,
  Lua's one number type, but method params do not.)
- **Null-scene safety.** A facade may be called from the game orchestrator's `launch()` before
  any scene exists (boot reorder, task #123). Guard anything scene-dependent
  (`currentScene == nullptr` -> safe default), and add a pre-scene battery.
- **Callbacks = `RefPtr<IScriptDelegate>`.** A script fn parameter is spelled
  `core::RefPtr<script::IScriptDelegate>`; both backends marshal a script function into one,
  and holding the RefPtr keeps it GC-alive. `Invoke(args)` calls back into script, marshalling
  against the handler's ACTUAL arity - so the native side fires with exactly the args the
  callback carries (a click fires with none), and a handler of any shape adapts. Handler
  authoring per backend:
  - **Luau**: a function of any arity - `Ui.onClick(h, "id", function() ... end)`.
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
  (`float`/`int`/`bool`), `string`, and enums are real value types and stay plain members. (See
  `Archive/game-ready-scripting-history.md`, Section 16.)
- **One binding type per facade.** Services are found by type, so two facades never share a
  binding type, and a run holding a type hides the application's copy for its scripts.
- **Editor-only facades** register from the editor `TypeDomain` (see the reflection track), so
  they surface only in the editor's script context.

## Testing (required)

Battery lives in the **owner's** test project, raw-context Net-style, on **BOTH** backends
(`Integration.ScriptFacades/NetFacadeScriptTests.cpp`, `Integration.ScriptFacades/
AudioFacadeScriptTests.cpp`, and the `run` cases in `Engine.GameInstance.Tests` are the
templates):

```cpp
RegisterCoreTypes();
ui::RegisterUiScriptFacade();
RefPtr<IScriptManager> mgr = angelscript::CreateScriptManager(DefaultAllocator());
RegisterReflectedTypes(*mgr);
RefPtr<IScriptContext> ctx = mgr->CreateContext();
UiFake fake;                                // fake records host calls
ScriptServices services;                    // a bare context: the test answers its lookups
services.Add(fake.binding);
ctx->SetServiceResolver(services.Resolver());
REQUIRE(ctx->Load(u8"...Ui::pushOverlay(...)...", u8"main").IsOk());
// assert fake recorded the calls; for callbacks, fire fake.clickFn->Invoke(...) and check.
```

Luau uses top-level statements; AngelScript uses a `void main()` (auto-run on Load) + globals
read back with `GetGlobal`. Cover: every method routes, numerics round-trip, an unwired
binding no-ops, and (if any) a delegate callback fires.
