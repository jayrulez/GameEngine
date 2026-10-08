// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.Facades - the `foundation.script.facades` module.
//
// The curated behavior facades: the per-entity
// `Entity` handle behaviors receive as their constructor argument, plus Log/Time/
// Random. A SEPARATE library (below the subsystem) so the COOK's harvest VM and the
// RUNTIME register the SAME "main"-module surface - a facade-using behavior that
// compiles at cook compiles at runtime and vice versa.
//
// Module-visibility rule (language-specific, so it lives in the BACKENDS): a backend
// whose reflected classes live in a separate module frames each behavior module with a
// prelude that imports the facade names. To keep that framing OUT of this neutral lib,
// the facade name list is exposed here as data (BehaviorFacadeNames) and each backend's
// AssembleBehaviorModuleSource builds its own prelude from it - so adding a facade never
// edits a backend.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module foundation.script.facades;

import foundation.core;
import foundation.scene;
import foundation.scene.resource; // PrefabSpawnSystem - what scene.spawn reaches
import foundation.script;
import foundation.resource; // ResourceManager - the run's resource-swap seam (Track A resource refs)

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::script
{
    namespace scene = foundation::scene;

    /// The service key the gameplay facades (Time/Random) resolve per context.
    inline constexpr StringView kScriptRuntimeService = u8"script.runtime";

    /// The reflected behavior-facade names a backend's behavior-module prelude must make
    /// visible (the classes RegisterScriptFacadeReflection installs). Exposed as DATA so
    /// the language framing stays in the backends: a backend builds its import line
    /// from this list, and adding a facade here reaches every backend for free. Order is
    /// the authored order. Kept in sync with RegisterScriptFacadeReflection below.
    [[nodiscard]] core::Span<const core::StringView> BehaviorFacadeNames();

    /// Register an ADDITIONAL facade name from an out-of-tree module (e.g. foundation.net's `Net`),
    /// so the behavior prelude imports it too - without this base lib depending on that module.
    /// Idempotent; call alongside the module's own reflection registration. Names are borrowed as
    /// interned copies. See ExtraFacadeNames.
    void RegisterExtraFacadeName(core::StringView name);
    /// The extra facade names registered by other modules (appended to the prelude after the built-ins).
    [[nodiscard]] core::Span<const core::StringView> ExtraFacadeNames();

    /// Register an ADDITIONAL emission root: a reflected type the collector should emit as a
    /// script class even when no facade signature statically reaches it (e.g. a component type
    /// returned only via a factory whose declared return IS that type - `RigidBody.of(entity)`).
    /// GENERAL, not component-specific (the UI View reflection follow-on wants the same door).
    /// AngelScript already emits every registry type, so this is consumed by the collector.
    /// Idempotent; the type is borrowed (its TypeInfo has static lifetime).
    void RegisterExtraScriptRootType(const core::TypeInfo* type);
    /// The additional emission roots registered by other modules (extra seeds for the reachability closure).
    [[nodiscard]] core::Span<const core::TypeInfo* const> ExtraScriptRootTypes();

    /// A reflected type worth binding to script: it has a constructor, property, or method (enums,
    /// containers, and pure-primitive types never qualify).
    [[nodiscard]] bool HasBindableSurface(const core::TypeInfo& type) noexcept;

    /// The REACHABILITY CLOSURE: the object types a script can actually receive a handle to, in
    /// emit order (seeds first). Seeds = every constructor-having type in `allTypes` + the registered
    /// ExtraScriptRootTypes (constructor-less factory returns like `RigidBody.of(entity)`), then
    /// closes over the reflected graph - each bound method's return/param types, each property's
    /// type, and each container element's base type plus its derived types. This bounds the emitted
    /// surface to what is reachable from the facades, NOT the whole registry. The ONE policy every
    /// binding backend uses, so their surfaces (and script_api) match.
    void CollectEmittableTypes(core::Span<const core::TypeInfo* const> allTypes,
                               core::Array<const core::TypeInfo*>& out);

    struct ScriptRuntimeBinding
    {
        f64 timeSeconds = 0.0;   // seconds since the run context was created
        f32 deltaSeconds = 0.0f; // last frame's dt
        core::Random random;     // the run's RNG (per-run determinism seam)

        // Behavior-to-behavior messaging: `entity.send("heal", amount)` routes here.
        // The subsystem installs this; it invokes `on<Heal>(amount)` on every behavior of
        // the target entity that declares the handler. Args are already marshalled. Null
        // when no subsystem is driving the run (a bare cook VM) - send becomes a no-op.
        core::Function<void(scene::Scene*, scene::EntityHandle, StringView,
                            core::Span<const core::Variant>)>
            dispatchMessage;

        // Resource swaps (Track A): the run's resource manager, for binding a resource id (a Guid)
        // onto a component's resource::Ref at runtime - e.g. `sceneRender.setMesh(entity, id)`.
        // LATE-BOUND (a getter, not a stored ptr) so it is correct regardless of the order the host
        // wires things: the composition root owns the manager (DefaultApplication::Resources()) and
        // installs this. Null / returns null on a bare cook VM => the swap sets the id only (unbound).
        core::Function<foundation::resource::ResourceManager*()> resolveResources;

        // The run this context's scripts belong to: the same opaque key the run's scenes carry
        // (Scene::Run), so a run-scoped service (the Audio facade's music and one-shots) plays
        // into the run's own group. Null = outside every run (an editor tool, a bare cook VM).
        const void* run = nullptr;
    };

    // ---- the curated behavior facades (camelCase = the script-visible names, the
    // Audio/Input facade precedent) ----

    struct Scene;        // bound scene handle (defined below); Entity.scene returns one
    struct SceneEvents;  // the scene event-bus handle (defined below); Scene.events returns one

    /// The per-entity handle behaviors receive as their constructor argument: transform
    /// get/set, name, destroy. A value type - the VM instance carries a copy; a stale
    /// handle (entity destroyed) turns every call into a safe no-op.
    struct Entity
    {
        scene::Scene* scene = nullptr;
        u32 entityIndex = scene::EntityHandle::kInvalidIndex;
        u32 entityGeneration = 0;

        [[nodiscard]] scene::EntityHandle Handle() const noexcept
        {
            return scene::EntityHandle{entityIndex, entityGeneration};
        }
        [[nodiscard]] bool Live() const noexcept
        {
            return scene != nullptr && scene->IsValid(Handle());
        }

        [[nodiscard]] bool isValid() const { return Live(); }
        /// The entity's OWN active flag.
        [[nodiscard]] bool active() const { return Live() && scene->IsActive(Handle()); }
        void setActive(bool value)
        {
            if (Live())
            {
                scene->SetActive(Handle(), value);
            }
        }
        /// EFFECTIVE state: own flag AND every ancestor's (what the runtime gates on).
        [[nodiscard]] bool activeInHierarchy() const
        {
            return Live() && scene->IsEffectivelyActive(Handle());
        }
        [[nodiscard]] String name() const
        {
            return Live() ? String(scene->GetEntityName(Handle())) : String{};
        }
        void setName(String value)
        {
            if (Live())
            {
                scene->SetEntityName(Handle(), value.AsView());
            }
        }
        [[nodiscard]] Float3 position() const
        {
            return Live() ? scene->GetLocalTransform(Handle()).position : Float3{0.0f, 0.0f, 0.0f};
        }
        void setPosition(f32 x, f32 y, f32 z)
        {
            if (Live())
            {
                scene->SetLocalPosition(Handle(), Float3{x, y, z});
            }
        }
        [[nodiscard]] Float3 worldPosition() const
        {
            if (!Live())
            {
                return Float3{0.0f, 0.0f, 0.0f};
            }
            return TransformPoint(Float3{0.0f, 0.0f, 0.0f}, scene->GetWorldMatrix(Handle()));
        }
        /// Absolute local rotation from Euler DEGREES (x = pitch, y = yaw, z = roll).
        void setRotationEuler(f32 xDegrees, f32 yDegrees, f32 zDegrees)
        {
            if (!Live())
            {
                return;
            }
            Transform transform = scene->GetLocalTransform(Handle());
            transform.rotation = FromYawPitchRoll(
                DegreesToRadians(yDegrees), DegreesToRadians(xDegrees), DegreesToRadians(zDegrees));
            scene->SetLocalTransform(Handle(), transform);
        }
        void setScale(f32 x, f32 y, f32 z)
        {
            if (!Live())
            {
                return;
            }
            Transform transform = scene->GetLocalTransform(Handle());
            transform.scale = Float3{x, y, z};
            scene->SetLocalTransform(Handle(), transform);
        }
        void destroy()
        {
            if (Live())
            {
                scene->DestroyEntity(Handle());
            }
        }

        // ---- as Sedulous's Entity: identity, the local transform by value, the hierarchy ----
        /// The entity's stable id (what an EntityRef stores); nil when stale.
        [[nodiscard]] Guid id() const { return Live() ? scene->GetEntityId(Handle()) : Guid{}; }
        void setPosition(Float3 position)
        {
            if (Live())
            {
                scene->SetLocalPosition(Handle(), position);
            }
        }
        [[nodiscard]] Quaternion rotation() const
        {
            return Live() ? scene->GetLocalTransform(Handle()).rotation : Quaternion{};
        }
        /// The rotation in the world: its own turn and every ancestor's (scale aside), as
        /// worldPosition is its place. Identity for a dead entity or a degenerate (zero-scale) one.
        [[nodiscard]] Quaternion worldRotation() const
        {
            Float3 translation;
            Quaternion world = Quaternion::Identity;
            Float3 scale;
            if (Live())
            {
                (void)Decompose(scene->GetWorldMatrix(Handle()), translation, world, scale);
            }
            return world;
        }
        /// The scale in the world: its own and every ancestor's, per axis (a basis axis's length).
        /// One for a dead entity or a degenerate (zero-scale) one. Under a rotated parent with an
        /// uneven scale the world shears, and an axis's length is all a scale can say of it.
        [[nodiscard]] Float3 worldScale() const
        {
            Float3 translation;
            Quaternion rotation;
            Float3 world = Float3::One;
            if (Live())
            {
                (void)Decompose(scene->GetWorldMatrix(Handle()), translation, rotation, world);
            }
            return world;
        }
        void setRotation(Quaternion rotation)
        {
            if (!Live())
            {
                return;
            }
            Transform transform = scene->GetLocalTransform(Handle());
            transform.rotation = rotation;
            scene->SetLocalTransform(Handle(), transform);
        }
        [[nodiscard]] Float3 scale() const
        {
            return Live() ? scene->GetLocalTransform(Handle()).scale : Float3{1.0f, 1.0f, 1.0f};
        }
        void setScale(Float3 scale)
        {
            if (!Live())
            {
                return;
            }
            Transform transform = scene->GetLocalTransform(Handle());
            transform.scale = scale;
            scene->SetLocalTransform(Handle(), transform);
        }
        [[nodiscard]] Transform localTransform() const
        {
            return Live() ? scene->GetLocalTransform(Handle()) : Transform{};
        }
        void setLocalTransform(Transform transform)
        {
            if (Live())
            {
                scene->SetLocalTransform(Handle(), transform);
            }
        }
        /// The parent, invalid at a root.
        [[nodiscard]] Entity parent() const;
        /// The first child, then each next sibling in order; invalid past the end.
        [[nodiscard]] Entity firstChild() const;
        [[nodiscard]] Entity nextSibling() const;
        /// The first direct child with this name, invalid if none.
        [[nodiscard]] Entity findChildByName(String name) const;
        /// Under `parent` (an invalid one makes it a root), last among its siblings; the local
        /// transform kept, or with `keepWorldTransform` the world one.
        void setParent(Entity parent) const;
        void setParent(Entity parent, bool keepWorldTransform) const;
        /// Just before `sibling`, under the sibling's parent.
        void moveBefore(Entity sibling) const;
        void moveBefore(Entity sibling, bool keepWorldTransform) const;

        /// The BOUND scene this entity belongs to (reflected as `.scene`). Operating through it
        /// (`entity.scene.spawn/find/...`) always targets THIS entity's scene - correct from any
        /// call site (update, onDestroy, a physics event, a stored callback), no ambient state.
        /// Named `sceneHandle()` in C++ to avoid clashing with the `scene` data member; the script
        /// name is `scene`. Defined out-of-line (Scene is completed below).
        [[nodiscard]] Scene sceneHandle() const;

        // ---- behavior messaging: entity.send(name[, payload]) invokes
        // `on<Name>(payload)` on EVERY behavior of this entity that declares it (the target
        // is this handle's entity - typically self or a resolved sibling). ONE conceptual method,
        // an ARITY FAMILY on the script surface: the event bus is StringHash + Variant underneath,
        // so the payload is a single Variant that carries any script value (number/string/entity/
        // ...) - the honest type, not the three a typed overload set would spell out.
        void send(String message) const { Dispatch(message.AsView(), {}); }
        void send(String message, Variant payload) const
        {
            Dispatch(message.AsView(), Span<const Variant>{&payload, 1});
        }

        void Dispatch(StringView message, Span<const Variant> args) const
        {
            if (!Live() || message.IsEmpty())
            {
                return;
            }
            IScriptContext* context = CurrentScriptContext();
            auto* binding =
                context != nullptr
                    ? static_cast<ScriptRuntimeBinding*>(context->GetService(kScriptRuntimeService))
                    : nullptr;
            if (binding != nullptr && binding->dispatchMessage)
            {
                binding->dispatchMessage(scene, Handle(), message, args);
            }
        }
    };

    /// Log.info/warn/error -> the engine log, Script category.
    class Log final : public Object
    {
        RTTI_OBJECT(Log, Object)
    public:
        static void info(String message) { LOG_INFO(u8"Script", u8"{}", message); }
        static void warn(String message) { LOG_WARNING(u8"Script", u8"{}", message); }
        static void error(String message) { LOG_ERROR(u8"Script", u8"{}", message); }
    };

    /// Time.now() (seconds since the run started) / Time.delta() (last frame dt).
    class Time final : public Object
    {
        RTTI_OBJECT(Time, Object)
    public:
        [[nodiscard]] static ScriptRuntimeBinding* Resolve()
        {
            IScriptContext* context = CurrentScriptContext();
            return context != nullptr ? static_cast<ScriptRuntimeBinding*>(
                                            context->GetService(kScriptRuntimeService))
                                      : nullptr;
        }
        [[nodiscard]] static f64 now()
        {
            ScriptRuntimeBinding* binding = Resolve();
            return binding != nullptr ? binding->timeSeconds : 0.0;
        }
        [[nodiscard]] static f32 delta()
        {
            ScriptRuntimeBinding* binding = Resolve();
            return binding != nullptr ? binding->deltaSeconds : 0.0f;
        }
    };

    /// Random.value() in [0,1) / Random.range(min,max) / Random.intRange(min,max).
    class Random final : public Object
    {
        RTTI_OBJECT(Random, Object)
    public:
        [[nodiscard]] static f32 value()
        {
            ScriptRuntimeBinding* binding = Time::Resolve();
            return binding != nullptr ? binding->random.NextFloat() : 0.0f;
        }
        [[nodiscard]] static f32 range(f32 min, f32 max)
        {
            ScriptRuntimeBinding* binding = Time::Resolve();
            return binding != nullptr ? binding->random.NextFloat(min, max) : min;
        }
        [[nodiscard]] static i32 intRange(i32 min, i32 max)
        {
            ScriptRuntimeBinding* binding = Time::Resolve();
            return (binding != nullptr && max >= min) ? binding->random.NextInt(min, max) : min;
        }
        /// Restarts the run's sequence from `seed`: the same seed, the same draws after it.
        static void seed(i64 seed)
        {
            if (ScriptRuntimeBinding* binding = Time::Resolve())
            {
                binding->random = core::Random(static_cast<u64>(seed));
            }
        }
        [[nodiscard]] static bool boolean()
        {
            ScriptRuntimeBinding* binding = Time::Resolve();
            return binding != nullptr && binding->random.NextBool();
        }
    };

    /// A BOUND scene handle. `scene.spawn(prefab,x,y,z)` / `scene.find(name)` /
    /// `scene.findByPath(path)` all operate on THIS scene (the ptr the value carries), so there is
    /// no ambient "current scene" to keep correct - a call from any site (update, onDestroy, a
    /// physics contact, a resumed coroutine, a stored callback) always targets the right scene.
    /// Behaviors reach it via `entity.scene`; the Level tier receives one as its constructor
    /// argument; the orchestrator queries `run.currentScene()`. A value type (the VM carries
    /// a copy); a null/stale scene makes every call a safe no-op returning an invalid Entity.
    struct Scene
    {
        scene::Scene* scene = nullptr;

        /// Instantiate a prefab into THIS scene at a position (the root's local position under
        /// the scene root); invalid Entity if the scene is null, has no PrefabSpawnSystem or the
        /// system has no content source (a bare cook VM), or the prefab id is nil / unknown. The
        /// id comes from an `asset:Prefab` behavior property (marshalled as a Guid). Reaches the
        /// scene's OWN spawn system - no ambient current-scene state, no host callback.
        [[nodiscard]] Entity spawn(Guid prefab, f32 x, f32 y, f32 z) const;
        /// First entity in THIS scene with this name (invalid if none).
        [[nodiscard]] Entity find(String name) const;
        /// Resolve a '/'-separated hierarchy path from THIS scene's roots, e.g.
        /// "Player/Weapon/Muzzle" (invalid if any segment misses).
        [[nodiscard]] Entity findByPath(String path) const;
        // ---- as Sedulous's Scene: entities made, ended and found by id; the scene's name ----
        [[nodiscard]] Entity createEntity() const;
        [[nodiscard]] Entity createEntity(String name) const;
        /// Ends the entity and everything under it (as entity.destroy()).
        void destroyEntity(Entity entity) const;
        /// The entity with this stable id (an EntityRef's), invalid if none.
        [[nodiscard]] Entity findEntity(Guid id) const;
        [[nodiscard]] String name() const
        {
            return scene != nullptr ? String(scene->Name()) : String{};
        }
        [[nodiscard]] u32 entityCount() const { return scene != nullptr ? scene->EntityCount() : 0u; }
        /// This scene's event-bus handle: `scene.events.emit(name, payload)`. A computed property
        /// (parens-less), so scripts write `scene.events.emit(...)` without call parens. Defined
        /// out-of-line (SceneEvents is completed below).
        [[nodiscard]] SceneEvents eventsHandle() const;
    };

    /// A BOUND scene event-bus handle: `scene.events.emit("OrbCollected", 1)`. Publishes a NAMED
    /// event onto THIS scene's native EventBus (deferred; delivered at the scene tick's top level).
    /// A C++ system Subscribe()s natively; a script behavior or the Level receives it as
    /// `on<Name>(payload)` through the engine's script event bridge. emit is overloaded by payload
    /// type - one reflected name, resolved by arg type, exactly like `entity.send`. A value type
    /// (the VM carries a copy); a null/stale scene makes emit a safe no-op. Grouping event ops under
    /// `.events` leaves room for `subscribe`/`unsubscribe` to join it.
    struct SceneEvents
    {
        scene::Scene* scene = nullptr;

        // ONE conceptual method, an ARITY FAMILY on the script surface: the bus is StringHash +
        // Variant underneath, so the payload is a single Variant carrying any script value.
        void emit(String name) const;
        void emit(String name, Variant payload) const;
    };

    /// The scene's prefab spawning (Sedulous's `scene.Prefabs`): `ScenePrefabs.of(scene)`. An
    /// authored prefab instantiated by asset id, its root at `position` (and `rotation`) under
    /// `parent` (none: a scene root), in the parent's space. Invalid Entity when the scene has no
    /// spawn system or content source, or the prefab is nil / unknown - never a partial spawn. A
    /// value type; a null scene makes every call a safe no-op.
    struct ScenePrefabs
    {
        scene::Scene* scene = nullptr;

        [[nodiscard]] Entity spawn(Guid prefab, Float3 position) const;
        [[nodiscard]] Entity spawn(Guid prefab, Float3 position, Quaternion rotation) const;
        [[nodiscard]] Entity spawn(Guid prefab, Float3 position, Quaternion rotation,
                                   Entity parent) const;

        [[nodiscard]] static ScenePrefabs of(Scene sceneHandle)
        {
            ScenePrefabs prefabs;
            prefabs.scene = sceneHandle.scene;
            return prefabs;
        }
    };

    /// Wrap a (scene, handle) pair into an Entity value (invalid handle -> invalid Entity).
    [[nodiscard]] inline Entity WrapEntity(scene::Scene* scene, scene::EntityHandle handle)
    {
        Entity result;
        if (scene != nullptr && handle.IsAssigned())
        {
            result.scene = scene;
            result.entityIndex = handle.index;
            result.entityGeneration = handle.generation;
        }
        return result;
    }

    inline Entity Entity::parent() const
    {
        return Live() ? WrapEntity(scene, scene->GetParent(Handle())) : Entity{};
    }
    inline Entity Entity::firstChild() const
    {
        return Live() ? WrapEntity(scene, scene->GetFirstChild(Handle())) : Entity{};
    }
    inline Entity Entity::nextSibling() const
    {
        return Live() ? WrapEntity(scene, scene->GetNextSibling(Handle())) : Entity{};
    }
    inline Entity Entity::findChildByName(String name) const
    {
        return Live() ? WrapEntity(scene, scene->FindChildByName(Handle(), name.AsView())) : Entity{};
    }
    inline void Entity::setParent(Entity parent) const { setParent(parent, false); }
    inline void Entity::setParent(Entity parent, bool keepWorldTransform) const
    {
        // Only within this entity's own scene: a parent from another scene reads as none.
        if (Live())
        {
            const scene::EntityHandle under =
                (parent.scene == scene && parent.Live()) ? parent.Handle() : scene::EntityHandle{};
            scene->SetParent(Handle(), under, keepWorldTransform);
        }
    }
    inline void Entity::moveBefore(Entity sibling) const { moveBefore(sibling, false); }
    inline void Entity::moveBefore(Entity sibling, bool keepWorldTransform) const
    {
        if (Live() && sibling.scene == scene && sibling.Live())
        {
            scene->MoveBefore(Handle(), sibling.Handle(), keepWorldTransform);
        }
    }

    inline Scene Entity::sceneHandle() const
    {
        Scene handle;
        handle.scene = scene; // the entity's own scene - never ambient
        return handle;
    }

    inline Entity Scene::spawn(Guid prefab, f32 x, f32 y, f32 z) const
    {
        if (scene == nullptr || prefab.IsNil())
        {
            return Entity{};
        }
        scene::PrefabSpawnSystem* spawner = scene->GetSystem<scene::PrefabSpawnSystem>();
        if (spawner == nullptr)
        {
            return Entity{};
        }
        return WrapEntity(scene, spawner->Spawn(prefab, Float3{x, y, z}));
    }

    inline Entity ScenePrefabs::spawn(Guid prefab, Float3 position) const
    {
        return spawn(prefab, position, Quaternion::Identity, Entity{});
    }
    inline Entity ScenePrefabs::spawn(Guid prefab, Float3 position, Quaternion rotation) const
    {
        return spawn(prefab, position, rotation, Entity{});
    }
    inline Entity ScenePrefabs::spawn(Guid prefab, Float3 position, Quaternion rotation,
                                      Entity parent) const
    {
        if (scene == nullptr || prefab.IsNil())
        {
            return Entity{};
        }
        scene::PrefabSpawnSystem* spawner = scene->GetSystem<scene::PrefabSpawnSystem>();
        if (spawner == nullptr)
        {
            return Entity{};
        }
        // A parent from another scene (or a stale one) reads as none: the spawn lands at a root.
        const scene::EntityHandle under =
            (parent.scene == scene && parent.Live()) ? parent.Handle() : scene::EntityHandle::Invalid();
        return WrapEntity(scene, spawner->Spawn(prefab, position, rotation, under));
    }

    inline Entity Scene::createEntity() const { return createEntity(String{}); }
    inline Entity Scene::createEntity(String name) const
    {
        return scene != nullptr ? WrapEntity(scene, scene->CreateEntity(name.AsView())) : Entity{};
    }
    inline void Scene::destroyEntity(Entity entity) const
    {
        if (entity.scene == scene)
        {
            entity.destroy();
        }
    }
    inline Entity Scene::findEntity(Guid id) const
    {
        return (scene != nullptr && !id.IsNil()) ? WrapEntity(scene, scene->FindEntity(id)) : Entity{};
    }

    inline Entity Scene::find(String name) const
    {
        return (scene != nullptr) ? WrapEntity(scene, scene->FindEntityByName(name.AsView()))
                                  : Entity{};
    }

    inline Entity Scene::findByPath(String path) const
    {
        return (scene != nullptr) ? WrapEntity(scene, scene->FindEntityByPath(path.AsView()))
                                  : Entity{};
    }

    inline SceneEvents Scene::eventsHandle() const
    {
        SceneEvents handle;
        handle.scene = scene; // this scene's bus - never ambient
        return handle;
    }

    // emit publishes straight onto the scene's native EventBus. Both foundation.script.facades and
    // foundation.scene are Foundation, so no ScriptRuntimeBinding hook is needed (unlike spawn, which
    // reaches an Engine-owned content DB): the call is a direct, host-independent Publish. Delivery
    // to scripts is the engine's job (the script event bridge drains the bus and dispatches
    // on<Name>). A null/empty name or null scene is a safe no-op.
    inline void SceneEvents::emit(String name) const
    {
        if (scene != nullptr && !name.IsEmpty())
        {
            if (messaging::EventBus* bus = scene->Events())
            {
                bus->Publish(StringHash(name.AsView()), Variant{});
            }
        }
    }
    inline void SceneEvents::emit(String name, Variant payload) const
    {
        // Already a Variant (the backend boxed whatever the script passed); publish it verbatim.
        if (scene != nullptr && !name.IsEmpty())
        {
            if (messaging::EventBus* bus = scene->Events())
            {
                bus->Publish(StringHash(name.AsView()), Move(payload));
            }
        }
    }

    /// The `Component.of(entity)` factory body (OPTION 1, spec Section 12): a re-resolving handle
    /// of component type T for `entity`, as a RESOLVE-mode Variant. Reflect it on T with the
    /// ReturnType-override so the declared script return IS T:
    ///     builder.Method<&foundation::script::ComponentOf<T>, T>("of");
    /// GENERIC - every component-owning module reuses this one body; empty (a clean null in script)
    /// when the entity's scene has no manager for T. The type must also be registered
    /// (GlobalTypeRegistry) + seeded as an emission root + given an extra facade name.
    template <typename T>
    [[nodiscard]] Variant ComponentOf(Entity entity)
    {
        return (entity.scene != nullptr)
                   ? entity.scene->MakeComponentRef(entity.Handle(), TypeOf<T>())
                   : Variant{};
    }

    /// The current run's resource manager (via the run binding's resolveResources getter), or null
    /// on a bare cook VM / when no host wired it. The one place Track A resource-swap ops (a
    /// SceneRender.setMesh, an AudioSource.setClip, ...) reach the manager to Bind a resource id
    /// onto a component's resource::Ref. No ambient scene state - the binding is per-run.
    [[nodiscard]] inline foundation::resource::ResourceManager* CurrentRunResources()
    {
        IScriptContext* context = CurrentScriptContext();
        auto* binding =
            context != nullptr
                ? static_cast<ScriptRuntimeBinding*>(context->GetService(kScriptRuntimeService))
                : nullptr;
        return (binding != nullptr && binding->resolveResources) ? binding->resolveResources()
                                                                 : nullptr;
    }

    /// The run the current script context belongs to (ScriptRuntimeBinding::run), or null.
    [[nodiscard]] inline const void* CurrentRun()
    {
        IScriptContext* context = CurrentScriptContext();
        auto* binding =
            context != nullptr
                ? static_cast<ScriptRuntimeBinding*>(context->GetService(kScriptRuntimeService))
                : nullptr;
        return binding != nullptr ? binding->run : nullptr;
    }

    /// Registers the behavior facade types (Entity/Log/Time/Random/Scene) with the
    /// registry - call BEFORE a script manager is created (the run host and the cook's
    /// builder both do). Idempotent.
    void RegisterScriptFacadeReflection();
}
