// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.script tests: the behaviours core, headless (a real script VM and a real Scene, no
// device). Covers lifecycle dispatch (deferred start, onUpdate(dt), enable and disable edges,
// onDestroy on entity destroy and on scene stop), defaults and hash-keyed overrides, a fault
// disabling only its own behaviour, hot reload (the product swaps, instances are re-created,
// overrides re-applied, transient state reset), the entity facade (with Log, Time and Random),
// component serialization symmetry through SerializeScene, and the prefab-override round trip
// (the component payload carries it with no extra code).

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // REFLECT_VALUE (the Gadget test component)
#include <initializer_list>
#include <cstdio>
#include <filesystem>

import foundation.core;
import foundation.vfs; // the data mount the subsystem reads engine data through
import foundation.runtime;
import foundation.scene;
import foundation.scene.resource;
import foundation.content; // ContentDatabase: a prefab for scene.spawn to reach
import engine.scene;
import foundation.resource;
import foundation.script;
import foundation.script.facades; // script::Entity, RegisterExtraScriptRootType/FacadeName
import foundation.script.angelscript;
import foundation.script.luau;
import foundation.script.resource;
import engine.script;
import foundation.physics;
import engine.physics;
import engine.integration; // ScriptPhysicsContactBridge (the composition-root adapter)
import engine.render;      // MeshComponent/LightComponent .of (render surface)
import foundation.render;             // debug::DebugDraw accessors (the DebugDraw real-effect test)
import foundation.rhi;                // rhi::Device (base of the headless NullDevice)
import foundation.rhi.null;           // NullDevice (headless RenderSubsystem, no GPU)
import foundation.audio;              // AudioSourceComponent + its manager (audio surface)
import engine.audio;       // AudioSourceComponent.of + SceneAudio (audio surface)
import foundation.animation;          // animation components + managers (animation surface)
import engine.animation;   // *.of + SceneAnimation (animation surface)
import foundation.particles;          // ParticleEffectComponent + manager (particle surface)
import engine.particles;   // *.of + SceneParticles (particle surface)
import foundation.net.replication;    // NetworkComponent + manager + .of (net surface)
import foundation.net;                // BitWriter / BitReader (a delta carried by hand)
import foundation.spline;             // SplinePoint (the path follow surface)
import engine.spline;      // PathFollowComponent.of (path follow surface)

using namespace foundation::core;
using namespace engine::script;
using namespace engine::scene;
using namespace engine::render;
using namespace engine::physics;
using namespace engine::particles;
using namespace engine::integration;
using namespace engine::audio;
using namespace engine::animation;
using namespace foundation::script;
namespace scene = foundation::scene;
namespace physics = foundation::physics;
namespace anim = foundation::animation;

// ---- A component reached only through a per-type `Gadget.of(entity)` factory whose declared
// return is the component type. Covers the whole script-facing pipeline: an extra emission root
// gives an emitted script class, the ReturnType-overridden `of` gives a resolving Variant of the
// component, and a property set through the re-resolving handle mutates the live component. ----
namespace
{
    struct Gadget
    {
        f32 power = 1.0f;
    };
    class GadgetManager final : public scene::ComponentManager<Gadget>
    {
    };
}
REFLECT_VALUE(Gadget, "rtti::engine::script::test")
{
    builder.Property<&Gadget::power>("power");
    // The generic factory: Gadget.of(entity) returns a resolving handle of type Gadget.
    builder.Method<&foundation::script::ComponentOf<Gadget>, Gadget>("of");
}
namespace
{
    // Registers the component once (idempotent). It sits after the reflect macro so
    // RttiRegisterValue_Gadget is declared. Runs its reflect builder (patching TypeOf<Gadget>),
    // puts it in the registry (both backends emit it), seeds it as an extra emission root (so
    // scripts can reach it), and makes its class name visible to behaviour preludes.
    void EnsureGadgetRegistered()
    {
        static const bool once = []()
        {
            RttiRegisterValue_Gadget();
            GlobalTypeRegistry().Register(TypeOf<Gadget>());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<Gadget>());
            foundation::script::RegisterExtraFacadeName(u8"Gadget");
            return true;
        }();
        (void)once;
    }
}

namespace
{
    // A cooked-product stand-in: samples and tests build ScriptClass products directly (the
    // factory test covers the cooked path; here the runtime is under test).
    [[nodiscard]] RefPtr<ScriptClass>
    MakeClass(StringView className, StringView source, std::initializer_list<StringView> handlers,
              std::initializer_list<ScriptPropertyDesc> properties = {})
    {
        RefPtr<ScriptClass> cls = MakeRef<ScriptClass>(DefaultAllocator());
        cls->language = String(u8"angelscript");
        cls->className = String(className);
        cls->source = String(source);
        for (StringView handler : handlers)
        {
            cls->handlers.PushBack(String(handler));
        }
        for (const ScriptPropertyDesc& property : properties)
        {
            cls->properties.PushBack(property);
        }
        cls->BuildProfileName();
        return cls;
    }

    // Same, but for an arbitrary backend language (the AngelScript behavior path).
    [[nodiscard]] RefPtr<ScriptClass>
    MakeClassLang(StringView language, StringView className, StringView source,
                  std::initializer_list<StringView> handlers,
                  std::initializer_list<ScriptPropertyDesc> properties = {})
    {
        RefPtr<ScriptClass> cls = MakeRef<ScriptClass>(DefaultAllocator());
        cls->language = String(language);
        cls->className = String(className);
        cls->source = String(source);
        for (StringView handler : handlers)
        {
            cls->handlers.PushBack(String(handler));
        }
        for (const ScriptPropertyDesc& property : properties)
        {
            cls->properties.PushBack(property);
        }
        cls->BuildProfileName();
        return cls;
    }

    [[nodiscard]] ScriptPropertyDesc FloatProperty(StringView name, f64 defaultValue)
    {
        ScriptPropertyDesc desc;
        desc.name = String(name);
        desc.hash = ScriptPropertyNameHash(name);
        desc.type = ScriptPropertyType::Float;
        desc.defaultValue.kind = ScriptPropertyType::Float;
        desc.defaultValue.number = defaultValue;
        return desc;
    }

    [[nodiscard]] ScriptPropertyDesc EntityProperty(StringView name)
    {
        ScriptPropertyDesc desc;
        desc.name = String(name);
        desc.hash = ScriptPropertyNameHash(name);
        desc.type = ScriptPropertyType::Entity;
        desc.defaultValue.kind = ScriptPropertyType::Entity;
        return desc;
    }

    struct ScriptedScene
    {
        // The test fixture is the run scope (scenes hold only a borrowed bus): it owns the bus,
        // injects it before systems bind, and drains it at frame top as GameInstance and the
        // editor page do.
        foundation::messaging::EventBus bus;
        scene::Scene scene{DefaultAllocator(), u8"script-test"};
        ScriptRunHost host{DefaultAllocator()};
        ScriptComponentManager* components = nullptr;
        ScriptSceneSystem* scripts = nullptr;

        ScriptedScene()
        {
            // Script-error logs reach the test output (silent otherwise).
            static bool logReady = []()
            {
                static ConsoleSink sink;
                GlobalLogger().AddSink(&sink);
                return true;
            }();
            (void)logReady;
            foundation::script::angelscript::RegisterAngelScriptBackend();
            foundation::script::RegisterLuauScriptBackend();
            RegisterCoreTypes();
            RegisterScriptComponentReflection();
            RegisterScriptFacadeReflection();
            scene.SetEventBus(&bus); // before AddSystem: the bridge binds at OnSceneCreate
            components = scene.AddSystem<ScriptComponentManager>();
            scripts = scene.AddSystem<ScriptSceneSystem>();
            components->SetScriptSystem(scripts);
            scripts->SetRunHost(&host);
            // Wire the single-scene message route (the ScriptSubsystem installs a
            // scene-multiplexer in a real run; here one system owns every entity).
            ScriptSceneSystem* system = scripts;
            host.Binding().dispatchMessage =
                Function<void(scene::Scene*, scene::EntityHandle, StringView, Span<const Variant>)>{
                    [system](scene::Scene*, scene::EntityHandle target, StringView message,
                             Span<const Variant> args)
                    { system->EnqueueMessage(target, message, args); }};
        }

        scene::EntityHandle AddScripted(const RefPtr<ScriptClass>& cls, StringView name)
        {
            scene::EntityHandle e = scene.CreateEntity(name);
            ScriptComponent& c = components->Add(e);
            ScriptBehavior behavior;
            behavior.script = cls;
            c.behaviors.PushBack(Move(behavior));
            return e;
        }

        void Start()
        {
            scene.Start();
            scene.SetSimulationEnabled(true);
        }

        void Frame(f32 deltaTime = 0.5f)
        {
            host.Binding().timeSeconds += static_cast<f64>(deltaTime);
            host.Binding().deltaSeconds = deltaTime;
            bus.Drain(); // the scope drains at frame top (events emitted last frame arrive now)
            scene.Update(deltaTime);
        }
    };

    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-4f; }

    // What scene.spawn reaches: the scene's PrefabSpawnSystem pointed at a content database holding
    // one prefab, Turret with a Barrel child. The scene's scripts spawn it by id; the assertions read
    // the scene (whether a Turret root exists, where, and with its child) rather than a callback.
    struct PrefabSource
    {
        std::filesystem::path dir = "scratch_script_prefabs";
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<foundation::content::ContentDatabase> db;
        Guid turret;

        explicit PrefabSource(ScriptedScene& bed)
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(
                DefaultAllocator(), u8"scratch_script_prefabs", DefaultAllocator());
            db = MakeUnique<foundation::content::ContentDatabase>(
                DefaultAllocator(), DefaultAllocator(), *mount, BinarySerializerFactory(), u8".rasset");
            scene::Scene author(DefaultAllocator(), u8"author");
            const scene::EntityHandle root = author.CreateEntity(u8"Turret");
            author.SetParent(author.CreateEntity(u8"Barrel"), root);
            MemoryStream payload;
            REQUIRE(scene::CapturePrefab(author, root, payload).IsOk());
            foundation::content::Instance* prefab =
                db->RootGroup()->CreateInstance(u8"Turret", scene::PrefabDocument::StaticType());
            REQUIRE(prefab != nullptr);
            REQUIRE(prefab->WriteData(u8"scene", payload.Bytes()).IsOk());
            turret = prefab->Id();
            bed.scene.AddSystem<scene::PrefabSpawnSystem>()->SetSource(db.Get(), nullptr);
        }
        ~PrefabSource()
        {
            db.Reset();
            mount.Reset();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        // The prefab's id as a behavior Asset property (the way a cooked script carries one).
        [[nodiscard]] ScriptPropertyDesc Property() const
        {
            ScriptPropertyDesc prefabProp;
            prefabProp.name = String(u8"prefab");
            prefabProp.hash = ScriptPropertyNameHash(u8"prefab");
            prefabProp.type = ScriptPropertyType::Asset;
            prefabProp.assetType = String(u8"Prefab");
            prefabProp.defaultValue.kind = ScriptPropertyType::Asset;
            prefabProp.defaultValue.guid = turret;
            return prefabProp;
        }
        // The canonical 36-char spelling (what Guid.new / Guid(text) parse).
        [[nodiscard]] String Canonical() const
        {
            char text[40];
            std::snprintf(text, sizeof text, "%08llx-%04llx-%04llx-%04llx-%012llx",
                          static_cast<unsigned long long>(turret.high >> 32),
                          static_cast<unsigned long long>((turret.high >> 16) & 0xFFFFu),
                          static_cast<unsigned long long>(turret.high & 0xFFFFu),
                          static_cast<unsigned long long>(turret.low >> 48),
                          static_cast<unsigned long long>(turret.low & 0xFFFFFFFFFFFFull));
            return String(StringView(reinterpret_cast<const utf8char*>(text)));
        }
        // How many Turret instances the scene holds (each spawn is one root named Turret).
        [[nodiscard]] static u32 Spawned(scene::Scene& in)
        {
            u32 count = 0;
            for (scene::EntityHandle root = in.GetFirstRoot();
                 root.IsAssigned(); root = in.GetNextSibling(root))
            {
                if (in.GetEntityName(root) == StringView(u8"Turret") ||
                    in.GetEntityName(root) == StringView(u8"child"))
                {
                    ++count;
                }
            }
            return count;
        }
    };
}

TEST_CASE("script.scene: lifecycle - onStart once (deferred to the first simulated "
          "tick), onUpdate(dt) with real dt, gated to simulation")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    private Entity@ self;\n"
                  u8"    float speed;\n"
                  u8"    Mover(Entity@ entity) {\n"
                  u8"        @self = entity;\n"
                  u8"        speed = 2.0f;\n"
                  u8"        self.setName(\"constructed\");\n"
                  u8"    }\n"
                  u8"    void onStart() { self.setName(\"started\"); }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Float3 p = self.position();\n"
                  u8"        self.setPosition(p.x + speed * float(dt), p.y, p.z);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate"}, {FloatProperty(u8"speed", 2.0)});

    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");

    // Not simulating yet: nothing instantiates, nothing runs.
    bed.scene.Start();
    bed.scene.SetSimulationEnabled(false);
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"walker"));
    CHECK_FALSE(bed.host.IsActive());

    bed.scene.SetSimulationEnabled(true);
    bed.Frame(); // instantiate, onStart and the first onUpdate, all this tick
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // 2.0 * 0.5
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    CHECK(bed.host.IsActive());
    CHECK(bed.scripts->InstanceCount() == 1u);
}

TEST_CASE("script.scene: entity-active - starts-inactive never instantiates; activation "
          "delivers onStart on the first ACTIVE tick")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Mover(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() { self.setName(\"started\"); }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Float3 p = self.position();\n"
                  u8"        self.setPosition(p.x + float(dt), p.y, p.z);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate"}, {});

    // Inactive through the parent (the hierarchy path), set before Start: the scene starts
    // with the entity inactive.
    const scene::EntityHandle parent = bed.scene.CreateEntity(u8"holder");
    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");
    bed.scene.SetParent(e, parent);
    bed.scene.SetActive(parent, false);

    bed.Start();
    bed.Frame();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"walker")); // no construct, no onStart
    CHECK(bed.scripts->InstanceCount() == 0u);                   // never instantiated
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));

    bed.scene.SetActive(parent, true);
    bed.Frame(); // first active tick: instantiate, onStart, onUpdate
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(bed.scripts->InstanceCount() == 1u);
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.5f));
}

TEST_CASE("script.scene: entity-active - deactivation freezes updates with NO lifecycle "
          "events; reactivation resumes without a second onStart")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Mover(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() { self.setName(\"started\"); }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Float3 p = self.position();\n"
                  u8"        self.setPosition(p.x + float(dt), p.y, p.z);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate"}, {});
    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");
    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.5f));

    bed.scene.SetActive(e, false);
    bed.scene.SetEntityName(e, u8"mid"); // a re-fired onStart would overwrite this
    bed.Frame();
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.5f)); // frozen
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"mid"));     // and silent (no events)

    bed.scene.SetActive(e, true);
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // resumed
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"mid"));     // onStart did not fire again
    CHECK(bed.scripts->InstanceCount() == 1u);
}

TEST_CASE("script.scene: the entity facade exposes active/setActive/activeInHierarchy")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> toggler =
        MakeClass(u8"Toggler",
                  u8"class Toggler {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Toggler(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        if (self.active() && self.activeInHierarchy()) {\n"
                  u8"            self.setName(\"live\");\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Float3 p = self.position();\n"
                  u8"        self.setPosition(p.x + 1.0f, p.y, p.z);\n"
                  u8"        self.setActive(false);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate"}, {});
    const scene::EntityHandle e = bed.AddScripted(toggler, u8"walker");
    bed.Start();

    bed.Frame(); // onStart saw active and activeInHierarchy; onUpdate moved, then deactivated itself
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"live"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK_FALSE(bed.scene.IsActive(e)); // setActive(false) reached the scene

    bed.Frame(); // now frozen by its own deactivation
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    bed.scene.SetActive(e, true); // wake it: one more step, then it self-deactivates again
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    CHECK_FALSE(bed.scene.IsActive(e));
}

TEST_CASE("script.scene: LUAU behavior lifecycle + property field-apply through the run host")
{
    ScriptedScene bed;
    // The Luau idiom: a global class table, colon methods, plain instance fields for properties.
    // The run host resolves the Luau backend from the class's language and dispatches the same
    // onStart/onUpdate; ApplyProperties writes the `speed` field through the field setter.
    RefPtr<ScriptClass> mover = MakeClassLang(
        u8"luau", u8"Mover",
        u8"Mover = {}\n"
        u8"Mover.__index = Mover\n"
        u8"function Mover.new(entity)\n"
        u8"    local self = setmetatable({ entity = entity, speed = 2.0 }, Mover)\n"
        u8"    entity:setName(\"constructed\")\n"
        u8"    return self\n"
        u8"end\n"
        u8"function Mover:onStart() self.entity:setName(\"started\") end\n"
        u8"function Mover:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
        u8"end\n",
        {u8"onStart", u8"onUpdate"}, {FloatProperty(u8"speed", 2.0)});

    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate, apply speed=2.0, onStart, first onUpdate
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // 2.0 * 0.5
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    CHECK(bed.scripts->InstanceCount() == 1u);
}

TEST_CASE("script.scene: LUAU hash-keyed property override wins over the harvested default")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover = MakeClassLang(
        u8"luau", u8"Mover",
        u8"Mover = {}\n"
        u8"Mover.__index = Mover\n"
        u8"function Mover.new(entity)\n"
        u8"    return setmetatable({ entity = entity, speed = 0.0 }, Mover)\n"
        u8"end\n"
        u8"function Mover:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
        u8"end\n",
        {u8"onUpdate"}, {FloatProperty(u8"speed", 2.0)});

    // Override speed to 10.0 (the field setter applies it after construction, over the default).
    const scene::EntityHandle e = bed.AddScripted(mover, u8"fast");
    {
        ScriptComponent* c = bed.components->Get(e);
        ScriptPropertyValue ten;
        ten.kind = ScriptPropertyType::Float;
        ten.number = 10.0;
        c->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"speed"), ten);
    }

    bed.Start();
    bed.Frame(); // speed = 10 (override), moved 10 * 0.5 = 5
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
}

TEST_CASE("script.scene: LUAU entity.send invokes on<Message>(arg) on the target behavior")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> receiver = MakeClassLang(
        u8"luau", u8"Receiver",
        u8"Receiver = {}\n"
        u8"Receiver.__index = Receiver\n"
        u8"function Receiver.new(entity) return setmetatable({ entity = entity }, Receiver) end\n"
        u8"function Receiver:onPing(amount) self.entity:setName(\"pinged:\" .. tostring(amount)) end\n",
        {u8"onPing"});
    RefPtr<ScriptClass> sender = MakeClassLang(
        u8"luau", u8"Sender",
        u8"Sender = {}\n"
        u8"Sender.__index = Sender\n"
        u8"function Sender.new(entity) return setmetatable({ entity = entity }, Sender) end\n"
        u8"function Sender:onStart() self.entity:send(\"ping\", 7) end\n",
        {u8"onStart"});

    scene::EntityHandle target = bed.scene.CreateEntity(u8"target");
    ScriptComponent& component = bed.components->Add(target);
    {
        ScriptBehavior b;
        b.script = receiver;
        component.behaviors.PushBack(Move(b));
    }
    {
        ScriptBehavior b;
        b.script = sender;
        component.behaviors.PushBack(Move(b));
    }

    bed.Start();
    bed.Frame(); // both instantiate; sender.onStart sends "ping" 7, receiver.onPing(7) runs
    CHECK(bed.scene.GetEntityName(target) == StringView(u8"pinged:7"));
}

TEST_CASE("script.scene: LUAU updateInterval throttles onUpdate and delivers accumulated dt")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ticker = MakeClassLang(
        u8"luau", u8"Ticker",
        u8"Ticker = {}\n"
        u8"Ticker.__index = Ticker\n"
        u8"function Ticker.new(entity) return setmetatable({ entity = entity }, Ticker) end\n"
        u8"function Ticker:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + 1.0, dt, p.z)\n" // x counts calls; y = last dt
        u8"end\n",
        {u8"onUpdate"});
    const scene::EntityHandle e = bed.AddScripted(ticker, u8"ticker");
    bed.components->Get(e)->behaviors[0].updateInterval = 1.0f;

    bed.Start();
    bed.Frame(0.5f); // acc 0.5 < 1.0 -> no update
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // acc 1.0 -> one update, dt = accumulated 1.0
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.y, 1.0f)); // accumulated dt, not 0.5
}

TEST_CASE("script.scene: LUAU coroutine waitSeconds(1.0) runs its body only after ~1s of ticks")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> waiter = MakeClassLang(
        u8"luau", u8"Waiter",
        u8"Waiter = {}\n"
        u8"Waiter.__index = Waiter\n"
        u8"function Waiter.new(entity) return setmetatable({ entity = entity }, Waiter) end\n"
        u8"function Waiter:onStart()\n"
        u8"    local this = self\n"
        u8"    startCoroutine(function()\n"
        u8"        waitSeconds(1.0)\n"
        u8"        this.entity:setPosition(5, 0, 0)\n"
        u8"    end, self)\n"
        u8"end\n",
        {u8"onStart"});
    waiter->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(waiter, u8"w");
    bed.Start();
    bed.Frame(0.5f); // onStart starts waitSeconds 1.0; +0.5s -> still pending
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // +0.5s reaches 1.0s; the resume sets the position
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
    bed.Frame(0.5f); // completed coroutine does not run again
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
}

TEST_CASE("script.scene: harvested defaults apply; hash-keyed overrides win")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    private Entity@ self;\n"
                  u8"    float speed;\n"
                  u8"    Mover(Entity@ entity) {\n"
                  u8"        @self = entity;\n"
                  u8"        speed = 0.0f;\n"
                  u8"    }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Float3 p = self.position();\n"
                  u8"        self.setPosition(p.x + speed * float(dt), p.y, p.z);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"}, {FloatProperty(u8"speed", 2.0)});

    const scene::EntityHandle defaulted = bed.AddScripted(mover, u8"defaulted");
    const scene::EntityHandle overridden = bed.AddScripted(mover, u8"overridden");
    {
        ScriptComponent* c = bed.components->Get(overridden);
        ScriptPropertyValue ten;
        ten.kind = ScriptPropertyType::Float;
        ten.number = 10.0;
        c->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"speed"), ten);
    }

    bed.Start();
    bed.Frame();                                                           // dt 0.5
    CHECK(Near(bed.scene.GetLocalTransform(defaulted).position.x, 1.0f));  // default 2
    CHECK(Near(bed.scene.GetLocalTransform(overridden).position.x, 5.0f)); // override 10
}

TEST_CASE("script.scene: enable/disable edges dispatch onEnable/onDisable; disabled "
          "behaviors do not tick")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> toggler =
        MakeClass(u8"Toggler",
                  u8"class Toggler {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Toggler(Entity@ entity) { @self = entity; }\n"
                  u8"    void onEnable() { self.setName(self.name() + \"+on\"); }\n"
                  u8"    void onDisable() { self.setName(self.name() + \"+off\"); }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Float3 p = self.position();\n"
                  u8"        self.setPosition(p.x + 1.0f, p.y, p.z);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onEnable", u8"onDisable", u8"onUpdate"});

    const scene::EntityHandle e = bed.AddScripted(toggler, u8"t");
    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    bed.components->Get(e)->behaviors[0].enabled = false;
    bed.Frame(); // delivers onDisable, skips onUpdate
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on+off"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    bed.Frame(); // stays off
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    bed.components->Get(e)->behaviors[0].enabled = true;
    bed.Frame(); // onEnable again, and ticks
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on+off+on"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
}

TEST_CASE("script.scene: onDestroy fires on entity destroy AND on scene stop; stop "
          "releases every instance")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> counter =
        MakeClass(u8"Counter",
                  u8"double DestroyCount = 0;\n"
                  u8"class Counter {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Counter(Entity@ entity) { @self = entity; }\n"
                  u8"    void onUpdate(double dt) {}\n"
                  u8"    void onDestroy() { DestroyCount = DestroyCount + 1; }\n"
                  u8"}\n",
                  {u8"onUpdate", u8"onDestroy"});

    const scene::EntityHandle a = bed.AddScripted(counter, u8"a");
    (void)bed.AddScripted(counter, u8"b");
    bed.Start();
    bed.Frame();
    CHECK(bed.scripts->InstanceCount() == 2u);

    bed.scene.DestroyEntity(a);
    bed.Frame();
    CHECK(bed.scripts->InstanceCount() == 1u);
    {
        const Variant count = bed.host.ScriptContext()->GetGlobal(u8"DestroyCount");
        REQUIRE(count.TryGet<f64>() != nullptr);
        CHECK(*count.TryGet<f64>() == 1.0);
    }

    bed.scene.Stop();
    CHECK(bed.scripts->InstanceCount() == 0u);
    {
        const Variant count = bed.host.ScriptContext()->GetGlobal(u8"DestroyCount");
        REQUIRE(count.TryGet<f64>() != nullptr);
        CHECK(*count.TryGet<f64>() == 2.0);
    }
}

TEST_CASE("script.scene: a faulting behavior is disabled and logged; siblings keep "
          "running")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> faulty = MakeClass(u8"Faulty",
                                           u8"class Faulty {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Faulty(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onUpdate(double dt) { int zero = 0; int boom = 10 / zero; }\n"
                                           u8"}\n",
                                           {u8"onUpdate"});
    RefPtr<ScriptClass> steady = MakeClass(u8"Steady",
                                           u8"class Steady {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Steady(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onUpdate(double dt) {\n"
                                           u8"        Float3 p = self.position();\n"
                                           u8"        self.setPosition(p.x + 1.0f, p.y, p.z);\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onUpdate"});

    const scene::EntityHandle e = bed.scene.CreateEntity(u8"both");
    ScriptComponent& c = bed.components->Add(e);
    {
        ScriptBehavior first;
        first.script = faulty;
        c.behaviors.PushBack(Move(first));
        ScriptBehavior second;
        second.script = steady;
        c.behaviors.PushBack(Move(second));
    }

    bed.Start();
    bed.Frame();
    bed.Frame();
    ScriptComponent* live = bed.components->Get(e);
    CHECK(live->behaviors[0].faulted);                            // disabled after the fault
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f)); // sibling unaffected
}

TEST_CASE("script.scene: hot reload - product swap re-instantiates, re-applies "
          "overrides, resets transient state")
{
    ScriptedScene bed;
    const char8_t* moverV1 = u8"class Mover {\n"
                             u8"    private Entity@ self;\n"
                             u8"    private int ticks;\n"
                             u8"    float speed;\n"
                             u8"    Mover(Entity@ entity) {\n"
                             u8"        @self = entity;\n"
                             u8"        speed = 0.0f;\n"
                             u8"        ticks = 0;\n"
                             u8"    }\n"
                             u8"    void onStart() { self.setName(self.name() + \"+start\"); }\n"
                             u8"    void onUpdate(double dt) {\n"
                             u8"        ticks = ticks + 1;\n"
                             u8"        Float3 p = self.position();\n"
                             u8"        self.setPosition(p.x + speed * float(dt), p.y, p.z);\n"
                             u8"    }\n"
                             u8"}\n";
    // v2: same convention, double the speed effect, so the reload is observable.
    const char8_t* moverV2 = u8"class Mover {\n"
                             u8"    private Entity@ self;\n"
                             u8"    float speed;\n"
                             u8"    Mover(Entity@ entity) {\n"
                             u8"        @self = entity;\n"
                             u8"        speed = 0.0f;\n"
                             u8"    }\n"
                             u8"    void onStart() { self.setName(self.name() + \"+restart\"); }\n"
                             u8"    void onUpdate(double dt) {\n"
                             u8"        Float3 p = self.position();\n"
                             u8"        self.setPosition(p.x + 2.0f * speed * float(dt), p.y, p.z);\n"
                             u8"    }\n"
                             u8"}\n";

    RefPtr<ScriptClass> v1 =
        MakeClass(u8"Mover", moverV1, {u8"onStart", u8"onUpdate"}, {FloatProperty(u8"speed", 1.0)});

    const scene::EntityHandle e = bed.AddScripted(v1, u8"m");
    {
        ScriptPropertyValue three;
        three.kind = ScriptPropertyType::Float;
        three.number = 3.0;
        bed.components->Get(e)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"speed"), three);
    }
    bed.Start();
    bed.Frame(); // dt 0.5: x = 1.5 (override 3)
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"m+start"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.5f));

    // The cook swaps the product (here: the Ref's direct object). Same class name.
    RefPtr<ScriptClass> v2 =
        MakeClass(u8"Mover", moverV2, {u8"onStart", u8"onUpdate"}, {FloatProperty(u8"speed", 1.0)});
    bed.components->Get(e)->behaviors[0].script = v2;

    bed.Frame(); // re-instantiate (fresh state, onStart again), override re-applied
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"m+start+restart"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 4.5f)); // +2*3*0.5
    CHECK(bed.components->Get(e)->behaviors[0].boundClass == v2.Get());
}

TEST_CASE("script.scene: entity-typed properties resolve guids to live entity handles "
          "(the facade crosses the boundary)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> chaser = MakeClass(u8"Chaser",
                                           u8"class Chaser {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Entity@ target;\n"
                                           u8"    Chaser(Entity@ entity) {\n"
                                           u8"        @self = entity;\n"
                                           u8"        @target = null;\n"
                                           u8"    }\n"
                                           u8"    void onUpdate(double dt) {\n"
                                           u8"        if (target !is null) {\n"
                                           u8"            Float3 t = target.position();\n"
                                           u8"            self.setPosition(t.x, t.y, t.z);\n"
                                           u8"        }\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onUpdate"}, {EntityProperty(u8"target")});

    const scene::EntityHandle goal = bed.scene.CreateEntity(u8"goal");
    bed.scene.SetLocalPosition(goal, Float3{7.0f, 8.0f, 9.0f});
    const scene::EntityHandle e = bed.AddScripted(chaser, u8"chaser");
    {
        ScriptPropertyValue target;
        target.kind = ScriptPropertyType::Entity;
        target.guid = bed.scene.GetEntityId(goal);
        bed.components->Get(e)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"target"),
                                                         target);
    }
    bed.Start();
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 7.0f));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.z, 9.0f));
}

TEST_CASE("script.scene: Log/Time/Random facades are callable (service-bound per run)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> user = MakeClass(
        u8"FacadeUser",
        u8"class FacadeUser {\n"
        u8"    private Entity@ self;\n"
        u8"    private int ticks;\n"
        u8"    FacadeUser(Entity@ entity) {\n"
        u8"        @self = entity;\n"
        u8"        ticks = 0;\n"
        u8"    }\n"
        u8"    void onUpdate(double dt) {\n"
        u8"        ticks = ticks + 1;\n"
        u8"        Log::info(\"tick at \" + Time::now() + \" delta \" + Time::delta());\n"
        u8"        double r = Random::range(1.0, 2.0);\n"
        u8"        if (r < 1.0 || r >= 2.0) { int zero = 0; int boom = 10 / zero; }\n"
        u8"        // The run clock starts WITH the context - meaningful from tick 2 on.\n"
        u8"        if (ticks > 1 && Time::delta() <= 0) { int zero = 0; int boom = 10 / zero; }\n"
        u8"        if (ticks > 1 && Time::now() <= 0) { int zero = 0; int boom = 10 / zero; }\n"
        u8"        self.setPosition(float(Random::intRange(4, 4)), 0.0f, 0.0f);\n"
        u8"    }\n"
        u8"}\n",
        {u8"onUpdate"});

    const scene::EntityHandle e = bed.AddScripted(user, u8"f");
    bed.Start();
    bed.Frame();
    bed.Frame();
    ScriptComponent* c = bed.components->Get(e);
    CHECK_FALSE(c->behaviors[0].faulted);
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 4.0f));
}

TEST_CASE("script.scene: component wire round-trips through SerializeScene (script "
          "refs, enabled flags, every override kind; symmetric + count-guarded)")
{
    RegisterScriptComponentReflection();
    scene::Scene scene(DefaultAllocator(), u8"wire");
    auto* manager = scene.AddSystem<ScriptComponentManager>();

    const scene::EntityHandle e = scene.CreateEntity(u8"scripted");
    ScriptComponent& c = manager->Add(e);
    {
        ScriptBehavior first;
        first.script.SetId(Guid{0x11, 0x22});
        first.enabled = false;
        ScriptPropertyValue speed;
        speed.kind = ScriptPropertyType::Float;
        speed.number = 12.5;
        first.SetOverride(ScriptPropertyNameHash(u8"speed"), speed);
        ScriptPropertyValue label;
        label.kind = ScriptPropertyType::String;
        label.text = String(u8"hello");
        first.SetOverride(ScriptPropertyNameHash(u8"label"), label);
        ScriptPropertyValue tint;
        tint.kind = ScriptPropertyType::Color;
        tint.color = Color{0.1f, 0.2f, 0.3f, 0.4f};
        first.SetOverride(ScriptPropertyNameHash(u8"tint"), tint);
        c.behaviors.PushBack(Move(first));

        ScriptBehavior second;
        second.script.SetId(Guid{0x33, 0x44});
        ScriptPropertyValue target;
        target.kind = ScriptPropertyType::Entity;
        target.guid = Guid{0x55, 0x66};
        second.SetOverride(ScriptPropertyNameHash(u8"target"), target);
        ScriptPropertyValue vec;
        vec.kind = ScriptPropertyType::Vec3;
        vec.vector = Float3{1, 2, 3};
        second.SetOverride(ScriptPropertyNameHash(u8"offset"), vec);
        c.behaviors.PushBack(Move(second));
    }
    const Guid entityId = scene.GetEntityId(e);

    MemoryStream stream;
    {
        BinarySerializer w(stream, SerializeMode::Write);
        scene::SerializeScene(w, scene);
        REQUIRE(w.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    scene::Scene loaded(DefaultAllocator(), u8"loaded");
    auto* loadedManager = loaded.AddSystem<ScriptComponentManager>();
    {
        BinarySerializer r(stream, SerializeMode::Read);
        scene::SerializeScene(r, loaded);
        REQUIRE(r.IsOk());
    }
    const scene::EntityHandle le = loaded.FindEntity(entityId);
    REQUIRE(le.IsAssigned());
    ScriptComponent* lc = loadedManager->Get(le);
    REQUIRE(lc != nullptr);
    REQUIRE(lc->behaviors.Size() == 2u);

    const ScriptBehavior& b0 = lc->behaviors[0];
    CHECK(b0.script.id == (Guid{0x11, 0x22}));
    CHECK_FALSE(b0.enabled);
    REQUIRE(b0.overrides.Size() == 3u);
    const ScriptPropertyOverride* speed = b0.FindOverride(ScriptPropertyNameHash(u8"speed"));
    REQUIRE(speed != nullptr);
    CHECK(speed->value.kind == ScriptPropertyType::Float);
    CHECK(speed->value.number == doctest::Approx(12.5));
    const ScriptPropertyOverride* label = b0.FindOverride(ScriptPropertyNameHash(u8"label"));
    REQUIRE(label != nullptr);
    CHECK(label->value.text == u8"hello");
    const ScriptPropertyOverride* tint = b0.FindOverride(ScriptPropertyNameHash(u8"tint"));
    REQUIRE(tint != nullptr);
    CHECK(tint->value.color.b == doctest::Approx(0.3f));

    const ScriptBehavior& b1 = lc->behaviors[1];
    CHECK(b1.script.id == (Guid{0x33, 0x44}));
    CHECK(b1.enabled);
    const ScriptPropertyOverride* target = b1.FindOverride(ScriptPropertyNameHash(u8"target"));
    REQUIRE(target != nullptr);
    CHECK(target->value.guid == (Guid{0x55, 0x66}));
    const ScriptPropertyOverride* offset = b1.FindOverride(ScriptPropertyNameHash(u8"offset"));
    REQUIRE(offset != nullptr);
    CHECK(offset->value.vector.y == doctest::Approx(2.0f));

    // Runtime fields never travel the wire.
    CHECK(b0.instance.Get() == nullptr);
    CHECK(b0.boundClass == nullptr);
    CHECK_FALSE(b0.started);
}

TEST_CASE("script.scene: a prefab-instance override on a behavior property round-trips "
          "(component-granular deltas - zero script-specific code)")
{
    RegisterScriptComponentReflection();

    // Author the prefab: one entity with a scripted behavior, default speed.
    scene::Scene author(DefaultAllocator(), u8"author");
    auto* authorScripts = author.AddSystem<ScriptComponentManager>();
    const scene::EntityHandle root = author.CreateEntity(u8"Bot");
    {
        ScriptComponent& c = authorScripts->Add(root);
        ScriptBehavior behavior;
        behavior.script.SetId(Guid{0xAA, 0x01});
        c.behaviors.PushBack(Move(behavior));
    }
    MemoryStream payload;
    REQUIRE(scene::CapturePrefab(author, root, payload).IsOk());

    // Level: two instances; one overrides the behavior's speed.
    scene::Scene level(DefaultAllocator(), u8"level");
    auto* levelScripts = level.AddSystem<ScriptComponentManager>();
    (void)payload.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0xBB, 0x02};
    const scene::EntityHandle inst1 = scene::SpawnPrefab(level, payload, prefabId);
    (void)payload.Seek(0, SeekOrigin::Begin);
    const scene::EntityHandle inst2 = scene::SpawnPrefab(level, payload, prefabId);
    REQUIRE(inst1.IsAssigned());
    REQUIRE(inst2.IsAssigned());
    {
        ScriptPropertyValue nine;
        nine.kind = ScriptPropertyType::Float;
        nine.number = 9.0;
        levelScripts->Get(inst1)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"speed"), nine);
    }
    const Guid inst1Id = level.GetEntityId(inst1);
    const Guid inst2Id = level.GetEntityId(inst2);

    // Save, load and resolve prefabs (instances are a ref and deltas on the wire).
    MemoryStream saved;
    {
        BinarySerializer w(saved, SerializeMode::Write);
        scene::SerializeScene(w, level);
        REQUIRE(w.IsOk());
    }
    scene::Scene loaded(DefaultAllocator(), u8"loaded");
    auto* loadedScripts = loaded.AddSystem<ScriptComponentManager>();
    (void)saved.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer r(saved, SerializeMode::Read);
        scene::SerializeScene(r, loaded);
        REQUIRE(r.IsOk());
    }
    const Span<const byte> payloadBytes = payload.Bytes();
    scene::ResolveScenePrefabs(
        loaded,
        Function<UniquePtr<IStream>(const Guid&)>{
            [&payloadBytes, prefabId](const Guid& id) -> UniquePtr<IStream>
            {
                if (id != prefabId)
                {
                    return UniquePtr<IStream>{};
                }
                auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
                (void)stream->Write(payloadBytes.Data(), payloadBytes.Size());
                (void)stream->Seek(0, SeekOrigin::Begin);
                return UniquePtr<IStream>(stream.Release(), DefaultAllocator());
            }});

    const scene::EntityHandle l1 = loaded.FindEntity(inst1Id);
    const scene::EntityHandle l2 = loaded.FindEntity(inst2Id);
    REQUIRE(l1.IsAssigned());
    REQUIRE(l2.IsAssigned());
    ScriptComponent* c1 = loadedScripts->Get(l1);
    ScriptComponent* c2 = loadedScripts->Get(l2);
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);
    REQUIRE(c1->behaviors.Size() == 1u);
    REQUIRE(c2->behaviors.Size() == 1u);

    // The override survived on instance 1; instance 2 still tracks the template.
    const ScriptPropertyOverride* over =
        c1->behaviors[0].FindOverride(ScriptPropertyNameHash(u8"speed"));
    REQUIRE(over != nullptr);
    CHECK(over->value.number == doctest::Approx(9.0));
    CHECK(c2->behaviors[0].overrides.IsEmpty());
    CHECK(c1->behaviors[0].script.id == (Guid{0xAA, 0x01}));
}

TEST_CASE("script.scene: run-host teardown after stop releases the context; a language "
          "without a backend refuses cleanly")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> noop = MakeClass(u8"Noop",
                                         u8"class Noop {\n"
                                         u8"    private Entity@ self;\n"
                                         u8"    Noop(Entity@ entity) { @self = entity; }\n"
                                         u8"    void onUpdate(double dt) {}\n"
                                         u8"}\n",
                                         {u8"onUpdate"});
    (void)bed.AddScripted(noop, u8"n");
    bed.Start();
    bed.Frame();
    CHECK(bed.host.IsActive());
    bed.scene.Stop();
    CHECK(bed.scripts->InstanceCount() == 0u);
    bed.host.Teardown(); // the subsystem's MaybeTeardown does this in-engine
    CHECK_FALSE(bed.host.IsActive());

    // Behaviors in an unregistered language stay disabled with a clean error.
    RefPtr<ScriptClass> alien = MakeClass(u8"Alien", u8"whatever", {u8"onUpdate"});
    alien->language = String(u8"nolang");
    ScriptRunHost host(DefaultAllocator());
    CHECK(host.EnsureContext(u8"nolang") == nullptr);
    CHECK_FALSE(host.EnsureClassLoaded(*alien));
}

TEST_CASE("script.scene: entity.send invokes on<Message>(arg) on every declaring "
          "behavior of the target (P2 messaging)")
{
    ScriptedScene bed;

    // Receiver at index 0 so it is instantiated before the sender (index 1) fires its onStart
    // send this same tick: dispatch skips instances that are not live yet.
    RefPtr<ScriptClass> receiver =
        MakeClass(u8"Receiver",
                  u8"class Receiver {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Receiver(Entity@ entity) { @self = entity; }\n"
                  u8"    void onPing(int amount) { self.setName(\"pinged:\" + amount); }\n"
                  u8"}\n",
                  {u8"onPing"});
    RefPtr<ScriptClass> sender = MakeClass(u8"Sender",
                                           u8"class Sender {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Sender(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() { self.send(\"ping\", 7); }\n"
                                           u8"}\n",
                                           {u8"onStart"});

    scene::EntityHandle target = bed.scene.CreateEntity(u8"target");
    ScriptComponent& component = bed.components->Add(target);
    {
        ScriptBehavior b;
        b.script = receiver;
        component.behaviors.PushBack(Move(b));
    }
    {
        ScriptBehavior b;
        b.script = sender;
        component.behaviors.PushBack(Move(b));
    }

    bed.Start();
    bed.Frame(); // both instantiate; sender.onStart sends "ping", receiver.onPing runs

    CHECK(bed.scene.GetEntityName(target) == u8"pinged:7");
}

TEST_CASE("script.scene: entity.send to a target with no matching handler is a safe "
          "no-op (P2 messaging)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> sender = MakeClass(u8"LoneSender",
                                           u8"class LoneSender {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    LoneSender(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() { self.send(\"noHandler\", 1); }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    scene::EntityHandle e = bed.AddScripted(sender, u8"lone");
    bed.Start();
    bed.Frame(); // must not fault the sender
    ScriptComponent* c = bed.components->Get(e);
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->behaviors[0].faulted);
}

// ---- The "Roll Call" sample game's core loop, headless: orbs collected by proximity emit
// "OrbCollected" onto the scene bus, the Level scores, and the game is won at the total. The game
// rules live in the scene's Level tier, and orbs announce a named event instead of finding a
// "GameManager" entity and sending to it; no GameManager entity exists. Score is observed through
// a stable "Scoreboard" entity's transform. The input and character path needs Input and Physics
// (exercised on Play), so it is outside this bed. ----

TEST_CASE("script.scene: the Roll Call sample game - orbs EMIT OrbCollected, the Level scores "
          "and wins, with NO GameManager entity (Track B reference)")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    // The Level: the scene's rules tier, total 2 for a short round. Score lives in per-instance
    // fields; onOrbCollected mirrors it onto the Scoreboard transform so the test can observe it.
    // It receives events by declaring on<Event>, with no wiring and no manager entity.
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    private int total;\n"
                  u8"    private int collected;\n"
                  u8"    private bool won;\n"
                  u8"    Level(Scene@ s) {\n"
                  u8"        @scene = s;\n"
                  u8"        total = 2;\n"
                  u8"        collected = 0;\n"
                  u8"        won = false;\n"
                  u8"    }\n"
                  u8"    void onStart() {\n"
                  u8"        collected = 0;\n"
                  u8"        won = false;\n"
                  u8"    }\n"
                  u8"    void onOrbCollected() {\n"
                  u8"        if (won) {\n"
                  u8"            return;\n"
                  u8"        }\n"
                  u8"        collected = collected + 1;\n"
                  u8"        int w = 0;\n"
                  u8"        if (collected >= total) {\n"
                  u8"            won = true;\n"
                  u8"            w = 1;\n"
                  u8"        }\n"
                  u8"        scene.find(\"Scoreboard\").setPosition(float(collected), float(w), 0.0f);\n"
                  u8"    }\n"
                  u8"    void onPlayerFell() { won = true; }\n"
                  u8"}\n",
                  {u8"onStart", u8"onOrbCollected", u8"onPlayerFell"});
    level->Settings().script = levelClass;

    // Pickup: near "Player", emit "OrbCollected" and remove itself.
    RefPtr<ScriptClass> pickup =
        MakeClass(u8"Pickup",
                  u8"class Pickup {\n"
                  u8"    private Entity@ self;\n"
                  u8"    private float r;\n"
                  u8"    private bool taken;\n"
                  u8"    Pickup(Entity@ entity) {\n"
                  u8"        @self = entity;\n"
                  u8"        r = 1.2f;\n"
                  u8"        taken = false;\n"
                  u8"    }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        if (taken) {\n"
                  u8"            return;\n"
                  u8"        }\n"
                  u8"        Entity@ player = self.scene.find(\"Player\");\n"
                  u8"        if (!player.isValid()) {\n"
                  u8"            return;\n"
                  u8"        }\n"
                  u8"        Float3 p = player.position();\n"
                  u8"        Float3 me = self.position();\n"
                  u8"        float dx = p.x - me.x;\n"
                  u8"        float dy = p.y - me.y;\n"
                  u8"        float dz = p.z - me.z;\n"
                  u8"        if (dx*dx + dy*dy + dz*dz > r*r) {\n"
                  u8"            return;\n"
                  u8"        }\n"
                  u8"        taken = true;\n"
                  u8"        self.scene.events.emit(\"OrbCollected\");\n"
                  u8"        self.destroy();\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});

    const scene::EntityHandle scoreboard = bed.scene.CreateEntity(u8"Scoreboard"); // score readout
    const scene::EntityHandle player = bed.scene.CreateEntity(u8"Player");
    bed.scene.SetLocalPosition(player, Float3{100.0f, 0.0f, 100.0f}); // start well clear
    const scene::EntityHandle orb1 = bed.AddScripted(pickup, u8"Orb1");
    const scene::EntityHandle orb2 = bed.AddScripted(pickup, u8"Orb2");
    bed.scene.SetLocalPosition(orb1, Float3{0.0f, 0.0f, 0.0f});
    bed.scene.SetLocalPosition(orb2, Float3{5.0f, 0.0f, 0.0f});

    bed.Start();
    bed.Frame(); // instantiate and onStart; the player is far, nothing collected
    CHECK(bed.scripts->InstanceCount() == 2u); // 2 orbs (the Level is not an entity behavior)
    CHECK(level->LevelActive());               // the rules tier is live, no GameManager entity
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 0.0f));

    // Walk onto orb1: it collects itself and emits "OrbCollected". Delivery follows the run
    // scope's cadence: the scope drains at the next frame's top, so events emitted in frame N
    // arrive in frame N+1, the cadence GameInstance has.
    bed.scene.SetLocalPosition(player, Float3{0.0f, 0.0f, 0.0f});
    bed.Frame(); // orb1 onUpdate: proximity hit, emit("OrbCollected"), destroy
    CHECK_FALSE(bed.scene.IsValid(orb1));
    bed.Frame(); // the scope drains at frame top; the Level's onOrbCollected scores
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 1.0f));
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.y, 0.0f)); // not won yet

    // Walk onto orb2: collecting the last orb wins (same one-frame delivery cadence).
    bed.scene.SetLocalPosition(player, Float3{5.0f, 0.0f, 0.0f});
    bed.Frame();
    CHECK_FALSE(bed.scene.IsValid(orb2));
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 2.0f)); // both collected
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.y, 1.0f)); // win branch ran
}

TEST_CASE("script.scene: the Roll Call sample game runs on a LUAU Level")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    RefPtr<ScriptClass> levelClass = MakeClassLang(
        u8"luau", u8"Level",
        u8"Level = {}\n"
        u8"Level.__index = Level\n"
        u8"function Level.new(scene)\n"
        u8"    return setmetatable({ scene = scene, total = 2, collected = 0, won = false }, Level)\n"
        u8"end\n"
        u8"function Level:onStart() self.collected = 0; self.won = false end\n"
        u8"function Level:onOrbCollected()\n"
        u8"    if self.won then return end\n"
        u8"    self.collected = self.collected + 1\n"
        u8"    local won = 0\n"
        u8"    if self.collected >= self.total then self.won = true; won = 1 end\n"
        u8"    self.scene:find(\"Scoreboard\"):setPosition(self.collected, won, 0)\n"
        u8"end\n"
        u8"function Level:onPlayerFell() self.won = true end\n",
        {u8"onStart", u8"onOrbCollected", u8"onPlayerFell"});
    level->Settings().script = levelClass;

    RefPtr<ScriptClass> pickup = MakeClassLang(
        u8"luau", u8"Pickup",
        u8"Pickup = {}\n"
        u8"Pickup.__index = Pickup\n"
        u8"function Pickup.new(entity)\n"
        u8"    return setmetatable({ entity = entity, r = 1.2, taken = false }, Pickup)\n"
        u8"end\n"
        u8"function Pickup:onUpdate(dt)\n"
        u8"    if self.taken then return end\n"
        u8"    local player = self.entity.scene:find(\"Player\")\n"
        u8"    if not player:isValid() then return end\n"
        u8"    local p = player:position()\n"
        u8"    local me = self.entity:position()\n"
        u8"    local dx = p.x - me.x\n"
        u8"    local dy = p.y - me.y\n"
        u8"    local dz = p.z - me.z\n"
        u8"    if dx*dx + dy*dy + dz*dz > self.r*self.r then return end\n"
        u8"    self.taken = true\n"
        u8"    self.entity.scene.events:emit(\"OrbCollected\")\n"
        u8"    self.entity:destroy()\n"
        u8"end\n",
        {u8"onUpdate"});

    const scene::EntityHandle scoreboard = bed.scene.CreateEntity(u8"Scoreboard");
    const scene::EntityHandle player = bed.scene.CreateEntity(u8"Player");
    bed.scene.SetLocalPosition(player, Float3{100.0f, 0.0f, 100.0f});
    const scene::EntityHandle orb1 = bed.AddScripted(pickup, u8"Orb1");
    const scene::EntityHandle orb2 = bed.AddScripted(pickup, u8"Orb2");
    bed.scene.SetLocalPosition(orb1, Float3{0.0f, 0.0f, 0.0f});
    bed.scene.SetLocalPosition(orb2, Float3{5.0f, 0.0f, 0.0f});

    bed.Start();
    bed.Frame(); // instantiate and onStart; the player is far, nothing collected
    CHECK(bed.scripts->InstanceCount() == 2u);
    CHECK(level->LevelActive());
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 0.0f));

    bed.scene.SetLocalPosition(player, Float3{0.0f, 0.0f, 0.0f});
    bed.Frame(); // orb1 collected and emits (delivered at the next frame's drain)
    CHECK_FALSE(bed.scene.IsValid(orb1));
    bed.Frame(); // scope drain: the Level scores
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 1.0f));
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.y, 0.0f)); // not won yet

    bed.scene.SetLocalPosition(player, Float3{5.0f, 0.0f, 0.0f});
    bed.Frame(); // last orb collected
    CHECK_FALSE(bed.scene.IsValid(orb2));
    bed.Frame(); // scope drain: win
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 2.0f));
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.y, 1.0f)); // win branch ran
}

TEST_CASE("script.scene: bus token teardown - a destroyed subscriber is never touched, "
          "survivors still fire")
{
    // The script system owns the bus subscriptions, and dispatch resolves live components at
    // drain time: a behaviour whose entity died between emit and drain is not found (no dispatch
    // to a dead instance, no crash). This is lifetime-sensitive, so it matters under ASAN.
    ScriptedScene bed;
    RefPtr<ScriptClass> listener =
        MakeClass(u8"Listener",
                  u8"class Listener {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Listener(Entity@ entity) { @self = entity; }\n"
                  u8"    void onPing() { self.setName(\"heard\"); }\n"
                  u8"}\n",
                  {u8"onPing"}, {});
    const scene::EntityHandle doomed = bed.AddScripted(listener, u8"doomed");
    const scene::EntityHandle survivor = bed.AddScripted(listener, u8"survivor");
    bed.Start();
    bed.Frame(); // instantiate both

    bed.bus.Publish(StringHash(u8"Ping"), Variant{});
    bed.scene.DestroyEntity(doomed); // dies between emit and the next frame's drain
    bed.Frame();                     // drain: dispatch resolves live components only

    CHECK_FALSE(bed.scene.IsValid(doomed));
    CHECK(bed.scene.GetEntityName(survivor) == StringView(u8"heard")); // survivor fired
}

TEST_CASE("script.scene: updateInterval throttles onUpdate and delivers the accumulated "
          "dt (P3)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ticker = MakeClass(
        u8"Ticker",
        u8"class Ticker {\n"
        u8"    private Entity@ self;\n"
        u8"    Ticker(Entity@ entity) { @self = entity; }\n"
        u8"    void onUpdate(double dt) {\n"
        u8"        Float3 p = self.position();\n"
        u8"        self.setPosition(p.x + 1.0f, float(dt), p.z);\n" // x counts calls; y = last dt
        u8"    }\n"
        u8"}\n",
        {u8"onUpdate"});
    const scene::EntityHandle e = bed.AddScripted(ticker, u8"ticker");
    bed.components->Get(e)->behaviors[0].updateInterval = 1.0f;

    bed.Start();
    bed.Frame(0.5f); // acc 0.5 < 1.0 -> no update
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // acc 1.0 -> one update, dt = accumulated 1.0
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.y, 1.0f)); // accumulated dt, not 0.5
    bed.Frame(0.5f);                                              // acc 0.5 -> no update
    bed.Frame(0.5f);                                              // acc 1.0 -> second update
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f)); // 2 calls over 4 ticks
}

TEST_CASE("script.scene: updateInterval survives the SerializeScene wire")
{
    RegisterScriptComponentReflection();
    scene::Scene scene(DefaultAllocator(), u8"interval-wire");
    auto* manager = scene.AddSystem<ScriptComponentManager>();
    const scene::EntityHandle e = scene.CreateEntity(u8"scripted");
    ScriptComponent& c = manager->Add(e);
    ScriptBehavior behavior;
    behavior.script.SetId(Guid{0x77, 0x88});
    behavior.updateInterval = 0.25f;
    c.behaviors.PushBack(Move(behavior));
    const Guid entityId = scene.GetEntityId(e);

    MemoryStream stream;
    {
        BinarySerializer w(stream, SerializeMode::Write);
        scene::SerializeScene(w, scene);
        REQUIRE(w.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    scene::Scene loaded(DefaultAllocator(), u8"loaded");
    auto* loadedManager = loaded.AddSystem<ScriptComponentManager>();
    {
        BinarySerializer r(stream, SerializeMode::Read);
        scene::SerializeScene(r, loaded);
        REQUIRE(r.IsOk());
    }
    ScriptComponent* lc = loadedManager->Get(loaded.FindEntity(entityId));
    REQUIRE(lc != nullptr);
    REQUIRE(lc->behaviors.Size() == 1u);
    CHECK(Near(lc->behaviors[0].updateInterval, 0.25f));
}

TEST_CASE("script.scene: LUAU enable/disable edges dispatch onEnable/onDisable; disabled skips tick")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> toggler = MakeClassLang(
        u8"luau", u8"Toggler",
        u8"Toggler = {}\n"
        u8"Toggler.__index = Toggler\n"
        u8"function Toggler.new(entity) return setmetatable({ entity = entity }, Toggler) end\n"
        u8"function Toggler:onEnable() self.entity:setName(self.entity:name() .. \"+on\") end\n"
        u8"function Toggler:onDisable() self.entity:setName(self.entity:name() .. \"+off\") end\n"
        u8"function Toggler:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + 1, p.y, p.z)\n"
        u8"end\n",
        {u8"onEnable", u8"onDisable", u8"onUpdate"});

    const scene::EntityHandle e = bed.AddScripted(toggler, u8"t");
    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    bed.components->Get(e)->behaviors[0].enabled = false;
    bed.Frame(); // onDisable, no tick
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on+off"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    bed.components->Get(e)->behaviors[0].enabled = true;
    bed.Frame(); // onEnable again, and ticks
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on+off+on"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
}

TEST_CASE("script.scene: LUAU onDestroy fires on entity destroy AND on scene stop")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> counter = MakeClassLang(
        u8"luau", u8"Counter",
        u8"DestroyCount = 0\n"
        u8"Counter = {}\n"
        u8"Counter.__index = Counter\n"
        u8"function Counter.new(entity) return setmetatable({ entity = entity }, Counter) end\n"
        u8"function Counter:onUpdate(dt) end\n"
        u8"function Counter:onDestroy() DestroyCount = DestroyCount + 1 end\n",
        {u8"onUpdate", u8"onDestroy"});

    const scene::EntityHandle a = bed.AddScripted(counter, u8"a");
    (void)bed.AddScripted(counter, u8"b");
    bed.Start();
    bed.Frame();
    CHECK(bed.scripts->InstanceCount() == 2u);

    bed.scene.DestroyEntity(a);
    bed.Frame();
    CHECK(bed.scripts->InstanceCount() == 1u);
    {
        const Variant count = bed.host.ScriptContext()->GetGlobal(u8"DestroyCount");
        REQUIRE(count.TryGet<f64>() != nullptr);
        CHECK(*count.TryGet<f64>() == 1.0);
    }

    bed.scene.Stop();
    CHECK(bed.scripts->InstanceCount() == 0u);
    {
        const Variant count = bed.host.ScriptContext()->GetGlobal(u8"DestroyCount");
        REQUIRE(count.TryGet<f64>() != nullptr);
        CHECK(*count.TryGet<f64>() == 2.0);
    }
}

TEST_CASE("script.scene: LUAU a faulting behavior is disabled; siblings keep running")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> faulty = MakeClassLang(
        u8"luau", u8"Faulty",
        u8"Faulty = {}\n"
        u8"Faulty.__index = Faulty\n"
        u8"function Faulty.new(entity) return setmetatable({ entity = entity }, Faulty) end\n"
        u8"function Faulty:onUpdate(dt) error(\"boom\") end\n",
        {u8"onUpdate"});
    RefPtr<ScriptClass> steady = MakeClassLang(
        u8"luau", u8"Steady",
        u8"Steady = {}\n"
        u8"Steady.__index = Steady\n"
        u8"function Steady.new(entity) return setmetatable({ entity = entity }, Steady) end\n"
        u8"function Steady:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + 1, p.y, p.z)\n"
        u8"end\n",
        {u8"onUpdate"});

    const scene::EntityHandle e = bed.scene.CreateEntity(u8"both");
    ScriptComponent& c = bed.components->Add(e);
    {
        ScriptBehavior first;
        first.script = faulty;
        c.behaviors.PushBack(Move(first));
        ScriptBehavior second;
        second.script = steady;
        c.behaviors.PushBack(Move(second));
    }

    bed.Start();
    bed.Frame();
    bed.Frame();
    ScriptComponent* live = bed.components->Get(e);
    CHECK(live->behaviors[0].faulted);                           // disabled after the fault
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f)); // sibling unaffected
}

TEST_CASE("script.scene: LUAU hot reload - product swap re-instantiates + re-applies overrides")
{
    ScriptedScene bed;
    const char8_t* moverV1 =
        u8"Mover = {}\n"
        u8"Mover.__index = Mover\n"
        u8"function Mover.new(entity) return setmetatable({ entity = entity, speed = 0.0 }, Mover) end\n"
        u8"function Mover:onStart() self.entity:setName(self.entity:name() .. \"+start\") end\n"
        u8"function Mover:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
        u8"end\n";
    // v2: double the speed effect, so the reload is observable.
    const char8_t* moverV2 =
        u8"Mover = {}\n"
        u8"Mover.__index = Mover\n"
        u8"function Mover.new(entity) return setmetatable({ entity = entity, speed = 0.0 }, Mover) end\n"
        u8"function Mover:onStart() self.entity:setName(self.entity:name() .. \"+restart\") end\n"
        u8"function Mover:onUpdate(dt)\n"
        u8"    local p = self.entity:position()\n"
        u8"    self.entity:setPosition(p.x + 2 * self.speed * dt, p.y, p.z)\n"
        u8"end\n";

    RefPtr<ScriptClass> v1 =
        MakeClassLang(u8"luau", u8"Mover", moverV1, {u8"onStart", u8"onUpdate"},
                      {FloatProperty(u8"speed", 1.0)});
    const scene::EntityHandle e = bed.AddScripted(v1, u8"m");
    {
        ScriptPropertyValue three;
        three.kind = ScriptPropertyType::Float;
        three.number = 3.0;
        bed.components->Get(e)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"speed"), three);
    }
    bed.Start();
    bed.Frame(); // dt 0.5: x = 1.5 (override 3)
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"m+start"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.5f));

    RefPtr<ScriptClass> v2 =
        MakeClassLang(u8"luau", u8"Mover", moverV2, {u8"onStart", u8"onUpdate"},
                      {FloatProperty(u8"speed", 1.0)});
    bed.components->Get(e)->behaviors[0].script = v2;

    bed.Frame(); // re-instantiate (fresh state, onStart again), override re-applied
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"m+start+restart"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 4.5f)); // +2*3*0.5
    CHECK(bed.components->Get(e)->behaviors[0].boundClass == v2.Get());
}

TEST_CASE("script.scene: LUAU entity-typed property resolves a guid to a live entity handle")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> chaser = MakeClassLang(
        u8"luau", u8"Chaser",
        u8"Chaser = {}\n"
        u8"Chaser.__index = Chaser\n"
        u8"function Chaser.new(entity) return setmetatable({ entity = entity, target = nil }, Chaser) end\n"
        u8"function Chaser:onUpdate(dt)\n"
        u8"    if self.target ~= nil then\n"
        u8"        local t = self.target:position()\n"
        u8"        self.entity:setPosition(t.x, t.y, t.z)\n"
        u8"    end\n"
        u8"end\n",
        {u8"onUpdate"}, {EntityProperty(u8"target")});

    const scene::EntityHandle goal = bed.scene.CreateEntity(u8"goal");
    bed.scene.SetLocalPosition(goal, Float3{7.0f, 8.0f, 9.0f});
    const scene::EntityHandle e = bed.AddScripted(chaser, u8"chaser");
    {
        ScriptPropertyValue target;
        target.kind = ScriptPropertyType::Entity;
        target.guid = bed.scene.GetEntityId(goal);
        bed.components->Get(e)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"target"), target);
    }
    bed.Start();
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 7.0f));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.z, 9.0f));
}

TEST_CASE("script.scene: LUAU Log/Time/Random facades are callable (service-bound per run)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> user = MakeClassLang(
        u8"luau", u8"FacadeUser",
        u8"FacadeUser = {}\n"
        u8"FacadeUser.__index = FacadeUser\n"
        u8"function FacadeUser.new(entity) return setmetatable({ entity = entity, ticks = 0 }, FacadeUser) end\n"
        u8"function FacadeUser:onUpdate(dt)\n"
        u8"    self.ticks = self.ticks + 1\n"
        u8"    Log.info(\"tick at \" .. tostring(Time.now()) .. \" delta \" .. tostring(Time.delta()))\n"
        u8"    local r = Random.range(1.0, 2.0)\n"
        u8"    if r < 1.0 or r >= 2.0 then error(\"range broken\") end\n"
        u8"    if self.ticks > 1 and Time.delta() <= 0 then error(\"delta broken\") end\n"
        u8"    if self.ticks > 1 and Time.now() <= 0 then error(\"clock broken\") end\n"
        u8"    self.entity:setPosition(Random.intRange(4, 4), 0, 0)\n"
        u8"end\n",
        {u8"onUpdate"});

    const scene::EntityHandle e = bed.AddScripted(user, u8"f");
    bed.Start();
    bed.Frame();
    bed.Frame();
    ScriptComponent* c = bed.components->Get(e);
    CHECK_FALSE(c->behaviors[0].faulted);
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 4.0f));
}

TEST_CASE("script.scene: Scene.spawn reaches the scene's PrefabSpawnSystem and returns the live "
          "instance root (P2)")
{
    ScriptedScene bed;
    // The real recipe: the scene's spawn system over a one-prefab database (no host callback).
    PrefabSource prefabs(bed);

    // The behavior spawns on start using a prefab id delivered as an asset property.
    RefPtr<ScriptClass> spawner =
        MakeClass(u8"Spawner",
                  u8"class Spawner {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ prefab;\n"
                  u8"    Spawner(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        Entity@ e = self.scene.spawn(prefab, 3.0f, 4.0f, 5.0f);\n"
                  u8"        e.setName(\"child\");\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    spawner->properties.PushBack(prefabs.Property());

    const scene::EntityHandle e = bed.AddScripted(spawner, u8"spawner");
    (void)e;
    bed.Start();
    bed.Frame();

    // Scene.spawn returned the live instance root: the script renamed it, it sits at the
    // requested position, and its Barrel child came with it (the whole payload spawned).
    CHECK(PrefabSource::Spawned(bed.scene) == 1u);
    const scene::EntityHandle spawnedHandle = bed.scene.FindEntityByName(u8"child");
    REQUIRE(spawnedHandle.IsAssigned());
    CHECK(Near(bed.scene.GetLocalTransform(spawnedHandle).position.x, 3.0f));
    CHECK(Near(bed.scene.GetLocalTransform(spawnedHandle).position.z, 5.0f));
    CHECK(bed.scene.FindChildByName(spawnedHandle, u8"Barrel").IsAssigned());
    CHECK(bed.scene.FindPrefabInstanceByRoot(bed.scene.GetEntityId(spawnedHandle)) != nullptr);
}

TEST_CASE("script.scene: LUAU Scene.spawn reaches the scene's PrefabSpawnSystem + returns a live Entity")
{
    ScriptedScene bed;
    PrefabSource prefabs(bed);

    // The prefab id crosses as an Asset property (a Guid), is stored on the instance field, then
    // read back and passed to spawn, exercising Guid marshalling both ways through the Luau VM.
    RefPtr<ScriptClass> spawner = MakeClassLang(
        u8"luau", u8"Spawner",
        u8"Spawner = {}\n"
        u8"Spawner.__index = Spawner\n"
        u8"function Spawner.new(entity) return setmetatable({ entity = entity }, Spawner) end\n"
        u8"function Spawner:onStart()\n"
        u8"    local e = self.entity.scene:spawn(self.prefab, 3.0, 4.0, 5.0)\n"
        u8"    e:setName(\"child\")\n"
        u8"end\n",
        {u8"onStart"});
    spawner->properties.PushBack(prefabs.Property());

    (void)bed.AddScripted(spawner, u8"spawner");
    bed.Start();
    bed.Frame();

    CHECK(PrefabSource::Spawned(bed.scene) == 1u);
    const scene::EntityHandle spawnedHandle = bed.scene.FindEntityByName(u8"child");
    REQUIRE(spawnedHandle.IsAssigned());
    CHECK(Near(bed.scene.GetLocalTransform(spawnedHandle).position.x, 3.0f));
    CHECK(Near(bed.scene.GetLocalTransform(spawnedHandle).position.z, 5.0f));
    CHECK(bed.scene.FindChildByName(spawnedHandle, u8"Barrel").IsAssigned());
}

TEST_CASE("script.scene: Scene.find / Scene.findByPath resolve entities in the current "
          "scene from a behavior (P2)")
{
    ScriptedScene bed;
    // Build a small hierarchy the behavior will look up: Target (root) and Player/Weapon.
    scene::EntityHandle target = bed.scene.CreateEntity(u8"Target");
    scene::EntityHandle player = bed.scene.CreateEntity(u8"Player");
    scene::EntityHandle weapon = bed.scene.CreateEntity(u8"Weapon");
    bed.scene.SetParent(weapon, player);
    (void)target;

    RefPtr<ScriptClass> finder =
        MakeClass(u8"Finder",
                  u8"class Finder {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Finder(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        Entity@ t = self.scene.find(\"Target\");\n"
                  u8"        if (t.isValid()) { t.setName(\"found-by-name\"); }\n"
                  u8"        Entity@ w = self.scene.findByPath(\"Player/Weapon\");\n"
                  u8"        if (w.isValid()) { w.setName(\"found-by-path\"); }\n"
                  u8"        Entity@ missing = self.scene.find(\"Nope\");\n"
                  u8"        if (!missing.isValid()) { self.setName(\"miss-ok\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(finder, u8"finder");
    bed.Start();
    bed.Frame();

    CHECK(bed.scene.GetEntityName(target) == StringView(u8"found-by-name"));
    CHECK(bed.scene.GetEntityName(weapon) == StringView(u8"found-by-path"));
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"miss-ok"));
}

// Two handles compare by the entity they name: the same entity found twice is equal, two entities
// are not, and the behavior's own handle equals the entity found by its name.
TEST_CASE("script.scene: entity handles compare with == and !=")
{
    ScriptedScene bed;
    scene::EntityHandle target = bed.scene.CreateEntity(u8"Target");
    scene::EntityHandle other = bed.scene.CreateEntity(u8"Other");
    (void)other;

    RefPtr<ScriptClass> comparer =
        MakeClass(u8"Comparer",
                  u8"class Comparer {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Comparer(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        Entity@ a = self.scene.find(\"Target\");\n"
                  u8"        Entity@ b = self.scene.find(\"Target\");\n"
                  u8"        Entity@ c = self.scene.find(\"Other\");\n"
                  u8"        Entity@ me = self.scene.find(\"comparer\");\n"
                  u8"        if (a == b && a != c && me == self) { a.setName(\"compared\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    (void)bed.AddScripted(comparer, u8"comparer");
    bed.Start();
    bed.Frame();

    CHECK(bed.scene.GetEntityName(target) == StringView(u8"compared"));
}

// worldPosition() inside onUpdate is this frame's: a behavior that moves a parent reads the child's
// new world position at once (it read last frame's, from the cached world matrix).
TEST_CASE("script.scene: worldPosition is current inside onUpdate after a move")
{
    ScriptedScene bed;
    scene::EntityHandle parent = bed.scene.CreateEntity(u8"Parent");
    scene::EntityHandle child = bed.scene.CreateEntity(u8"Child");
    bed.scene.SetParent(child, parent);
    bed.scene.SetLocalPosition(child, Float3{5.0f, 0.0f, 0.0f});

    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Mover(Entity@ entity) { @self = entity; }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Entity@ p = self.scene.find(\"Parent\");\n"
                  u8"        Entity@ c = self.scene.find(\"Child\");\n"
                  u8"        p.setPosition(20.0f, 0.0f, 0.0f);\n"
                  u8"        Float3 w = c.worldPosition();\n"
                  u8"        if (w.x > 24.99f && w.x < 25.01f) { c.setName(\"fresh\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    (void)bed.AddScripted(mover, u8"mover");
    bed.Start();
    bed.Frame();

    CHECK(bed.scene.GetEntityName(child) == StringView(u8"fresh"));
}

// worldRotation() / worldScale(): a child's turn in the world is its parent's after its own, scale
// aside, and its scale theirs times its own. A guard's lantern (a child) aims where the guard's turn
// and its own tip send it.
TEST_CASE("script.scene: worldRotation composes the ancestors' turns, worldScale their scales")
{
    ScriptedScene bed;
    scene::EntityHandle parent = bed.scene.CreateEntity(u8"Parent");
    scene::EntityHandle child = bed.scene.CreateEntity(u8"Child");
    bed.scene.SetParent(child, parent);
    Transform p = bed.scene.GetLocalTransform(parent);
    p.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 1.2f);
    p.scale = Float3{2.0f, 2.0f, 2.0f};
    bed.scene.SetLocalTransform(parent, p);
    Transform c = bed.scene.GetLocalTransform(child);
    c.rotation = Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.4f);
    c.scale = Float3{1.5f, 1.5f, 1.5f};
    bed.scene.SetLocalTransform(child, c);

    RefPtr<ScriptClass> reader =
        MakeClass(u8"Reader",
                  u8"class Reader {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Reader(Entity@ entity) { @self = entity; }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Entity@ p = self.scene.find(\"Parent\");\n"
                  u8"        Entity@ c = self.scene.find(\"Child\");\n"
                  u8"        Float3 f = Float3(0.0f, 0.0f, -1.0f);\n"
                  u8"        Float3 world = Quaternion::RotateVector(c.worldRotation(), f);\n"
                  u8"        Float3 composed = Quaternion::RotateVector(p.rotation(), Quaternion::RotateVector(c.rotation(), f));\n"
                  u8"        Float3 d = Float3(world.x - composed.x, world.y - composed.y, world.z - composed.z);\n"
                  u8"        bool turned = world.z > -0.99f;\n"
                  u8"        Float3 s = c.worldScale();\n"
                  u8"        bool scaled = s.x > 2.999f && s.x < 3.001f && s.y > 2.999f && s.z < 3.001f;\n"
                  u8"        if (turned && scaled && d.x * d.x + d.y * d.y + d.z * d.z < 1e-8f) { c.setName(\"composed\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    (void)bed.AddScripted(reader, u8"reader");
    bed.Start();
    bed.Frame();

    CHECK(bed.scene.GetEntityName(child) == StringView(u8"composed"));
}

// ---- A second backend: a .as behaviour runs the same neutral runtime path as any backend. The
// RunHost resolves the backend by the class's language, assembles the module through the backend
// (AngelScript needs no prelude), instantiates and dispatches the lifecycle. This behaviour
// carries no properties.

TEST_CASE("script.scene: an AngelScript behavior runs the neutral lifecycle path "
          "(onStart + onUpdate(dt)) - proves both backends, uniformly")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClassLang(u8"angelscript", u8"Mover",
                      u8"class Mover {\n"
                      u8"    private Entity@ self;\n"
                      u8"    private float x;\n"
                      u8"    private float speed;\n"
                      u8"    Mover(Entity@ entity) { @self = entity; x = 0.0f; speed = 2.0f; }\n"
                      u8"    void onStart() { self.setName(\"started\"); }\n"
                      u8"    void onUpdate(double dt) { x = x + speed * float(dt); "
                      u8"self.setPosition(x, 0.0f, 0.0f); }\n"
                      u8"}\n",
                      {u8"onStart", u8"onUpdate"});

    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate, onStart, first onUpdate (dt 0.5): x = 1.0
    CHECK(bed.host.Language() == StringView(u8"angelscript"));
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // 2.0 * 0.5
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    ScriptComponent* c = bed.components->Get(e);
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->behaviors[0].faulted);
}

// ---- The step debugger through the subsystem: a breakpoint in a behaviour handler pauses the
// game (behaviours stop advancing) while the debugger owns the suspended handler; Continue runs it
// to completion and ticking resumes. The suspension is not a fault. Covers the game-pause wiring
// and the debug-suspend handling end to end through the real ScriptSceneSystem, headless.

TEST_CASE("script.scene: a breakpoint in a behavior handler pauses the game and resumes clean")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    // Line-numbered so the breakpoint below lands inside onUpdate before x is incremented.
    RefPtr<ScriptClass> breaker =
        MakeClassLang(u8"angelscript", u8"Breaker",
                      u8"class Breaker {\n"                                           // 1
                      u8"    private Entity@ self;\n"                                 // 2
                      u8"    private float x;\n"                                      // 3
                      u8"    Breaker(Entity@ entity) { @self = entity; x = 0.0f; }\n" // 4
                      u8"    void onUpdate(double dt) {\n"                            // 5
                      u8"        x = x + 1.0f;\n"                    // 6  <- breakpoint
                      u8"        self.setPosition(x, 0.0f, 0.0f);\n" // 7
                      u8"    }\n"                                    // 8
                      u8"}\n",                                       // 9
                      {u8"onUpdate"});
    // The cook stamps sourceName with the asset file; the runtime loads this class in its own
    // section named by it, so a breakpoint keyed on the source file (the editor's key) lines up.
    breaker->sourceName = String(u8"Breaker.as");

    bed.host.RequestDebugger(Function<void(IScriptDebugger&)>{}); // debuggable run

    const scene::EntityHandle e = bed.AddScripted(breaker, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate + onUpdate (no breakpoint yet): x = 1
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    // Set the breakpoint on the source file (the editor's breakpoint key) at the increment line,
    // now that the debugger and module exist; it has to line up with the class's section.
    IScriptDebugger* debugger = bed.host.Debugger();
    REQUIRE(debugger != nullptr);
    CHECK_FALSE(bed.host.IsDebugPaused());
    debugger->SetBreakpoint(u8"Breaker.as", 6);

    // Next tick: onUpdate hits the breakpoint and suspends before the increment. The game is
    // paused, the world holds still (position frozen at 1), and the behaviour is not faulted.
    bed.Frame();
    CHECK(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);

    // The suspended context is inspectable: the innermost frame is on the break line, and
    // onUpdate's `dt` local is captured.
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 6);
    CHECK(StringView(frames[0].file) == u8"Breaker.as"); // the section is the source file
    Array<ScriptVariable> locals = debugger->CaptureLocals(0);
    bool sawDt = false;
    for (const ScriptVariable& local : locals)
    {
        if (StringView(local.name) == u8"dt")
        {
            sawDt = true;
        }
    }
    CHECK(sawDt);

    // While paused, further ticks advance nothing (frozen world).
    bed.Frame();
    CHECK(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    // Continue runs the suspended handler to completion: x increments to 2, setPosition runs,
    // and the pause clears so ticking resumes.
    debugger->Continue();
    CHECK_FALSE(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));

    // With the breakpoint removed, ticking is fully normal again.
    debugger->RemoveBreakpoint(u8"Breaker.as", 6);
    bed.Frame();
    CHECK_FALSE(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 3.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);
}

// The same run-host debugger wiring, backend-neutral, on Luau: a breakpoint in a Luau behaviour
// handler pauses the game, the world holds still, Continue resumes clean, no fault.
TEST_CASE("script.scene: LUAU a breakpoint in a behavior handler pauses the game and resumes clean")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> breaker = MakeClassLang(
        u8"luau", u8"Breaker",
        u8"Breaker = {}\n"                                                            // 1
        u8"Breaker.__index = Breaker\n"                                               // 2
        u8"function Breaker.new(entity)\n"                                            // 3
        u8"    return setmetatable({ entity = entity, x = 0 }, Breaker)\n"            // 4
        u8"end\n"                                                                     // 5
        u8"function Breaker:onUpdate(dt)\n"                                           // 6
        u8"    self.x = self.x + 1\n"                    // 7  <- breakpoint
        u8"    self.entity:setPosition(self.x, 0, 0)\n"  // 8
        u8"end\n",                                       // 9
        {u8"onUpdate"});
    breaker->sourceName = String(u8"Breaker.luau");

    bed.host.RequestDebugger(Function<void(IScriptDebugger&)>{});
    const scene::EntityHandle e = bed.AddScripted(breaker, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate + onUpdate (no breakpoint yet): x = 1
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    IScriptDebugger* debugger = bed.host.Debugger();
    REQUIRE(debugger != nullptr);
    CHECK_FALSE(bed.host.IsDebugPaused());
    debugger->SetBreakpoint(u8"Breaker.luau", 7);

    // Next tick: onUpdate hits the breakpoint and suspends: paused, world frozen, not faulted.
    bed.Frame();
    CHECK(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);

    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 7);
    CHECK(StringView(frames[0].file) == u8"Breaker.luau"); // the per-class chunk is the source file

    // Frozen while paused.
    bed.Frame();
    CHECK(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    // Continue runs the handler to completion: setPosition uses the incremented x, position 2.
    debugger->Continue();
    CHECK_FALSE(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));

    debugger->RemoveBreakpoint(u8"Breaker.luau", 7);
    bed.Frame();
    CHECK_FALSE(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 3.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);
}

// A breakpoint in a handler reached through a nested entity.send: the sender's onUpdate is
// mid-send (a C facade boundary) on its pooled thread when the receiver's onPing breaks on a second
// pooled thread (pool depth equals nesting depth). The game pauses; Continue completes the nested
// handler. The executor and debugger survive re-entrant dispatch across the C boundary.
TEST_CASE("script.scene: LUAU a breakpoint in a nested entity.send handler pauses on a second thread")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> receiver = MakeClassLang(
        u8"luau", u8"Receiver",
        u8"Receiver = {}\n"                                                              // 1
        u8"Receiver.__index = Receiver\n"                                                // 2
        u8"function Receiver.new(entity)\n"                                              // 3
        u8"    return setmetatable({ entity = entity }, Receiver)\n"                     // 4
        u8"end\n"                                                                        // 5
        u8"function Receiver:onPing(amount)\n"                                           // 6
        u8"    self.entity:setName(\"pinged:\" .. tostring(amount))\n" // 7  <- breakpoint
        u8"end\n",                                                     // 8
        {u8"onPing"});
    receiver->sourceName = String(u8"Receiver.luau");
    RefPtr<ScriptClass> sender = MakeClassLang(
        u8"luau", u8"Sender",
        u8"Sender = {}\n"
        u8"Sender.__index = Sender\n"
        u8"function Sender.new(entity) return setmetatable({ entity = entity, n = 6 }, Sender) end\n"
        u8"function Sender:onUpdate(dt)\n"
        u8"    self.n = self.n + 1\n"
        u8"    self.entity:send(\"ping\", self.n)\n"
        u8"end\n",
        {u8"onUpdate"});
    sender->sourceName = String(u8"Sender.luau");

    scene::EntityHandle target = bed.scene.CreateEntity(u8"target");
    ScriptComponent& component = bed.components->Add(target);
    {
        ScriptBehavior b;
        b.script = receiver;
        component.behaviors.PushBack(Move(b));
    }
    {
        ScriptBehavior b;
        b.script = sender;
        component.behaviors.PushBack(Move(b));
    }

    bed.host.RequestDebugger(Function<void(IScriptDebugger&)>{});
    bed.Start();
    bed.Frame(); // n=7: send ping 7 -> onPing(7) sets the name (no breakpoint yet)
    CHECK(bed.scene.GetEntityName(target) == StringView(u8"pinged:7"));

    IScriptDebugger* debugger = bed.host.Debugger();
    REQUIRE(debugger != nullptr);
    debugger->SetBreakpoint(u8"Receiver.luau", 7);

    // Next tick: sender.onUpdate sends ping 8; the nested onPing breaks before setName. The game
    // pauses, the name still reads the previous frame's value, and the innermost frame is onPing.
    bed.Frame();
    CHECK(bed.host.IsDebugPaused());
    CHECK(bed.scene.GetEntityName(target) == StringView(u8"pinged:7"));
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 7);
    CHECK(StringView(frames[0].file) == u8"Receiver.luau");

    // Continue completes the nested handler: setName runs with amount 8.
    debugger->Continue();
    CHECK_FALSE(bed.host.IsDebugPaused());
    CHECK(bed.scene.GetEntityName(target) == StringView(u8"pinged:8"));
}

// Bytecode consumption end to end through the run host: a behaviour ScriptClass carrying only
// cooked bytecode (source stripped, as the player has it) instantiates and ticks. The run host
// threads ScriptClass::bytecode into LoadBehaviorModule and the backend loads it with no compiler.
TEST_CASE("script.scene: LUAU a behavior runs from cooked bytecode with no source (player path)")
{
    foundation::script::RegisterLuauScriptBackend();
    ScriptedScene bed;

    RefPtr<ScriptClass> mover = MakeClassLang(
        u8"luau", u8"Mover",
        u8"Mover = {}\n"
        u8"Mover.__index = Mover\n"
        u8"function Mover.new(entity) return setmetatable({ entity = entity, x = 0 }, Mover) end\n"
        u8"function Mover:onUpdate(dt)\n"
        u8"    self.x = self.x + 1\n"
        u8"    self.entity:setPosition(self.x, 0, 0)\n"
        u8"end\n",
        {u8"onUpdate"});
    mover->sourceName = String(u8"Mover.luau");

    // Cook the source to bytecode (a standalone Luau manager, as the cook does), store it on the
    // class, then strip the source: only the bytecode can drive the behaviour now, as in the player.
    RefPtr<IScriptManager> cooker = foundation::script::CreateLuauScriptManager(DefaultAllocator());
    Result<RefPtr<IScriptBlob>> blob = cooker->CompileToBlob(mover->source.AsView(), u8"Mover.luau");
    REQUIRE(blob.HasValue());
    MemoryStream cooked;
    {
        BinarySerializer w(cooked, SerializeMode::Write);
        blob.Value()->Serialize(w);
    }
    const Span<const byte> bytes = cooked.Bytes();
    mover->bytecode.Reserve(bytes.Size());
    for (usize i = 0; i < bytes.Size(); ++i)
    {
        mover->bytecode.PushBack(bytes[i]);
    }
    mover->source = String{}; // the player ships no source

    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate and onUpdate, all from bytecode
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);
}

// AngelScript bytecode consumption through the run host: the cook produces a single-class module
// blob (SaveByteCode); the run host loads it into its own module with LoadByteCode, and
// CreateInstance/FindFunction find the class across owned modules. A behaviour with its source
// stripped instantiates and ticks from bytecode alone (the player path, no Build()).
TEST_CASE("script.scene: ANGELSCRIPT a behavior runs from cooked bytecode with no source (player path)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;

    // Bring up the AngelScript run context first (EnsureContext registers the reflected surface and
    // facades on its engine) so we can cook an Entity-using behaviour on the same engine that will
    // load it with LoadByteCode: AngelScript bytecode is bound to the engine's type registration.
    REQUIRE(bed.host.EnsureContext(u8"angelscript") != nullptr);
    IScriptManager* manager = bed.host.Manager();
    REQUIRE(manager != nullptr);

    const StringView moverSource =
        u8"class Mover {\n"
        u8"    private Entity@ self;\n"
        u8"    private float x;\n"
        u8"    Mover(Entity@ entity) { @self = entity; x = 0.0f; }\n"
        u8"    void onUpdate(double dt) {\n"
        u8"        x = x + 1.0f;\n"
        u8"        self.setPosition(x, 0.0f, 0.0f);\n"
        u8"    }\n"
        u8"}\n";
    Result<RefPtr<IScriptBlob>> blob = manager->CompileToBlob(moverSource, u8"Mover.as");
    REQUIRE(blob.HasValue());
    MemoryStream cooked;
    {
        BinarySerializer w(cooked, SerializeMode::Write);
        blob.Value()->Serialize(w);
    }
    const Span<const byte> bytes = cooked.Bytes();

    RefPtr<ScriptClass> mover = MakeClassLang(u8"angelscript", u8"Mover", u8"", {u8"onUpdate"});
    mover->sourceName = String(u8"Mover.as");
    mover->bytecode.Reserve(bytes.Size());
    for (usize i = 0; i < bytes.Size(); ++i)
    {
        mover->bytecode.PushBack(bytes[i]);
    }
    // source stays empty, so only the bytecode can drive it.

    const scene::EntityHandle e = bed.AddScripted(mover, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate and onUpdate, all from bytecode (LoadByteCode, never Build)
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);
}

// The cook keeps debug info and the sourceName section in the AngelScript blob, so a breakpoint
// still lines up on a behaviour loaded from bytecode. Pins the stripDebugInfo=false and
// section=sourceName choices that make consumed bytecode debuggable.
TEST_CASE("script.scene: ANGELSCRIPT a breakpoint works on a behavior loaded from bytecode (debug info survives)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    REQUIRE(bed.host.EnsureContext(u8"angelscript") != nullptr);
    IScriptManager* manager = bed.host.Manager();
    REQUIRE(manager != nullptr);

    const StringView breakerSource =
        u8"class Breaker {\n"                                           // 1
        u8"    private Entity@ self;\n"                                 // 2
        u8"    private float x;\n"                                      // 3
        u8"    Breaker(Entity@ entity) { @self = entity; x = 0.0f; }\n" // 4
        u8"    void onUpdate(double dt) {\n"                            // 5
        u8"        x = x + 1.0f;\n"                    // 6  <- breakpoint
        u8"        self.setPosition(x, 0.0f, 0.0f);\n" // 7
        u8"    }\n"                                    // 8
        u8"}\n";                                       // 9
    Result<RefPtr<IScriptBlob>> blob = manager->CompileToBlob(breakerSource, u8"Breaker.as");
    REQUIRE(blob.HasValue());
    MemoryStream cooked;
    {
        BinarySerializer w(cooked, SerializeMode::Write);
        blob.Value()->Serialize(w);
    }
    const Span<const byte> bytes = cooked.Bytes();

    RefPtr<ScriptClass> breaker = MakeClassLang(u8"angelscript", u8"Breaker", u8"", {u8"onUpdate"});
    breaker->sourceName = String(u8"Breaker.as");
    breaker->bytecode.Reserve(bytes.Size());
    for (usize i = 0; i < bytes.Size(); ++i)
    {
        breaker->bytecode.PushBack(bytes[i]);
    }

    bed.host.RequestDebugger(Function<void(IScriptDebugger&)>{});
    const scene::EntityHandle e = bed.AddScripted(breaker, u8"walker");
    bed.Start();
    bed.Frame(); // x = 1 (no breakpoint yet)
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    IScriptDebugger* debugger = bed.host.Debugger();
    REQUIRE(debugger != nullptr);
    debugger->SetBreakpoint(u8"Breaker.as", 6);
    bed.Frame(); // onUpdate hits the breakpoint: paused; the section is the sourceName
    CHECK(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // frozen before the increment
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 6);
    CHECK(StringView(frames[0].file) == u8"Breaker.as"); // debug info and section survived the cook

    debugger->Continue();
    CHECK_FALSE(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
}

// A harvested AngelScript editor property (a member field) reaches the instance through the
// neutral setter Invoke: the subsystem invokes `<name>=`, which the AngelScript backend writes to
// the same-named member field. Both the default and a hash-keyed override drive it.

TEST_CASE("script.scene: an AngelScript harvested float property applies (default + override) "
          "and drives onUpdate")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();

    auto makeSpeeder = []()
    {
        return MakeClassLang(
            u8"angelscript", u8"Speeder",
            u8"class Speeder {\n"
            u8"    private Entity@ self;\n"
            u8"    private float x;\n"
            u8"    float speed;\n" // harvested editor property (member field)
            u8"    Speeder(Entity@ entity) { @self = entity; x = 0.0f; speed = 0.0f; }\n"
            u8"    void onUpdate(double dt) { x = x + speed * float(dt); "
            u8"self.setPosition(x, 0.0f, 0.0f); }\n"
            u8"}\n",
            {u8"onUpdate"}, {FloatProperty(u8"speed", 2.0)});
    };

    SUBCASE("the harvested default applies")
    {
        ScriptedScene bed;
        const scene::EntityHandle e = bed.AddScripted(makeSpeeder(), u8"walker");
        bed.Start();
        bed.Frame(); // default speed 2.0 applied (overrides the ctor's 0.0): x = 2.0 * 0.5
        CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
        CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);
    }

    SUBCASE("a hash-keyed override wins over the default")
    {
        ScriptedScene bed;
        const scene::EntityHandle e = bed.AddScripted(makeSpeeder(), u8"runner");
        ScriptPropertyValue five;
        five.kind = ScriptPropertyType::Float;
        five.number = 5.0;
        bed.components->Get(e)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"speed"), five);
        bed.Start();
        bed.Frame(); // override speed 5.0: x = 5.0 * 0.5
        CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.5f));
        CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);
    }
}

// ---- Coroutines: the coroutine `Behavior` base and the host scheduler, driven by the subsystem
// tick (AdvanceCoroutines once per frame), and cancelled on disable or destroy.

TEST_CASE("script.scene: a coroutine wait(1.0) runs its body only after ~1s of ticks")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> waiter = MakeClass(u8"Waiter",
                                           u8"class Waiter {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Waiter(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() { startCoroutine(ScriptCoroutine(this.body)); }\n"
                                           u8"    void body() {\n"
                                           u8"        wait(1.0f);\n"
                                           u8"        self.setPosition(5.0f, 0.0f, 0.0f);\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    waiter->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(waiter, u8"w");
    bed.Start();
    bed.Frame(0.5f); // onStart registers wait 1.0; +0.5s -> still pending
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // +0.5s reaches 1.0s; the resume runs finish()
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
    bed.Frame(0.5f); // completed coroutine does not run again
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
}

TEST_CASE("script.scene: a coroutine waitUntil resumes when the predicate flips")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> gater = MakeClass(u8"Gater",
                                          u8"class Gater {\n"
                                          u8"    private Entity@ self;\n"
                                          u8"    private bool open;\n"
                                          u8"    private int ticks;\n"
                                          u8"    Gater(Entity@ entity) {\n"
                                          u8"        @self = entity;\n"
                                          u8"        open = false;\n"
                                          u8"        ticks = 0;\n"
                                          u8"    }\n"
                                          u8"    void onStart() { startCoroutine(ScriptCoroutine(this.body)); }\n"
                                          u8"    void body() {\n"
                                          u8"        Coroutine::waitUntil(CoroutinePredicate(this.isOpen));\n"
                                          u8"        self.setPosition(9.0f, 0.0f, 0.0f);\n"
                                          u8"    }\n"
                                          u8"    void onUpdate(double dt) {\n"
                                          u8"        ticks = ticks + 1;\n"
                                          u8"        if (ticks >= 3) { open = true; }\n"
                                          u8"    }\n"
                                          u8"    bool isOpen() { return open; }\n"
                                          u8"}\n",
                                          {u8"onStart", u8"onUpdate"});
    gater->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(gater, u8"g");
    bed.Start();
    bed.Frame(0.5f); // tick 1, gate closed
    bed.Frame(0.5f); // tick 2, gate closed
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // tick 3 opens the gate; waitUntil resumes and runs finish()
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 9.0f));
}

TEST_CASE("script.scene: destroying a behavior cancels its pending coroutine (never fires)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ghost = MakeClass(u8"Ghost",
                                          u8"double GhostFired = 0;\n"
                                          u8"class Ghost {\n"
                                          u8"    private Entity@ self;\n"
                                          u8"    Ghost(Entity@ entity) { @self = entity; }\n"
                                          u8"    void onStart() { startCoroutine(ScriptCoroutine(this.body)); }\n"
                                          u8"    void body() {\n"
                                          u8"        wait(1.0f);\n"
                                          u8"        fire();\n"
                                          u8"    }\n"
                                          u8"    void fire() { GhostFired = GhostFired + 1; }\n"
                                          u8"}\n",
                                          {u8"onStart"});
    ghost->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(ghost, u8"ghost");
    bed.Start();
    bed.Frame(0.5f);            // registers wait 1.0; +0.5s pending
    bed.scene.DestroyEntity(e); // onDestroy path cancels the coroutine
    bed.Frame(0.5f);
    bed.Frame(0.5f);
    bed.Frame(0.5f); // well past 1.0s: a cancelled coroutine does not run

    const Variant fired = bed.host.ScriptContext()->GetGlobal(u8"GhostFired");
    REQUIRE(fired.TryGet<f64>() != nullptr);
    CHECK(*fired.TryGet<f64>() == 0.0);
}

TEST_CASE("script.scene: disabling a behavior cancels its pending coroutine (never fires)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ghost = MakeClass(u8"Sleeper",
                                          u8"double SleeperFired = 0;\n"
                                          u8"class Sleeper {\n"
                                          u8"    private Entity@ self;\n"
                                          u8"    Sleeper(Entity@ entity) { @self = entity; }\n"
                                          u8"    void onStart() { startCoroutine(ScriptCoroutine(this.body)); }\n"
                                          u8"    void body() {\n"
                                          u8"        wait(1.0f);\n"
                                          u8"        fire();\n"
                                          u8"    }\n"
                                          u8"    void onUpdate(double dt) {}\n"
                                          u8"    void fire() { SleeperFired = SleeperFired + 1; }\n"
                                          u8"}\n",
                                          {u8"onStart", u8"onUpdate"});
    ghost->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(ghost, u8"s");
    bed.Start();
    bed.Frame(0.5f); // registers wait 1.0; +0.5s pending
    bed.components->Get(e)->behaviors[0].enabled = false;
    bed.Frame(0.5f); // onDisable edge cancels the coroutine
    bed.Frame(0.5f);
    bed.Frame(0.5f); // past 1.0s: does not fire

    const Variant fired = bed.host.ScriptContext()->GetGlobal(u8"SleeperFired");
    REQUIRE(fired.TryGet<f64>() != nullptr);
    CHECK(*fired.TryGet<f64>() == 0.0);
}

// ---- Physics contact events reach behaviours (end to end, real Jolt and a real Scene) ----
// A full subsystem manager wiring SceneSubsystem, PhysicsSubsystem and ScriptSubsystem: the
// physics tick resolves contacts to entities and pushes them to the script subsystem (registered
// as an IContactListener at OnReady), which enqueues them onto the owning scene's deferred queue;
// the scene tick drains them and dispatches the on<Event> handler.

namespace
{
    namespace runtime = foundation::runtime;

    // Builds a SubsystemManager with all three subsystems started and a live scene, and returns the
    // scene. It acts as its own composition root: it bridges physics contacts to the script
    // subsystem's neutral DeliverContact ingress with the same adapter DefaultApplication uses in a
    // real run (engine.integration), so these contact tests also exercise that bridge, and the
    // script subsystem itself keeps no physics dependency.
    struct ContactWorld
    {
        runtime::SubsystemManager ctx{DefaultAllocator()};
        engine::scene::SceneSubsystem* scenes = nullptr;
        engine::physics::PhysicsSubsystem* physics = nullptr;
        ScriptSubsystem* scripts = nullptr;
        scene::Scene* scene = nullptr;
        UniquePtr<scene::SceneManager> sm; // this bed's scene group (subsystem owns none)
        engine::integration::ScriptPhysicsContactBridge bridge; // the composition-root adapter

        ContactWorld()
        {
            foundation::script::angelscript::RegisterAngelScriptBackend();
            foundation::script::RegisterLuauScriptBackend();
            RegisterCoreTypes();
            engine::physics::RegisterPhysicsComponentReflection();
            RegisterScriptComponentReflection();
            RegisterScriptFacadeReflection();
            scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
            physics = ctx.AddSubsystem<engine::physics::PhysicsSubsystem>();
            scripts = ctx.AddSubsystem<ScriptSubsystem>();
            {
                const scene::SceneModule scriptModule{u8"script",
                                                      &engine::script::AddScriptSceneManagers,
                                                      nullptr};
                const scene::SceneModule physicsModule{u8"physics",
                                                       &engine::physics::AddPhysicsSceneManagers,
                                                       nullptr};
                const scene::SceneModule* modules[] = {&scriptModule, &physicsModule};
                scenes->SetComposition(scene::SceneComposition::Build(modules));
            }
            ctx.Startup();
            bridge.Install(*physics, *scripts); // the real composition-root adapter
            sm = MakeUnique<scene::SceneManager>(DefaultAllocator(), DefaultAllocator());
            scenes->RegisterManager(sm.Get());
            scene = sm->CreateScene(u8"level");
        }
        ~ContactWorld()
        {
            bridge.Uninstall(); // before ctx.Shutdown destroys the physics subsystem
            ctx.Shutdown();
        }

        scene::EntityHandle AddBody(StringView name, Float3 position, physics::MotionKind motion,
                                    Float3 halfExtents, bool trigger = false)
        {
            scene::EntityHandle e = scene->CreateEntity(name);
            scene->SetLocalPosition(e, position);
            auto& body = scene->GetSystem<engine::physics::RigidBodyComponentManager>()->Add(e);
            body.motion = motion;
            body.layer = motion == physics::MotionKind::Static ? physics::PhysicsLayer::Static
                         : motion == physics::MotionKind::Kinematic
                             ? physics::PhysicsLayer::Kinematic
                             : physics::PhysicsLayer::Dynamic;
            body.halfExtents = halfExtents;
            body.isTrigger = trigger;
            return e;
        }

        void Attach(scene::EntityHandle e, const RefPtr<ScriptClass>& cls)
        {
            auto& component = scene->GetSystem<ScriptComponentManager>()->Add(e);
            ScriptBehavior behavior;
            behavior.script = cls;
            component.behaviors.PushBack(Move(behavior));
        }

        void Play(int frames)
        {
            scene->UpdateTransforms();
            scene->Start();
            scene->SetSimulationEnabled(true);
            for (int i = 0; i < frames; ++i)
            {
                ctx.BeginFrame(1.0f / 60.0f); // fixed steps: physics contacts are enqueued for scripts
                ctx.Update(1.0f / 60.0f);     // scene tick: drain and dispatch to behaviours
            }
        }
    };
}

TEST_CASE("script.scene: a physics collision dispatches onContactBegin(other, point, normal, "
          "speed) to the behavior on the colliding entity")
{
    ContactWorld world;
    const scene::EntityHandle floor = world.AddBody(
        u8"floor", Float3{0, -0.5f, 0}, physics::MotionKind::Static, Float3{50, 0.5f, 50});
    (void)floor;
    const scene::EntityHandle box = world.AddBody(
        u8"box", Float3{0, 1.4f, 0}, physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});

    // Records the other entity's name only if speed is non-negative and the normal is about unit
    // length, so all four arguments crossed the boundary intact.
    RefPtr<ScriptClass> bumper = MakeClass(
        u8"Bumper",
        u8"class Bumper {\n"
        u8"    private Entity@ self;\n"
        u8"    Bumper(Entity@ entity) { @self = entity; }\n"
        u8"    void onContactBegin(Entity@ other, Float3@ point, Float3@ normal, float speed) {\n"
        u8"        float len = normal.x*normal.x + normal.y*normal.y + normal.z*normal.z;\n"
        u8"        if (speed >= 0.0f && len > 0.5f) { self.setName(\"hit:\" + other.name()); }\n"
        u8"    }\n"
        u8"}\n",
        {u8"onContactBegin"});
    world.Attach(box, bumper);

    world.Play(180);

    CHECK(world.scene->GetEntityName(box) == StringView(u8"hit:floor"));
}

TEST_CASE("script.scene: a physics trigger dispatches onTriggerEnter(other) to a behavior")
{
    ContactWorld world;
    (void)world.AddBody(u8"floor", Float3{0, -0.5f, 0}, physics::MotionKind::Static,
                        Float3{50, 0.5f, 50});
    // A kinematic sensor volume with a behavior; a box falls through it.
    const scene::EntityHandle volume =
        world.AddBody(u8"volume", Float3{0, 2.0f, 0}, physics::MotionKind::Kinematic,
                      Float3{1, 1, 1}, /*trigger*/ true);
    const scene::EntityHandle faller = world.AddBody(
        u8"faller", Float3{0, 6.0f, 0}, physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    (void)faller;

    RefPtr<ScriptClass> sensor =
        MakeClass(u8"Sensor",
                  u8"class Sensor {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Sensor(Entity@ entity) { @self = entity; }\n"
                  u8"    void onTriggerEnter(Entity@ other) { self.setName(\"sensed:\" + other.name()); }\n"
                  u8"}\n",
                  {u8"onTriggerEnter"});
    world.Attach(volume, sensor);

    world.Play(240);

    CHECK(world.scene->GetEntityName(volume) == StringView(u8"sensed:faller"));
}

TEST_CASE("script.scene: LUAU physics onContactBegin(other, point, normal, speed) - all four "
          "args cross to a Luau behavior")
{
    ContactWorld world;
    const scene::EntityHandle floor = world.AddBody(
        u8"floor", Float3{0, -0.5f, 0}, physics::MotionKind::Static, Float3{50, 0.5f, 50});
    (void)floor;
    const scene::EntityHandle box = world.AddBody(
        u8"box", Float3{0, 1.4f, 0}, physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});

    // `normal` and `point` are Float3 native vectors; `other` is an Entity handle; `speed` an f64.
    // The whole four-argument contact payload marshals through the Luau metatable dispatch.
    RefPtr<ScriptClass> bumper = MakeClassLang(
        u8"luau", u8"Bumper",
        u8"Bumper = {}\n"
        u8"Bumper.__index = Bumper\n"
        u8"function Bumper.new(entity) return setmetatable({ entity = entity }, Bumper) end\n"
        u8"function Bumper:onContactBegin(other, point, normal, speed)\n"
        u8"    local len = normal.x*normal.x + normal.y*normal.y + normal.z*normal.z\n"
        u8"    if speed >= 0 and len > 0.5 then self.entity:setName(\"hit:\" .. other:name()) end\n"
        u8"end\n",
        {u8"onContactBegin"});
    world.Attach(box, bumper);

    world.Play(180);

    CHECK(world.scene->GetEntityName(box) == StringView(u8"hit:floor"));
}

TEST_CASE("script.scene: behaviors tick without error when no physics subsystem is present "
          "(contact-listener registration is guarded)")
{
    namespace runtime = foundation::runtime;
    runtime::SubsystemManager ctx(foundation::core::DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    {
        const scene::SceneModule scriptModule{u8"script", &engine::script::AddScriptSceneManagers,
                                              nullptr};
        const scene::SceneModule* modules[] = {&scriptModule};
        scenes->SetComposition(scene::SceneComposition::Build(modules));
    }
    ctx.AddSubsystem<ScriptSubsystem>(); // no physics subsystem
    foundation::script::angelscript::RegisterAngelScriptBackend();
    RegisterCoreTypes();
    RegisterScriptComponentReflection();
    RegisterScriptFacadeReflection();
    ctx.Startup(); // OnReady must not crash resolving the (absent) physics subsystem

    scene::Scene* scene = sm.CreateScene(u8"no-physics");
    RefPtr<ScriptClass> mover = MakeClass(u8"Mover",
                                          u8"class Mover {\n"
                                          u8"    private Entity@ self;\n"
                                          u8"    Mover(Entity@ entity) { @self = entity; }\n"
                                          u8"    void onUpdate(double dt) {\n"
                                          u8"        Float3 p = self.position();\n"
                                          u8"        self.setPosition(p.x + 1.0f, p.y, p.z);\n"
                                          u8"    }\n"
                                          u8"}\n",
                                          {u8"onUpdate"});
    scene::EntityHandle e = scene->CreateEntity(u8"m");
    {
        auto& component = scene->GetSystem<ScriptComponentManager>()->Add(e);
        ScriptBehavior behavior;
        behavior.script = mover;
        component.behaviors.PushBack(Move(behavior));
    }
    scene->Start();
    scene->SetSimulationEnabled(true);
    ctx.BeginFrame(1.0f / 60.0f);
    ctx.Update(1.0f / 60.0f);
    CHECK(Near(scene->GetLocalTransform(e).position.x, 1.0f));

    ctx.Shutdown();
}

// ---- Scene-facade safety. The bound Scene carries its own scene pointer, so there is no ambient
//      "current scene" to misroute: every call site (update, onDestroy, a resumed coroutine,
//      cross-scene) targets the right scene by construction. These pin the paths where an ambient
//      current scene would be stale or null. ----

TEST_CASE("script-facade: entity.scene() is bound to the entity's OWN scene (cross-scene isolation)")
{
    // A pure facade test with no VM. Two scenes; an entity of A resolves and finds in A, never B.
    scene::Scene a(DefaultAllocator(), u8"A");
    scene::Scene b(DefaultAllocator(), u8"B");
    const scene::EntityHandle ea = a.CreateEntity(u8"ea");
    const scene::EntityHandle eb = b.CreateEntity(u8"eb");
    a.CreateEntity(u8"target"); // only in A

    const Entity wa = WrapEntity(&a, ea);
    const Entity wb = WrapEntity(&b, eb);

    CHECK(wa.sceneHandle().scene == &a); // bound to its own scene...
    CHECK(wb.sceneHandle().scene == &b); // ...regardless of any ambient state (there is none)

    // find() targets the entity's scene: A's handle finds "target"; B's handle does not.
    CHECK(wa.sceneHandle().find(String(u8"target")).Live());
    CHECK_FALSE(wb.sceneHandle().find(String(u8"target")).Live());
}

TEST_CASE("script.scene: entity.scene.spawn works from onDestroy - a FORMER footgun")
{
    ScriptedScene bed;
    PrefabSource prefabs(bed);

    RefPtr<ScriptClass> dier =
        MakeClass(u8"Dier",
                  u8"class Dier {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ prefab;\n"
                  u8"    Dier(Entity@ entity) { @self = entity; }\n"
                  u8"    void onDestroy() { self.scene.spawn(prefab, 1.0f, 2.0f, 3.0f); }\n"
                  u8"}\n",
                  {u8"onDestroy"});
    dier->properties.PushBack(prefabs.Property());

    const scene::EntityHandle e = bed.AddScripted(dier, u8"dier");
    bed.Start();
    bed.Frame(); // instantiate + (no onStart handler)
    CHECK(PrefabSource::Spawned(bed.scene) == 0u);

    bed.scene.DestroyEntity(e);
    bed.Frame(); // removal detected: onDestroy spawns into the entity's own scene
    CHECK(PrefabSource::Spawned(bed.scene) == 1u); // the entity's scene, from the bound handle
    const scene::EntityHandle spawned = bed.scene.FindEntityByName(u8"Turret");
    REQUIRE(spawned.IsAssigned());
    CHECK(Near(bed.scene.GetLocalTransform(spawned).position.y, 2.0f));
}

TEST_CASE("script.scene: entity.scene.spawn works from a resumed coroutine - a FORMER footgun")
{
    ScriptedScene bed;
    PrefabSource prefabs(bed);

    RefPtr<ScriptClass> spawner =
        MakeClass(u8"CoroSpawner",
                  u8"class CoroSpawner {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ prefab;\n"
                  u8"    CoroSpawner(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() { startCoroutine(ScriptCoroutine(this.body)); }\n"
                  u8"    void body() {\n"
                  u8"        wait(1.0f);\n"
                  u8"        self.scene.spawn(prefab, 7.0f, 8.0f, 9.0f);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    spawner->usesCoroutines = true;
    spawner->properties.PushBack(prefabs.Property());

    (void)bed.AddScripted(spawner, u8"cs");
    bed.Start();
    bed.Frame(0.5f); // registers wait(1.0); +0.5s -> pending, no spawn
    CHECK(PrefabSource::Spawned(bed.scene) == 0u);
    bed.Frame(0.5f); // +0.5s reaches 1.0s; the coroutine resumes and spawns
    CHECK(PrefabSource::Spawned(bed.scene) == 1u); // resumed outside any tick, still the right scene
    CHECK(Near(bed.scene.GetLocalTransform(bed.scene.FindEntityByName(u8"Turret")).position.x, 7.0f));
}

TEST_CASE("script.scene: an AngelScript behavior spawns through self.scene() (bound Scene, 2nd backend)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    PrefabSource prefabs(bed);

    // The id is spelled in source as its two halves (the Guid value constructor).
    char literal[64];
    std::snprintf(literal, sizeof literal, "Guid(0x%llx, 0x%llx)",
                  static_cast<unsigned long long>(prefabs.turret.high),
                  static_cast<unsigned long long>(prefabs.turret.low));
    String source(u8"class Spawner {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Spawner(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() { self.scene.spawn(");
    source.Append(StringView(reinterpret_cast<const utf8char*>(literal)));
    source.Append(u8", 1.0f, 0.0f, 0.0f); }\n}\n");
    RefPtr<ScriptClass> spawner =
        MakeClassLang(u8"angelscript", u8"Spawner", source.AsView(), {u8"onStart"});

    (void)bed.AddScripted(spawner, u8"as");
    bed.Start();
    bed.Frame();
    // The bound Scene value carried the entity's scene through AngelScript: the spawn landed here.
    CHECK(PrefabSource::Spawned(bed.scene) == 1u);
    CHECK(Near(bed.scene.GetLocalTransform(bed.scene.FindEntityByName(u8"Turret")).position.x, 1.0f));
}

// ---- Scene-level scripting (the third tier): one `Level` object per scene, constructed with the
//      scene's bound handle, gated on simulation, dispatched onStart/onUpdate(dt)/onFixedUpdate(dt)/onStop. ----

TEST_CASE("script.scene: a Level runs onStart/onUpdate/onStop through its bound scene, sim-gated")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    // Sentinels the Level manipulates through its constructor-injected bound scene.
    const scene::EntityHandle flag = bed.scene.CreateEntity(u8"flag");
    const scene::EntityHandle ticker = bed.scene.CreateEntity(u8"ticker"); // x = onUpdate count

    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onStart() { scene.find(\"flag\").setName(\"started\"); }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Entity@ e = scene.find(\"ticker\");\n"
                  u8"        Float3 p = e.position();\n"
                  u8"        e.setPosition(p.x + 1.0f, 0.0f, 0.0f);\n"
                  u8"    }\n"
                  u8"    void onStop() { scene.find(\"started\").setName(\"stopped\"); }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate", u8"onStop"});
    level->Settings().script = levelClass;

    bed.Start(); // OnSceneStarted: construct new(scene), then onStart
    CHECK(level->LevelActive());
    CHECK(bed.scene.GetEntityName(flag) == StringView(u8"started")); // onStart ran via bound scene

    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 1.0f));
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 2.0f));

    bed.scene.SetSimulationEnabled(false);
    bed.Frame(); // gated on simulation: no onUpdate
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 2.0f));
    bed.scene.SetSimulationEnabled(true);
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 3.0f));

    bed.scene.Stop(); // OnSceneStopped: onStop, then destroy
    CHECK(bed.scene.GetEntityName(flag) == StringView(u8"stopped"));
    CHECK_FALSE(level->LevelActive());
}

TEST_CASE("script.scene: a Level's harvested property default is applied before onStart")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);
    const scene::EntityHandle probe = bed.scene.CreateEntity(u8"probe");

    // The Level reads its declared `timeLimit` in onStart and stamps it into a bound-scene entity,
    // so the test can read back the applied value. With no override the harvested default (90) applies.
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    float timeLimit;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onStart() { scene.find(\"probe\").setPosition(timeLimit, 0.0f, 0.0f); }\n"
                  u8"}\n",
                  {u8"onStart"}, {FloatProperty(u8"timeLimit", 90.0)});
    level->Settings().script = levelClass;

    bed.Start(); // construct, apply timeLimit=90 (default), onStart
    CHECK(level->LevelActive());
    CHECK(Near(bed.scene.GetLocalTransform(probe).position.x, 90.0f)); // default reached the field
}

TEST_CASE("script.scene: a Level's hash-keyed property override wins over the harvested default")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);
    const scene::EntityHandle probe = bed.scene.CreateEntity(u8"probe");

    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    float timeLimit;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onStart() { scene.find(\"probe\").setPosition(timeLimit, 0.0f, 0.0f); }\n"
                  u8"}\n",
                  {u8"onStart"}, {FloatProperty(u8"timeLimit", 90.0)});
    level->Settings().script = levelClass;

    // The scene-settings override (the inspector-authored value) wins over the class default.
    ScriptPropertyValue five;
    five.kind = ScriptPropertyType::Float;
    five.number = 5.0;
    level->Settings().SetOverride(ScriptPropertyNameHash(u8"timeLimit"), five);

    bed.Start();
    CHECK(Near(bed.scene.GetLocalTransform(probe).position.x, 5.0f)); // override reached the field
}

TEST_CASE("script.scene: SceneScriptSettings overrides round-trip through a v2 versioned payload")
{
    // The machinery the editor's settings-block command and the scene-file path use:
    // BeginVersionedPayload(TypeOf<SceneScriptSettings>) writes the v2 chain, the Serialize body's
    // `ar.Version() >= 2` gate lets the overrides through, and the read reconstructs them.
    RegisterScriptComponentReflection(); // patches DataVersion(2) onto the TypeInfo

    SceneScriptSettings out;
    out.enabled = true;
    ScriptPropertyValue value;
    value.kind = ScriptPropertyType::Float;
    value.number = 42.0;
    out.SetOverride(ScriptPropertyNameHash(u8"timeLimit"), value);

    MemoryStream buffer;
    {
        BinarySerializer w(buffer, SerializeMode::Write);
        BeginVersionedPayload(w, TypeOf<SceneScriptSettings>());
        SerializeSceneScriptSettings(w, out);
        EndVersionedPayload(w);
        REQUIRE(w.IsOk());
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    SceneScriptSettings in;
    {
        BinarySerializer r(buffer, SerializeMode::Read);
        BeginVersionedPayload(r, TypeOf<SceneScriptSettings>());
        SerializeSceneScriptSettings(r, in);
        EndVersionedPayload(r);
        REQUIRE(r.IsOk());
    }
    const ScriptPropertyOverride* over = in.FindOverride(ScriptPropertyNameHash(u8"timeLimit"));
    REQUIRE(over != nullptr);
    CHECK(over->value.number == doctest::Approx(42.0));
    CHECK(in.enabled);
}

TEST_CASE("script.scene: a Level receives onFixedUpdate on the fixed lane")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);
    const scene::EntityHandle f = bed.scene.CreateEntity(u8"f");

    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onFixedUpdate(double dt) {\n"
                  u8"        Entity@ e = scene.find(\"f\");\n"
                  u8"        e.setPosition(e.position().x + 1.0f, 0.0f, 0.0f);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onFixedUpdate"});
    level->Settings().script = levelClass;

    bed.Start();
    bed.scene.FixedUpdate(1.0f / 60.0f); // one fixed step runs onFixedUpdate
    CHECK(Near(bed.scene.GetLocalTransform(f).position.x, 1.0f));
    bed.scene.FixedUpdate(1.0f / 60.0f);
    CHECK(Near(bed.scene.GetLocalTransform(f).position.x, 2.0f));

    bed.scene.SetSimulationEnabled(false);
    bed.scene.FixedUpdate(1.0f / 60.0f); // gated on simulation
    CHECK(Near(bed.scene.GetLocalTransform(f).position.x, 2.0f));
}

TEST_CASE("script.scene: a faulting Level handler disables THAT scene's level only")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);
    const scene::EntityHandle t = bed.scene.CreateEntity(u8"t");

    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        scene.find(\"t\").setName(\"ticked\");\n"
                  u8"        int zero = 0; int boom = 10 / zero;\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    level->Settings().script = levelClass;

    bed.Start();
    CHECK_FALSE(level->LevelFaulted());
    bed.Frame(); // onUpdate runs once (renames t) then faults
    CHECK(bed.scene.GetEntityName(t) == StringView(u8"ticked"));
    CHECK(level->LevelFaulted());

    bed.scene.SetEntityName(t, u8"reset");
    bed.Frame(); // faulted: no further dispatch
    CHECK(bed.scene.GetEntityName(t) == StringView(u8"reset"));
}

TEST_CASE("script.scene: two scenes sharing one Level class get INDEPENDENT objects (additive)")
{
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        Entity@ e = scene.find(\"ticker\");\n"
                  u8"        e.setPosition(e.position().x + 1.0f, 0.0f, 0.0f);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});

    ScriptedScene a;
    ScriptedScene b;
    SceneScriptSystem* la = a.scene.AddSystem<SceneScriptSystem>();
    SceneScriptSystem* lb = b.scene.AddSystem<SceneScriptSystem>();
    la->SetRunHost(&a.host);
    lb->SetRunHost(&b.host);
    const scene::EntityHandle ta = a.scene.CreateEntity(u8"ticker");
    const scene::EntityHandle tb = b.scene.CreateEntity(u8"ticker");
    la->Settings().script = levelClass;
    lb->Settings().script = levelClass;

    a.Start();
    b.Start();
    a.Frame();
    a.Frame(); // A ticks twice
    b.Frame(); // B ticks once; each Level targets its own scene
    CHECK(Near(a.scene.GetLocalTransform(ta).position.x, 2.0f));
    CHECK(Near(b.scene.GetLocalTransform(tb).position.x, 1.0f));
}

TEST_CASE("script.scene: an AngelScript Level runs the tier (2nd backend)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);
    const scene::EntityHandle flag = bed.scene.CreateEntity(u8"flag");

    RefPtr<ScriptClass> levelClass = MakeClassLang(
        u8"angelscript", u8"Level",
        u8"class Level {\n"
        u8"    private Scene@ scene;\n"
        u8"    Level(Scene@ s) { @scene = s; }\n"
        u8"    void onStart() { scene.find(\"flag\").setName(\"started\"); }\n"
        u8"}\n",
        {u8"onStart"});
    level->Settings().script = levelClass;

    bed.Start();
    CHECK(level->LevelActive());
    CHECK(bed.scene.GetEntityName(flag) == StringView(u8"started"));
}

TEST_CASE("script.scene: SceneScriptSettings round-trips its Level ref + enabled flag")
{
    SceneScriptSettings out;
    out.script.SetId(Guid{0xABCD, 0x1234});
    out.enabled = false;

    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        SerializeSceneScriptSettings(writer, out);
    }

    SceneScriptSettings in;
    in.enabled = true; // ensure the read actually overwrites
    {
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        SerializeSceneScriptSettings(reader, in);
    }

    CHECK(in.script.id == (Guid{0xABCD, 0x1234}));
    CHECK_FALSE(in.enabled);
}

// ---- End to end: Gadget.of(self).power = 5 mutates the live component (AngelScript). Covers the
// extra emission root (Gadget is emitted though no signature reaches it), the ReturnType-overridden
// factory, and property mutation through the re-resolving handle, together. ----
TEST_CASE("script.scene: OPTION 1 - Component.of(entity).field mutates the live component through a "
          "re-resolving handle")
{
    EnsureGadgetRegistered(); // before any VM context is built

    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();

    RefPtr<ScriptClass> setter = MakeClass(u8"Setter",
                                           u8"class Setter {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Setter(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() {\n"
                                           u8"        Gadget@ g = Gadget::of(self);\n"
                                           u8"        g.power = 5.0f;\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(setter, u8"e");
    gadgets->Add(e).power = 1.0f;

    bed.Start();
    bed.Frame(); // onStart: Gadget.of(_entity).power = 5.0
    REQUIRE(gadgets->Get(e) != nullptr);
    CHECK(gadgets->Get(e)->power == doctest::Approx(5.0f)); // the live component was mutated
}

// The same on the second backend: AngelScript is statically typed, so the ReturnType override is
// what makes `Gadget::of(self)` box as a Gadget handle (its declared return). Cross-backend parity
// for the entity.get shape.
TEST_CASE("script.scene: OPTION 1 - Component::of(entity).field mutates the live component "
          "(AngelScript) - cross-backend parity")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    EnsureGadgetRegistered();

    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();

    RefPtr<ScriptClass> setter =
        MakeClassLang(u8"angelscript", u8"Setter",
                      u8"class Setter {\n"
                      u8"    private Entity@ self;\n"
                      u8"    Setter(Entity@ entity) { @self = entity; }\n"
                      u8"    void onStart() {\n"
                      u8"        Gadget@ g = Gadget::of(self);\n"
                      u8"        g.power = 5.0f;\n"
                      u8"    }\n"
                      u8"}\n",
                      {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(setter, u8"e");
    gadgets->Add(e).power = 1.0f;

    bed.Start();
    bed.Frame();
    REQUIRE(gadgets->Get(e) != nullptr);
    CHECK(gadgets->Get(e)->power == doctest::Approx(5.0f)); // the live component was mutated
}

// Component.of(entity) is null for an entity without the component, so the `!is null` test scripts
// write reads as it says. A handle taken while the component was there, used after it is removed,
// is the script's error, not a write through null: the behaviour faults (disabled, as any script
// error leaves it) and the rest of the scene goes on.
TEST_CASE("script.scene: Component::of is null without the component, and a gone one is an error (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    EnsureGadgetRegistered();

    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();
    RefPtr<ScriptClass> probe =
        MakeClassLang(u8"angelscript", u8"Probe",
                      u8"class Probe {\n"
                      u8"    private Entity@ self;\n"
                      u8"    private Gadget@ g;\n"
                      u8"    Probe(Entity@ entity) { @self = entity; }\n"
                      u8"    void onStart() {\n"
                      u8"        @g = Gadget::of(self);\n"
                      u8"        self.setName(g is null ? \"none\" : \"has\");\n"
                      u8"    }\n"
                      u8"    void onUpdate(double dt) {\n"
                      u8"        if (g !is null) { g.power = g.power + 1.0f; }\n"
                      u8"    }\n"
                      u8"}\n",
                      {u8"onStart", u8"onUpdate"});
    const scene::EntityHandle bare = bed.AddScripted(probe, u8"bare");
    const scene::EntityHandle with = bed.AddScripted(probe, u8"with");
    gadgets->Add(with).power = 1.0f;

    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(bare) == StringView(u8"none"));
    CHECK(bed.scene.GetEntityName(with) == StringView(u8"has"));
    REQUIRE(gadgets->Get(with) != nullptr);
    CHECK(gadgets->Get(with)->power == doctest::Approx(2.0f));

    gadgets->Remove(with);
    bed.Frame(); // the handle's component is gone: a script error for that call, no crash
    CHECK(gadgets->Get(with) == nullptr);
    CHECK(bed.components->Get(with)->behaviors[0].faulted);
    CHECK_FALSE(bed.components->Get(bare)->behaviors[0].faulted); // the others run on
}

TEST_CASE("script.scene: Component.of is nil without the component (Luau)")
{
    EnsureGadgetRegistered();

    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();
    RefPtr<ScriptClass> probe =
        MakeClassLang(u8"luau", u8"Probe",
                      u8"Probe = {}\n"
                      u8"Probe.__index = Probe\n"
                      u8"function Probe.new(entity) return setmetatable({ entity = entity }, Probe) end\n"
                      u8"function Probe:onStart()\n"
                      u8"    local g = Gadget.of(self.entity)\n"
                      u8"    self.entity:setName(g == nil and \"none\" or \"has\")\n"
                      u8"    if g then g.power = 4.0 end\n"
                      u8"end\n",
                      {u8"onStart"});
    const scene::EntityHandle bare = bed.AddScripted(probe, u8"bare");
    const scene::EntityHandle with = bed.AddScripted(probe, u8"with");
    gadgets->Add(with).power = 1.0f;

    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(bare) == StringView(u8"none"));
    CHECK(bed.scene.GetEntityName(with) == StringView(u8"has"));
    CHECK(gadgets->Get(with)->power == doctest::Approx(4.0f));
}

// PathFollowComponent.of(entity): a script starts a stopped follower and sets its pace (Snowline's
// medal ghosts ride the course line this way), and the follower travels at that pace.
TEST_CASE("script.scene: PathFollowComponent.of(entity) sets a follower's speed and starts it")
{
    engine::spline::RegisterSplineScriptFacade(); // the follower as a script class + of() (idempotent)

    ScriptedScene bed;
    engine::spline::AddSplineSceneManagers(bed.scene);
    auto* splines = bed.scene.GetSystem<engine::spline::SplineComponentManager>();
    auto* follows = bed.scene.GetSystem<engine::spline::PathFollowComponentManager>();
    REQUIRE(splines != nullptr);
    REQUIRE(follows != nullptr);

    const scene::EntityHandle path = bed.scene.CreateEntity(u8"path");
    engine::spline::SplineComponent& spline = splines->Add(path);
    spline.curve.points.PushBack(foundation::spline::SplinePoint{Float3{0, 0, 0}});
    spline.curve.points.PushBack(foundation::spline::SplinePoint{Float3{100, 0, 0}});
    spline.curve.UpdateAutoHandles();
    spline.curve.RebuildArcLength();

    RefPtr<ScriptClass> pacer = MakeClass(u8"Pacer",
                                          u8"class Pacer {\n"
                                          u8"    private Entity@ self;\n"
                                          u8"    Pacer(Entity@ entity) { @self = entity; }\n"
                                          u8"    void onStart() {\n"
                                          u8"        PathFollowComponent@ f = PathFollowComponent::of(self);\n"
                                          u8"        f.speed = 4.0f;\n"
                                          u8"        f.playing = true;\n"
                                          u8"    }\n"
                                          u8"}\n",
                                          {u8"onStart"});
    const scene::EntityHandle ghost = bed.AddScripted(pacer, u8"ghost");
    engine::spline::PathFollowComponent& follow = follows->Add(ghost);
    follow.spline = bed.scene.GetEntityId(path);
    follow.loop = false;
    follow.playing = false; // stopped until the script starts it
    follow.speed = 0.0f;

    bed.Start();
    bed.Frame(); // onStart: the script sets the pace and starts it
    REQUIRE(follows->Get(ghost) != nullptr);
    CHECK(follows->Get(ghost)->speed == doctest::Approx(4.0f));
    CHECK(follows->Get(ghost)->playing);
    const f32 before = follows->Get(ghost)->distance;
    bed.Frame(); // 0.5 s at 4 m/s
    CHECK(follows->Get(ghost)->distance - before == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(bed.scene.GetWorldPosition(ghost).x == doctest::Approx(follows->Get(ghost)->distance).epsilon(0.02));
}

// The same on a real engine component: RigidBodyComponent.of(entity).friction, on a live physics
// scene. Same machinery as the Gadget case, wired to an actual engine component.
TEST_CASE("script.scene: OPTION 1 - RigidBodyComponent.of(entity).friction mutates the live "
          "physics component")
{
    engine::physics::RegisterPhysicsScriptFacade(); // register components as script classes + of() (idempotent)

    ContactWorld world;
    const scene::EntityHandle e = world.AddBody(u8"e", Float3{0, 2, 0}, physics::MotionKind::Dynamic,
                                                Float3{0.5f, 0.5f, 0.5f});
    auto* bodies = world.scene->GetSystem<engine::physics::RigidBodyComponentManager>();
    REQUIRE(bodies->Get(e) != nullptr);
    bodies->Get(e)->friction = 0.1f;

    RefPtr<ScriptClass> setter = MakeClass(u8"Setter",
                                           u8"class Setter {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Setter(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() {\n"
                                           u8"        RigidBodyComponent@ b = RigidBodyComponent::of(self);\n"
                                           u8"        b.friction = 0.5f;\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    world.Attach(e, setter);

    world.Play(2); // onStart: RigidBodyComponent.of(_entity).friction = 0.5
    REQUIRE(bodies->Get(e) != nullptr);
    CHECK(bodies->Get(e)->friction == doctest::Approx(0.5f)); // the live component was mutated
}

// scene.physics through the factory ScenePhysics.of(scene): reflected physics ops act on this
// scene's world. Also checks parity: gravityY reads the same world the static Physics facade reads
// (both delegate to PhysicsSceneSystem->World()->Gravity()), verified against ground truth.
TEST_CASE("script.scene: ScenePhysics.of(scene) reads + writes gravity on THIS scene's world")
{
    engine::physics::RegisterPhysicsScriptFacade();
    ContactWorld world; // the world is created on scene Start (inside Play), not before

    const scene::EntityHandle e = world.scene->CreateEntity(u8"driver");
    RefPtr<ScriptClass> driver =
        MakeClass(u8"Driver",
                  u8"class Driver {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Driver(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        ScenePhysics@ p = ScenePhysics::of(self.scene);\n"
                  u8"        p.setGravity(0.0f, -20.0f, 0.0f);\n"          // write this scene's world
                  u8"        self.setPosition(p.gravityY(), 0.0f, 0.0f);\n" // read it back: x = -20
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    world.Attach(e, driver);

    world.Play(2); // onStart: ScenePhysics.of(scene).setGravity, then gravityY
    auto* sys = world.scene->GetSystem<engine::physics::PhysicsSceneSystem>();
    REQUIRE(sys != nullptr);
    REQUIRE(sys->World() != nullptr);
    CHECK(world.scene->GetLocalTransform(e).position.x == doctest::Approx(-20.0f)); // gravityY read
    CHECK(sys->World()->Gravity().y == doctest::Approx(-20.0f)); // setGravity hit the real scene world
}

// DebugDraw.of(scene): the immediate-mode debug-draw facade is script-callable. No render service is
// installed in this bed, so each draw is a safe no-op (SceneDebug resolves null); the facade
// resolves and the calls run to completion without faulting (a rename after the draws lands).
TEST_CASE("script.scene: DebugDraw.of(scene) is script-callable + null-safe without a render service")
{
    engine::render::RegisterRenderScriptFacade(); // registers the DebugDraw facade type + prelude name
    ScriptedScene bed;
    RefPtr<ScriptClass> gizmo =
        MakeClass(u8"Gizmo",
                  u8"class Gizmo {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Gizmo(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        DebugDraw@ d = DebugDraw::of(self.scene);\n"
                  u8"        d.line(0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 1.0f, 0.0f, 0.0f);\n"
                  u8"        d.sphere(1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f, 0.0f);\n"
                  u8"        d.arrow(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f);\n"
                  u8"        self.setName(\"drew\");\n" // reached only if the draws did not fault
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(gizmo, u8"gizmo");
    bed.scene.Start();
    bed.scene.SetSimulationEnabled(true);
    bed.Frame(); // instantiate and onStart: DebugDraw::of, no-op draws, the rename
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"drew"));
}

// DebugDraw.of(scene) with the render service installed (as RenderSubsystem::ExposeToScript wires
// a real run): the draw lands in that scene's per-scene debug list on the subsystem, and in no
// other scene's. A headless RenderSubsystem over the NullDevice (DebugScene is pure accumulator
// state, no GPU) is the real service, the same object the render pass reads at frame end.
TEST_CASE("script.scene: DebugDraw.of(scene) appends to THIS scene's list on the real render service")
{
    engine::render::RegisterRenderScriptFacade();

    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(), DefaultAllocator());
    engine::render::RenderSubsystem sub{DefaultAllocator(), device, 2, dataFs};

    ScriptedScene bed;
    // The app-side wiring: ExposeToScript installs the per-context render service at context
    // creation through the host's configurator, as Input and Audio are in a real run.
    bed.host.SetContextConfigurator(Function<void(IScriptContext&)>{
        [&sub](IScriptContext& scriptContext) { sub.ExposeToScript(scriptContext); }});

    RefPtr<ScriptClass> gizmo =
        MakeClass(u8"Gizmo",
                  u8"class Gizmo {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Gizmo(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        DebugDraw@ d = DebugDraw::of(self.scene);\n"
                  u8"        d.line(0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 1.0f, 0.0f, 0.0f);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(gizmo, u8"gizmo");
    (void)e;
    bed.scene.Start();
    bed.scene.SetSimulationEnabled(true);
    bed.Frame(); // instantiate + onStart: the draw goes through the service into the subsystem

    // The scripted scene's per-scene list got exactly the one line (2 vertices), endpoints intact.
    auto& drawn = sub.DebugScene(bed.scene);
    CHECK(drawn.HasAnyDraws());
    REQUIRE(drawn.LineVertices().Size() == 2u);
    CHECK(drawn.LineVertices()[1].position.x == doctest::Approx(1.0f));
    CHECK(drawn.LineVertices()[1].position.y == doctest::Approx(2.0f));
    CHECK(drawn.LineVertices()[1].position.z == doctest::Approx(3.0f));

    // Another scene's list on the same subsystem stays empty: per-scene isolation.
    scene::Scene other{DefaultAllocator(), u8"other-scene"};
    CHECK_FALSE(sub.DebugScene(other).HasAnyDraws());
}

// scene.physics on the second backend: ScenePhysics::of returns the concrete handle type (its
// declared return), so AngelScript boxes it right. Cross-backend parity for the scene-system facade.
TEST_CASE("script.scene: ScenePhysics.of(scene) reads + writes gravity on THIS scene's world "
          "(AngelScript) - cross-backend parity")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::physics::RegisterPhysicsScriptFacade();
    ContactWorld world;

    const scene::EntityHandle e = world.scene->CreateEntity(u8"driver");
    RefPtr<ScriptClass> driver =
        MakeClassLang(u8"angelscript", u8"Driver",
                      u8"class Driver {\n"
                      u8"    private Entity@ self;\n"
                      u8"    Driver(Entity@ entity) { @self = entity; }\n"
                      u8"    void onStart() {\n"
                      u8"        ScenePhysics@ p = ScenePhysics::of(self.scene);\n"
                      u8"        p.setGravity(0.0f, -20.0f, 0.0f);\n"
                      u8"        self.setPosition(p.gravityY(), 0.0f, 0.0f);\n"
                      u8"    }\n"
                      u8"}\n",
                      {u8"onStart"});
    world.Attach(e, driver);

    world.Play(2);
    auto* sys = world.scene->GetSystem<engine::physics::PhysicsSceneSystem>();
    REQUIRE(sys != nullptr);
    REQUIRE(sys->World() != nullptr);
    CHECK(world.scene->GetLocalTransform(e).position.x == doctest::Approx(-20.0f)); // gravityY read
    CHECK(sys->World()->Gravity().y == doctest::Approx(-20.0f)); // setGravity hit the real world
}

// Per-entity character control through CharacterComponent.of(entity).move/jump. These are
// component-data ops (the tick reads moveVelocity/jumpSpeed), so they work through the re-resolving
// handle with no world access, and per entity, unlike the static facade, which reaches only the
// first character.
TEST_CASE("script.scene: CharacterComponent.of(entity).move/jump - per-entity control")
{
    engine::physics::RegisterPhysicsScriptFacade();
    ScriptedScene bed;
    auto* characters = bed.scene.AddSystem<engine::physics::CharacterComponentManager>();

    RefPtr<ScriptClass> driver = MakeClass(u8"Driver",
                                           u8"class Driver {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Driver(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() {\n"
                                           u8"        CharacterComponent@ c = CharacterComponent::of(self);\n"
                                           u8"        c.move(5.0f, 3.0f);\n"
                                           u8"        c.jump(6.0f);\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(driver, u8"e");
    characters->Add(e);

    bed.Start();
    bed.Frame();
    REQUIRE(characters->Get(e) != nullptr);
    CHECK(characters->Get(e)->moveVelocity.x == doctest::Approx(5.0f));
    CHECK(characters->Get(e)->moveVelocity.z == doctest::Approx(3.0f));
    CHECK(characters->Get(e)->jumpSpeed == doctest::Approx(6.0f));
}

TEST_CASE("script.scene: CharacterComponent.of(entity).move/jump - per-entity control (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::physics::RegisterPhysicsScriptFacade();
    ScriptedScene bed;
    auto* characters = bed.scene.AddSystem<engine::physics::CharacterComponentManager>();

    RefPtr<ScriptClass> driver =
        MakeClassLang(u8"angelscript", u8"Driver",
                      u8"class Driver {\n"
                      u8"    private Entity@ self;\n"
                      u8"    Driver(Entity@ entity) { @self = entity; }\n"
                      u8"    void onStart() {\n"
                      u8"        CharacterComponent@ c = CharacterComponent::of(self);\n"
                      u8"        c.move(5.0f, 3.0f);\n"
                      u8"        c.jump(6.0f);\n"
                      u8"        c.launch(4.0f);\n"
                      u8"    }\n"
                      u8"}\n",
                      {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(driver, u8"e");
    characters->Add(e);

    bed.Start();
    bed.Frame();
    REQUIRE(characters->Get(e) != nullptr);
    CHECK(characters->Get(e)->moveVelocity.x == doctest::Approx(5.0f));
    CHECK(characters->Get(e)->jumpSpeed == doctest::Approx(6.0f));
    // A launch, the air's jump, held for the physics step.
    CHECK(characters->Get(e)->launchPending);
    CHECK(characters->Get(e)->launchSpeed == doctest::Approx(4.0f));
}

// A board's surface: read the motion and the ground under the character (read-only properties),
// drive the whole velocity. The tick measures velocity and groundNormal; here they are seeded.
TEST_CASE("script.scene: CharacterComponent velocity, groundNormal and drive (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::physics::RegisterPhysicsScriptFacade();
    ScriptedScene bed;
    auto* characters = bed.scene.AddSystem<engine::physics::CharacterComponentManager>();

    RefPtr<ScriptClass> board =
        MakeClassLang(u8"angelscript", u8"Board",
                      u8"class Board {\n"
                      u8"    private Entity@ self;\n"
                      u8"    Board(Entity@ entity) { @self = entity; }\n"
                      u8"    void onStart() {\n"
                      u8"        CharacterComponent@ c = CharacterComponent::of(self);\n"
                      u8"        Float3 v = c.velocity;\n"
                      u8"        Float3 n = c.groundNormal;\n"
                      u8"        c.drive(v.x + n.x, v.y + n.y, v.z + n.z);\n"
                      u8"    }\n"
                      u8"}\n",
                      {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(board, u8"e");
    engine::physics::CharacterComponent& character = characters->Add(e);
    character.velocity = Float3{1.0f, 2.0f, 3.0f};
    character.groundNormal = Float3{0.0f, 0.0f, 1.0f};

    bed.Start();
    bed.Frame();
    REQUIRE(characters->Get(e) != nullptr);
    CHECK(characters->Get(e)->driving);
    CHECK(characters->Get(e)->driveVelocity.x == doctest::Approx(1.0f));
    CHECK(characters->Get(e)->driveVelocity.y == doctest::Approx(2.0f));
    CHECK(characters->Get(e)->driveVelocity.z == doctest::Approx(4.0f));
}

// A scriptable impulse on a dynamic body: ScenePhysics.of(scene).applyImpulse(entity, x, y, z), a
// world op keyed by entity. The upward impulse gives the dynamic body positive Y velocity.
TEST_CASE("script.scene: ScenePhysics.of(scene).applyImpulse(entity, ...) - scriptable impulse")
{
    engine::physics::RegisterPhysicsScriptFacade();
    ContactWorld world;
    const scene::EntityHandle e = world.AddBody(u8"ball", Float3{0, 5, 0},
                                                physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    RefPtr<ScriptClass> pusher = MakeClass(u8"Pusher",
                                           u8"class Pusher {\n"
                                           u8"    private Entity@ self;\n"
                                           u8"    Pusher(Entity@ entity) { @self = entity; }\n"
                                           u8"    void onStart() {\n"
                                           u8"        ScenePhysics::of(self.scene).applyImpulse(self, 0.0f, 5000.0f, 0.0f);\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    world.Attach(e, pusher);

    world.Play(2); // onStart applies the impulse; the next step integrates it
    auto* sys = world.scene->GetSystem<engine::physics::PhysicsSceneSystem>();
    auto* bodies = world.scene->GetSystem<engine::physics::RigidBodyComponentManager>();
    REQUIRE(sys->World() != nullptr);
    REQUIRE(bodies->Get(e) != nullptr);
    CHECK(sys->World()->LinearVelocity(bodies->Get(e)->body).y > 0.0f); // impulse dominated gravity
}

// The full overlap set reaches script as a native array. ScenePhysics.overlapSphere returns
// Array<Entity>, which crosses as a real AngelScript `array<Entity@>`; a behaviour iterates it with
// .length()/[] and acts on every hit, covering container-return marshalling end to end. The
// scanner impulses each overlapped body upward; both dynamic targets end with positive Y velocity.
TEST_CASE("script.scene: ScenePhysics.overlapSphere returns a native array - behavior acts on every hit")
{
    engine::physics::RegisterPhysicsScriptFacade();
    ContactWorld world;
    const scene::EntityHandle a = world.AddBody(u8"left", Float3{-1.5f, 0, 0},
                                                physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    const scene::EntityHandle b = world.AddBody(u8"right", Float3{1.5f, 0, 0},
                                                physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    const scene::EntityHandle scanner = world.scene->CreateEntity(u8"scanner");
    RefPtr<ScriptClass> sweep = MakeClass(
        u8"Scanner",
        u8"class Scanner {\n"
        u8"    private Entity@ self;\n"
        u8"    Scanner(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        ScenePhysics@ p = ScenePhysics::of(self.scene);\n"
        u8"        array<Entity@>@ hits = p.overlapSphere(0.0f, 0.0f, 0.0f, 3.0f, -1);\n"
        u8"        for (uint i = 0; i < hits.length(); i++) {\n"
        u8"            p.applyImpulse(hits[i], 0.0f, 5000.0f, 0.0f);\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});
    world.Attach(scanner, sweep);

    world.Play(2); // onStart sweeps + impulses every hit; the next step integrates
    auto* sys = world.scene->GetSystem<engine::physics::PhysicsSceneSystem>();
    auto* bodies = world.scene->GetSystem<engine::physics::RigidBodyComponentManager>();
    REQUIRE(sys->World() != nullptr);
    REQUIRE(bodies->Get(a) != nullptr);
    REQUIRE(bodies->Get(b) != nullptr);
    // Both bodies overlapped the r=3 sphere at the origin and both received the upward impulse.
    CHECK(sys->World()->LinearVelocity(bodies->Get(a)->body).y > 0.0f);
    CHECK(sys->World()->LinearVelocity(bodies->Get(b)->body).y > 0.0f);
}

// The Luau twin: overlapSphere's Array<Entity> crosses as a 1-indexed Lua table walked with #/[].
// This is the case the u64 precision rule protects (a Lua table element is an f64, so the facade
// returns resolved Entity elements, never packed handles). A Luau behaviour impulses every hit.
TEST_CASE("script.scene: LUAU ScenePhysics.overlapSphere returns a Lua table - acts on every hit")
{
    engine::physics::RegisterPhysicsScriptFacade();
    ContactWorld world;
    const scene::EntityHandle a = world.AddBody(u8"left", Float3{-1.5f, 0, 0},
                                                physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    const scene::EntityHandle b = world.AddBody(u8"right", Float3{1.5f, 0, 0},
                                                physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    const scene::EntityHandle scanner = world.scene->CreateEntity(u8"scanner");
    RefPtr<ScriptClass> sweep = MakeClassLang(
        u8"luau", u8"Scanner",
        u8"Scanner = {}\n"
        u8"Scanner.__index = Scanner\n"
        u8"function Scanner.new(entity) return setmetatable({ entity = entity }, Scanner) end\n"
        u8"function Scanner:onStart()\n"
        u8"    local p = ScenePhysics.of(self.entity.scene)\n"
        u8"    local hits = p:overlapSphere(0.0, 0.0, 0.0, 3.0, -1)\n"
        u8"    for i = 1, #hits do\n"
        u8"        p:applyImpulse(hits[i], 0.0, 5000.0, 0.0)\n"
        u8"    end\n"
        u8"end\n",
        {u8"onStart"});
    world.Attach(scanner, sweep);

    world.Play(2);
    auto* sys = world.scene->GetSystem<engine::physics::PhysicsSceneSystem>();
    auto* bodies = world.scene->GetSystem<engine::physics::RigidBodyComponentManager>();
    REQUIRE(sys->World() != nullptr);
    REQUIRE(bodies->Get(a) != nullptr);
    REQUIRE(bodies->Get(b) != nullptr);
    CHECK(sys->World()->LinearVelocity(bodies->Get(a)->body).y > 0.0f);
    CHECK(sys->World()->LinearVelocity(bodies->Get(b)->body).y > 0.0f);
}

// ---- The scene event bus reaches script. A behaviour publishes scene.events.emit(name, x); every
//      behaviour and the Level that declare on<Name>(x) receive it (same bus, both tiers), the
//      payload surviving as a Variant. A subscriber torn down is unsubscribed. The bus is native
//      and backend-neutral, so each backend is checked on its own bus. ----

TEST_CASE("script.scene: scene.events.emit reaches a sibling behavior AND the Level's "
          "on<Event>(payload) - both tiers, one bus")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    RefPtr<ScriptClass> emitter =
        MakeClass(u8"Emitter",
                  u8"class Emitter {\n"
                  u8"    private Entity@ self;\n"
                  u8"    private bool done;\n"
                  u8"    Emitter(Entity@ entity) {\n"
                  u8"        @self = entity;\n"
                  u8"        done = false;\n"
                  u8"    }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        if (!done) {\n"
                  u8"            self.scene.events.emit(\"OrbCollected\", 5);\n"
                  u8"            done = true;\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    RefPtr<ScriptClass> receiver =
        MakeClass(u8"Receiver",
                  u8"class Receiver {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Receiver(Entity@ entity) { @self = entity; }\n"
                  u8"    void onOrbCollected(int n) { self.setName(\"got:\" + n); }\n"
                  u8"}\n",
                  {u8"onOrbCollected"});
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    private Scene@ scene;\n"
                  u8"    Level(Scene@ s) { @scene = s; }\n"
                  u8"    void onOrbCollected(int n) { scene.find(\"levelmark\").setName(\"level:\" + n); }\n"
                  u8"}\n",
                  {u8"onOrbCollected"});
    level->Settings().script = levelClass;

    const scene::EntityHandle listener = bed.AddScripted(receiver, u8"listener");
    (void)bed.AddScripted(emitter, u8"emitter");
    const scene::EntityHandle levelmark = bed.scene.CreateEntity(u8"levelmark");

    bed.Start();
    bed.Frame(); // instantiate all; emitter.onUpdate emits; the bus drains, both receive
    bed.Frame(); // slack for any instantiation or subscription ordering

    // The sibling behavior received on<Event>(payload): payload 5 survived as a Variant.
    CHECK(bed.scene.GetEntityName(listener) == StringView(u8"got:5"));
    // The Level received the same event on the same bus, with no GameManager entity.
    CHECK(bed.scene.GetEntityName(levelmark) == StringView(u8"level:5"));
}

TEST_CASE("script.scene: scene.events.emit reaches a sibling behavior AND the Level's "
          "on<Event>(payload) - the 2nd backend (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    RefPtr<ScriptClass> emitter = MakeClassLang(
        u8"angelscript", u8"Emitter",
        u8"class Emitter {\n"
        u8"    private Entity@ self;\n"
        u8"    private bool done;\n"
        u8"    Emitter(Entity@ entity) { @self = entity; done = false; }\n"
        u8"    void onUpdate(float dt) {\n"
        u8"        if (!done) { self.scene.events.emit(\"OrbCollected\", 5.0); done = true; }\n"
        u8"    }\n"
        u8"}\n",
        {u8"onUpdate"});
    RefPtr<ScriptClass> receiver = MakeClassLang(
        u8"angelscript", u8"Receiver",
        u8"class Receiver {\n"
        u8"    private Entity@ self;\n"
        u8"    Receiver(Entity@ entity) { @self = entity; }\n"
        u8"    void onOrbCollected(float n) { if (n > 4.5f) self.setName(\"got5\"); }\n"
        u8"}\n",
        {u8"onOrbCollected"});
    RefPtr<ScriptClass> levelClass = MakeClassLang(
        u8"angelscript", u8"Level",
        u8"class Level {\n"
        u8"    private Scene@ scene;\n"
        u8"    Level(Scene@ s) { @scene = s; }\n"
        u8"    void onOrbCollected(float n) { if (n > 4.5f) scene.find(\"levelmark\").setName(\"level5\"); }\n"
        u8"}\n",
        {u8"onOrbCollected"});
    level->Settings().script = levelClass;

    const scene::EntityHandle listener = bed.AddScripted(receiver, u8"listener");
    (void)bed.AddScripted(emitter, u8"emitter");
    const scene::EntityHandle levelmark = bed.scene.CreateEntity(u8"levelmark");

    bed.Start();
    bed.Frame();
    bed.Frame();

    CHECK(bed.scene.GetEntityName(listener) == StringView(u8"got5"));   // behavior received payload
    CHECK(bed.scene.GetEntityName(levelmark) == StringView(u8"level5")); // Level received it too
}

TEST_CASE("script.scene: LUAU scene.events.emit reaches a sibling behavior AND the Level's "
          "on<Event>(payload) - both tiers, one bus")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    // The emitter reaches the bus through the facade chain: self.entity.scene (Entity computed
    // property) .events (Scene computed property) :emit(name, payload) (method, colon call).
    RefPtr<ScriptClass> emitter = MakeClassLang(
        u8"luau", u8"Emitter",
        u8"Emitter = {}\n"
        u8"Emitter.__index = Emitter\n"
        u8"function Emitter.new(entity)\n"
        u8"    return setmetatable({ entity = entity, done = false }, Emitter)\n"
        u8"end\n"
        u8"function Emitter:onUpdate(dt)\n"
        u8"    if not self.done then\n"
        u8"        self.entity.scene.events:emit(\"OrbCollected\", 5)\n"
        u8"        self.done = true\n"
        u8"    end\n"
        u8"end\n",
        {u8"onUpdate"});
    RefPtr<ScriptClass> receiver = MakeClassLang(
        u8"luau", u8"Receiver",
        u8"Receiver = {}\n"
        u8"Receiver.__index = Receiver\n"
        u8"function Receiver.new(entity) return setmetatable({ entity = entity }, Receiver) end\n"
        u8"function Receiver:onOrbCollected(n)\n"
        u8"    if n > 4.5 then self.entity:setName(\"got5\") end\n"
        u8"end\n",
        {u8"onOrbCollected"});
    RefPtr<ScriptClass> levelClass = MakeClassLang(
        u8"luau", u8"Level",
        u8"Level = {}\n"
        u8"Level.__index = Level\n"
        u8"function Level.new(scene) return setmetatable({ scene = scene }, Level) end\n"
        u8"function Level:onOrbCollected(n)\n"
        u8"    if n > 4.5 then self.scene:find(\"levelmark\"):setName(\"level5\") end\n"
        u8"end\n",
        {u8"onOrbCollected"});
    level->Settings().script = levelClass;

    const scene::EntityHandle listener = bed.AddScripted(receiver, u8"listener");
    (void)bed.AddScripted(emitter, u8"emitter");
    const scene::EntityHandle levelmark = bed.scene.CreateEntity(u8"levelmark");

    bed.Start();
    bed.Frame(); // instantiate all; the emitter emits; the bus drains, both receive
    bed.Frame(); // slack for subscription ordering

    CHECK(bed.scene.GetEntityName(listener) == StringView(u8"got5"));   // sibling behavior
    CHECK(bed.scene.GetEntityName(levelmark) == StringView(u8"level5")); // the Level, same bus
}

TEST_CASE("script.scene: a destroyed subscriber stops receiving bus events; the emitter "
          "keeps running fault-free (teardown)")
{
    ScriptedScene bed;

    RefPtr<ScriptClass> emitter =
        MakeClass(u8"Bumper",
                  u8"class Bumper {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Bumper(Entity@ entity) { @self = entity; }\n"
                  u8"    void onUpdate(double dt) { self.scene.events.emit(\"Bump\", 1); }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    RefPtr<ScriptClass> receiver =
        MakeClass(u8"Counter",
                  u8"class Counter {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Counter(Entity@ entity) { @self = entity; }\n"
                  u8"    void onBump(int n) {\n"
                  u8"        Entity@ c = self.scene.find(\"counter\");\n"
                  u8"        Float3 p = c.position();\n"
                  u8"        c.setPosition(p.x + float(n), 0.0f, 0.0f);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onBump"});

    const scene::EntityHandle counter = bed.scene.CreateEntity(u8"counter");
    const scene::EntityHandle receiverEntity = bed.AddScripted(receiver, u8"receiver");
    const scene::EntityHandle emitterEntity = bed.AddScripted(emitter, u8"emitter");

    bed.Start();
    bed.Frame();
    bed.Frame();
    const f32 whileAlive = bed.scene.GetLocalTransform(counter).position.x;
    CHECK(whileAlive > 0.0f); // the receiver was advancing the counter on each Bump

    bed.scene.DestroyEntity(receiverEntity); // the only onBump subscriber is gone
    bed.Frame();
    bed.Frame();
    // The counter is frozen: the destroyed subscriber receives nothing, and BroadcastEvent's
    // per-behaviour re-resolution finds nothing to deliver to (the system subscription stays,
    // harmless). The emitter kept publishing without faulting.
    CHECK(Near(bed.scene.GetLocalTransform(counter).position.x, whileAlive));
    ScriptComponent* emitterComp = bed.components->Get(emitterEntity);
    REQUIRE(emitterComp != nullptr);
    CHECK_FALSE(emitterComp->behaviors[0].faulted);
}

// The generic payload: a reflected value (here a live component handle from Gadget.of) rides the
// bus as a Variant and the receiver reads its live field. Covers the emit(name, Variant) sink and
// the ?&in overload that accepts any Variant.
TEST_CASE("script.scene: scene.events.emit carries a reflected component handle as the payload; "
          "the receiver reads its live field")
{
    EnsureGadgetRegistered();
    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();

    RefPtr<ScriptClass> producer =
        MakeClass(u8"Producer",
                  u8"class Producer {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Producer(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() { self.scene.events.emit(\"Configured\", Gadget::of(self)); }\n"
                  u8"}\n",
                  {u8"onStart"});
    RefPtr<ScriptClass> consumer =
        MakeClass(u8"Consumer",
                  u8"class Consumer {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Consumer(Entity@ entity) { @self = entity; }\n"
                  u8"    void onConfigured(Gadget@ g) { Gadget::of(self).power = g.power; }\n"
                  u8"}\n",
                  {u8"onConfigured"});

    const scene::EntityHandle producerEntity = bed.AddScripted(producer, u8"producer");
    const scene::EntityHandle consumerEntity = bed.AddScripted(consumer, u8"consumer");
    gadgets->Add(producerEntity).power = 7.0f; // the value the payload carries
    gadgets->Add(consumerEntity).power = 1.0f; // overwritten from the payload's live field

    bed.Start();
    bed.Frame(); // producer.onStart emits Gadget.of(producer); the bus drains to consumer.onConfigured
    bed.Frame();

    REQUIRE(gadgets->Get(consumerEntity) != nullptr);
    // The payload handle re-resolved to the producer's live Gadget (power 7) across the bus: a
    // reflected value survived as the event payload, and the receiver read its live field.
    CHECK(gadgets->Get(consumerEntity)->power == doctest::Approx(7.0f));
}

TEST_CASE("script.scene: an event's reflected value reaches a handler that takes it by value as well "
          "as one that takes a handle")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> producer =
        MakeClass(u8"Producer",
                  u8"class Producer {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Producer(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() { self.scene.events.emit(\"Spotted\", Float3(1.0f, 2.0f, 3.0f)); }\n"
                  u8"}\n",
                  {u8"onStart"});
    // The same Float3 payload reaches a handler taking it by value (`Float3 at`) and one taking a handle.
    RefPtr<ScriptClass> byValue =
        MakeClass(u8"ByValue",
                  u8"class ByValue {\n"
                  u8"    private Entity@ self;\n"
                  u8"    ByValue(Entity@ entity) { @self = entity; }\n"
                  u8"    void onSpotted(Float3 at) { if (at.y == 2.0f && at.z == 3.0f) self.setName(\"seen\"); }\n"
                  u8"}\n",
                  {u8"onSpotted"});
    RefPtr<ScriptClass> byHandle =
        MakeClass(u8"ByHandle",
                  u8"class ByHandle {\n"
                  u8"    private Entity@ self;\n"
                  u8"    ByHandle(Entity@ entity) { @self = entity; }\n"
                  u8"    void onSpotted(Float3@ at) { if (at !is null && at.x == 1.0f) self.setName(\"seen\"); }\n"
                  u8"}\n",
                  {u8"onSpotted"});

    (void)bed.AddScripted(producer, u8"producer");
    const scene::EntityHandle a = bed.AddScripted(byValue, u8"a");
    const scene::EntityHandle b = bed.AddScripted(byHandle, u8"b");
    bed.Start();
    bed.Frame(); // the producer emits; the bus drains to both handlers
    bed.Frame();

    CHECK(bed.scene.GetEntityName(a) == StringView(u8"seen"));
    CHECK(bed.scene.GetEntityName(b) == StringView(u8"seen"));
}

// ---- Render surface: MeshComponent.of / LightComponent.of. A behaviour reaches the real render
//      components by type and mutates them live, as with the physics components. Pure-data props
//      (visible/intensity/range/enabled); resource swaps (mesh/material) go through SceneRender. ----

TEST_CASE("script.scene: MeshComponent.of / LightComponent.of set live render props")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* meshes = bed.scene.AddSystem<engine::render::MeshComponentManager>();
    auto* lights = bed.scene.AddSystem<engine::render::LightComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"Tweaker",
                  u8"class Tweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Tweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        MeshComponent::of(self).visible = false;\n"
                  u8"        LightComponent@ light = LightComponent::of(self);\n"
                  u8"        light.intensity = 4.0f;\n"
                  u8"        light.range = 25.0f;\n"
                  u8"        light.enabled = false;\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    meshes->Add(e).visible = true;
    engine::render::LightComponent& light = lights->Add(e);
    light.intensity = 1.0f;
    light.enabled = true;

    bed.Start();
    bed.Frame(); // onStart mutates the live components through the re-resolving handles

    REQUIRE(meshes->Get(e) != nullptr);
    CHECK_FALSE(meshes->Get(e)->visible);
    REQUIRE(lights->Get(e) != nullptr);
    CHECK(lights->Get(e)->intensity == doctest::Approx(4.0f));
    CHECK(lights->Get(e)->range == doctest::Approx(25.0f));
    CHECK_FALSE(lights->Get(e)->enabled);
}

TEST_CASE("script.scene: a behaviour sets its member's clip and its crowd's pose policy")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* sets = bed.scene.AddSystem<engine::render::InstancedMeshComponentManager>();
    auto* members = bed.scene.AddSystem<engine::render::InstanceMemberComponentManager>();
    RefPtr<ScriptClass> dancer =
        MakeClass(u8"Dancer",
                  u8"class Dancer {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Dancer(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        InstanceMemberComponent::of(self).clip = 3;\n"
                  u8"        InstancedMeshComponent::of(self).poseAssignment = PoseAssignment::Unison;\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(dancer, u8"e");
    sets->Add(e);
    members->Add(e).clip = 0;
    bed.Start();
    bed.Frame();
    CHECK(members->Get(e)->clip == 3u);
    CHECK(sets->Get(e)->poseAssignment == foundation::render::PoseAssignment::Unison);
}

TEST_CASE("script.scene: MeshComponent::of / LightComponent::of set live render props (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* meshes = bed.scene.AddSystem<engine::render::MeshComponentManager>();
    auto* lights = bed.scene.AddSystem<engine::render::LightComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"Tweaker",
        u8"class Tweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    Tweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        MeshComponent@ m = MeshComponent::of(self);\n"
        u8"        m.visible = false;\n"
        u8"        LightComponent@ l = LightComponent::of(self);\n"
        u8"        l.intensity = 4.0f;\n"
        u8"        l.range = 25.0f;\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    meshes->Add(e).visible = true;
    lights->Add(e).intensity = 1.0f;

    bed.Start();
    bed.Frame();

    REQUIRE(meshes->Get(e) != nullptr);
    CHECK_FALSE(meshes->Get(e)->visible);
    REQUIRE(lights->Get(e) != nullptr);
    CHECK(lights->Get(e)->intensity == doctest::Approx(4.0f));
    CHECK(lights->Get(e)->range == doctest::Approx(25.0f));
}

// ---- The resource-swap primitive: SceneRender.of(scene).setMesh(entity, id) /
//      setMaterial(entity, id). A world op keyed by entity (it needs the run's resource manager,
//      which the component data cannot reach), so it lives on the scene handle like ScenePhysics.
//      The resource id (a Guid) reaches the behaviour as an asset property ("swap to the mesh I
//      picked"). No ResourceManager is wired here, so the swap sets the id (the script to component
//      path); Ref::Bind resolution is covered natively in the render resource-ref tests. ----

TEST_CASE("script.scene: SceneRender.setMesh / setMaterial swap a component's resource id from "
          "script")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* meshes = bed.scene.AddSystem<engine::render::MeshComponentManager>();

    RefPtr<ScriptClass> swapper =
        MakeClass(u8"Swapper",
                  u8"class Swapper {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ meshRes;\n"
                  u8"    Guid@ matRes;\n"
                  u8"    Swapper(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        SceneRender@ r = SceneRender::of(self.scene);\n"
                  u8"        bool swapped = r.setMesh(self, meshRes);\n"
                  u8"        r.setMaterial(self, matRes);\n"
                  u8"        bool slotted = r.setMaterial(self, matRes, 2);\n"
                  u8"        if (swapped && slotted && !r.setMaterial(self, matRes, -1)) {\n"
                  u8"            self.setName(\"swapped\");\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const Guid meshId{0x1111, 0x2222};
    const Guid matId{0x3333, 0x4444};
    ScriptPropertyDesc meshProp;
    meshProp.name = String(u8"meshRes");
    meshProp.hash = ScriptPropertyNameHash(u8"meshRes");
    meshProp.type = ScriptPropertyType::Asset;
    meshProp.assetType = String(u8"Mesh");
    meshProp.defaultValue.kind = ScriptPropertyType::Asset;
    meshProp.defaultValue.guid = meshId;
    swapper->properties.PushBack(meshProp);
    ScriptPropertyDesc matProp;
    matProp.name = String(u8"matRes");
    matProp.hash = ScriptPropertyNameHash(u8"matRes");
    matProp.type = ScriptPropertyType::Asset;
    matProp.assetType = String(u8"Material");
    matProp.defaultValue.kind = ScriptPropertyType::Asset;
    matProp.defaultValue.guid = matId;
    swapper->properties.PushBack(matProp);

    const scene::EntityHandle e = bed.AddScripted(swapper, u8"e");
    meshes->Add(e); // empty mesh/materials

    bed.Start();
    bed.Frame(); // onStart: SceneRender.setMesh/setMaterial set the component's resource ids

    REQUIRE(meshes->Get(e) != nullptr);
    CHECK(meshes->Get(e)->mesh.id == meshId);          // mesh Ref id swapped from script
    REQUIRE_FALSE(meshes->Get(e)->materials.IsEmpty()); // slot 0 created
    CHECK(meshes->Get(e)->materials[0].id == matId);    // material slot-0 Ref id swapped
    // Slot 2 by number: the slots grew to reach it; -1 refused.
    REQUIRE(meshes->Get(e)->materials.Size() == 3u);
    CHECK(meshes->Get(e)->materials[2].id == matId);
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"swapped"));
}

// ---- per-mesh material properties: SceneRender.setMaterialFloat / setMaterialFloat4 set a property
//      of one slot for this mesh alone (the renderer draws it with an instance of its own), and
//      clearMaterialProperty puts it back; the component keeps them, each change bumping its
//      version so the renderer re-applies. ----

namespace
{
    void CheckMaterialPropertyScript(StringView language, StringView className, StringView source)
    {
        engine::render::RegisterRenderScriptFacade();
        ScriptedScene bed;
        auto* meshes = bed.scene.AddSystem<engine::render::MeshComponentManager>();
        RefPtr<ScriptClass> tinter = MakeClassLang(language, className, source, {u8"onStart"});
        const scene::EntityHandle e = bed.AddScripted(tinter, u8"e");
        meshes->Add(e);
        bed.Start();
        bed.Frame();

        const engine::render::MeshComponent* mesh = meshes->Get(e);
        REQUIRE(mesh != nullptr);
        CHECK(bed.scene.GetEntityName(e) == StringView(u8"set"));
        // Roughness set then set again (one entry, the later value), the glow set, the tint set and
        // cleared: two left, the version bumped by each of the five changes (a second clear of the
        // tint and a negative slot change nothing).
        REQUIRE(mesh->materialOverrides.Size() == 2u);
        CHECK(mesh->materialOverrides[0].name == StringView(u8"Roughness"));
        CHECK(mesh->materialOverrides[0].slot == 0u);
        CHECK(mesh->materialOverrides[0].size == sizeof(f32));
        CHECK(mesh->materialOverrides[0].value.x == doctest::Approx(0.25f));
        CHECK(mesh->materialOverrides[1].name == StringView(u8"EmissiveColor"));
        CHECK(mesh->materialOverrides[1].slot == 1u);
        CHECK(mesh->materialOverrides[1].size == sizeof(Float4));
        CHECK(mesh->materialOverrides[1].value.w == doctest::Approx(2.0f));
        CHECK(mesh->materialOverrideVersion == 5u);
    }
}

TEST_CASE("script.scene: SceneRender sets and clears a mesh's own material properties")
{
    CheckMaterialPropertyScript(
        u8"angelscript", u8"Tinter",
        u8"class Tinter {\n"
        u8"    private Entity@ self;\n"
        u8"    Tinter(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        SceneRender@ r = SceneRender::of(self.scene);\n"
        u8"        bool a = r.setMaterialFloat(self, 0, \"Roughness\", 0.5f);\n"
        u8"        bool b = r.setMaterialFloat(self, 0, \"Roughness\", 0.25f);\n"
        u8"        bool c = r.setMaterialFloat4(self, 1, \"EmissiveColor\", Float4(0.3f, 0.4f, 0.5f, 2.0f));\n"
        u8"        r.setMaterialFloat4(self, 0, \"BaseColor\", Float4(1.0f, 0.0f, 0.0f, 1.0f));\n"
        u8"        bool d = r.clearMaterialProperty(self, 0, \"BaseColor\");\n"
        u8"        bool e = r.clearMaterialProperty(self, 0, \"BaseColor\");\n"
        u8"        bool f = r.setMaterialFloat(self, -1, \"Roughness\", 1.0f);\n"
        u8"        if (a && b && c && d && !e && !f) { self.setName(\"set\"); }\n"
        u8"    }\n"
        u8"}\n");
}

TEST_CASE("script.scene: SceneRender sets and clears a mesh's own material properties (Luau)")
{
    CheckMaterialPropertyScript(
        u8"luau", u8"Tinter",
        u8"Tinter = {}\n"
        u8"Tinter.__index = Tinter\n"
        u8"function Tinter.new(entity) return setmetatable({ entity = entity }, Tinter) end\n"
        u8"function Tinter:onStart()\n"
        u8"    local me = self.entity\n"
        u8"    local r = SceneRender.of(me.scene)\n"
        u8"    local a = r:setMaterialFloat(me, 0, \"Roughness\", 0.5)\n"
        u8"    local b = r:setMaterialFloat(me, 0, \"Roughness\", 0.25)\n"
        u8"    local c = r:setMaterialFloat4(me, 1, \"EmissiveColor\", Float4.new(0.3, 0.4, 0.5, 2.0))\n"
        u8"    r:setMaterialFloat4(me, 0, \"BaseColor\", Float4.new(1, 0, 0, 1))\n"
        u8"    local d = r:clearMaterialProperty(me, 0, \"BaseColor\")\n"
        u8"    local e = r:clearMaterialProperty(me, 0, \"BaseColor\")\n"
        u8"    local f = r:setMaterialFloat(me, -1, \"Roughness\", 1.0)\n"
        u8"    if a and b and c and d and not e and not f then me:setName(\"set\") end\n"
        u8"end\n");
}

TEST_CASE("script.scene: SceneRender.lightAt reads how lit a place is, with and without a group mask")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* lights = bed.scene.AddSystem<engine::render::LightComponentManager>();
    const scene::EntityHandle lampEntity = bed.scene.CreateEntity(u8"lamp");
    bed.scene.SetLocalPosition(lampEntity, Float3{0.0f, 2.0f, 0.0f});
    engine::render::LightComponent& lamp = lights->Add(lampEntity);
    lamp.type = engine::render::LightType::Point;
    lamp.intensity = 4.0f;
    lamp.range = 0.0f; // no falloff: the full 4 arrives anywhere

    RefPtr<ScriptClass> meter =
        MakeClass(u8"Meter",
                  u8"class Meter {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Meter(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        SceneRender@ r = SceneRender::of(self.scene);\n"
                  u8"        Float3 lit = r.lightAt(Float3(0.0f, 0.0f, 0.0f));\n"
                  u8"        Float3 masked = r.lightAt(Float3(0.0f, 0.0f, 0.0f), 1);\n"
                  u8"        if (lit.x > 3.99f && lit.x < 4.01f && masked.y > 3.99f) {\n"
                  u8"            self.setName(\"lit\");\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(meter, u8"e");
    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"lit"));
}

TEST_CASE("script.scene: SceneRender.setCameraTarget points a camera at a render texture; nil "
          "clears it")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* cameras = bed.scene.AddSystem<engine::render::CameraComponentManager>();

    RefPtr<ScriptClass> minimap =
        MakeClass(u8"Minimap",
                  u8"class Minimap {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ texture;\n"
                  u8"    int frames = 0;\n"
                  u8"    Minimap(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        if (SceneRender::of(self.scene).setCameraTarget(self, texture)) {\n"
                  u8"            self.setName(\"targeted\");\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"    void onUpdate(double dt) {\n"
                  u8"        frames += 1;\n"
                  u8"        if (frames == 2) {\n"
                  u8"            SceneRender::of(self.scene).setCameraTarget(self, Guid(0, 0));\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate"});
    const Guid textureId{0x5555, 0x6666};
    ScriptPropertyDesc textureProp;
    textureProp.name = String(u8"texture");
    textureProp.hash = ScriptPropertyNameHash(u8"texture");
    textureProp.type = ScriptPropertyType::Asset;
    textureProp.assetType = String(u8"RenderTexture");
    textureProp.defaultValue.kind = ScriptPropertyType::Asset;
    textureProp.defaultValue.guid = textureId;
    minimap->properties.PushBack(textureProp);

    const scene::EntityHandle e = bed.AddScripted(minimap, u8"e");
    cameras->Add(e);

    bed.Start();
    bed.Frame(); // onStart: the camera now draws into the texture
    REQUIRE(cameras->Get(e) != nullptr);
    CHECK(cameras->Get(e)->target.id == textureId);
    CHECK(cameras->Get(e)->HasTarget()); // a target, so not the screen's camera
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"targeted"));

    bed.Frame();
    bed.Frame(); // the second update clears it: the screen's camera again
    CHECK_FALSE(cameras->Get(e)->HasTarget());
}

// ---- Audio surface: AudioSourceComponent.of (live volume/pitch/loop, data) and
//      SceneAudio.of(scene) (play/stop/pause/isPlaying/setClip, world ops keyed by entity that
//      reach the scene's AudioEngine). No engine is wired here (the ops are null-safe), so this
//      covers the script surface, forwarding and the data and resource paths on both backends;
//      voice playback is covered natively in AudioSceneTests. ----

TEST_CASE("script.scene: AudioSourceComponent.of props + SceneAudio.of play/stop/pause/setClip "
          "are bound and control the component")
{
    engine::audio::RegisterAudioScriptFacade();
    ScriptedScene bed;
    auto* sources = bed.scene.AddSystem<engine::audio::AudioSourceComponentManager>();
    (void)bed.scene.AddSystem<engine::audio::AudioSceneSystem>(); // no engine set: the ops are safe no-ops

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"AudioTweaker",
                  u8"class AudioTweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ clipRes;\n"
                  u8"    AudioTweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        AudioSourceComponent@ s = AudioSourceComponent::of(self);\n"
                  u8"        s.volume = 0.25f;\n"
                  u8"        s.pitch = 2.0f;\n"
                  u8"        s.loop = true;\n"
                  u8"        SceneAudio@ a = SceneAudio::of(self.scene);\n"
                  u8"        a.setClip(self, clipRes);\n"
                  u8"        a.play(self);\n"
                  u8"        a.pause(self, true);\n"
                  u8"        a.stop(self);\n"
                  u8"        if (!a.isPlaying(self)) { self.setName(\"silent\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const Guid clipId{0xAA11, 0xBB22};
    ScriptPropertyDesc clipProp;
    clipProp.name = String(u8"clipRes");
    clipProp.hash = ScriptPropertyNameHash(u8"clipRes");
    clipProp.type = ScriptPropertyType::Asset;
    clipProp.assetType = String(u8"AudioClip");
    clipProp.defaultValue.kind = ScriptPropertyType::Asset;
    clipProp.defaultValue.guid = clipId;
    tweaker->properties.PushBack(clipProp);

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    sources->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(sources->Get(e) != nullptr);
    CHECK(sources->Get(e)->volume == doctest::Approx(0.25f)); // .of data path
    CHECK(sources->Get(e)->pitch == doctest::Approx(2.0f));
    CHECK(sources->Get(e)->loop);
    CHECK(sources->Get(e)->clip.id == clipId);               // SceneAudio.setClip resource swap
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"silent")); // isPlaying() returned a bool
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted); // play/stop/pause all bound and callable
}

TEST_CASE("script.scene: AudioSourceComponent::of props + SceneAudio::of play/stop/setClip "
          "are bound and control the component (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::audio::RegisterAudioScriptFacade();
    ScriptedScene bed;
    auto* sources = bed.scene.AddSystem<engine::audio::AudioSourceComponentManager>();
    (void)bed.scene.AddSystem<engine::audio::AudioSceneSystem>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"AudioTweaker",
        u8"class AudioTweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    AudioTweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        AudioSourceComponent@ s = AudioSourceComponent::of(self);\n"
        u8"        s.volume = 0.25f;\n"
        u8"        s.loop = true;\n"
        u8"        SceneAudio a = SceneAudio::of(self.scene);\n"
        u8"        a.setClip(self, Guid(0xCC33, 0xDD44));\n" // construct the resource id inline
        u8"        a.play(self);\n"
        u8"        a.stop(self);\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});
    const Guid clipId{0xCC33, 0xDD44};

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    sources->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(sources->Get(e) != nullptr);
    CHECK(sources->Get(e)->volume == doctest::Approx(0.25f));
    CHECK(sources->Get(e)->loop);
    CHECK(sources->Get(e)->clip.id == clipId);
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted);
}

TEST_CASE("script.scene: Guid constructs from its canonical string in AngelScript "
          "(the editor text-document spelling)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::audio::RegisterAudioScriptFacade();
    ScriptedScene bed;
    auto* sources = bed.scene.AddSystem<engine::audio::AudioSourceComponentManager>();
    (void)bed.scene.AddSystem<engine::audio::AudioSceneSystem>();

    // Guid("...") parses the canonical spelling, so an id copies straight out of an .xasset
    // instead of being split into Guid(high, low) halves. Malformed input yields Nil (IsNil).
    RefPtr<ScriptClass> writer = MakeClassLang(
        u8"angelscript", u8"GuidWriter",
        u8"class GuidWriter {\n"
        u8"    private Entity@ self;\n"
        u8"    GuidWriter(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        Guid id = Guid(\"00000000-0000-cc33-0000-00000000dd44\");\n"
        u8"        if (Guid(\"not-a-guid\").IsNil() && !id.IsNil()) {\n"
        u8"            SceneAudio::of(self.scene).setClip(self, id);\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(writer, u8"e");
    sources->Add(e);

    bed.Start();
    bed.Frame();

    // The clip id proves the string parsed to the exact halves (and the malformed guard ran).
    REQUIRE(sources->Get(e) != nullptr);
    CHECK(sources->Get(e)->clip.id == Guid{0xCC33, 0xDD44});
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted);
}

TEST_CASE("script.scene: Guid.new constructs from its canonical string in Luau")
{
    ScriptedScene bed;
    PrefabSource prefabs(bed);

    // The prefab's id spelled canonically in source: only a correct parse spawns it.
    String source(u8"Spawner = {}\n"
                  u8"Spawner.__index = Spawner\n"
                  u8"function Spawner.new(entity) return setmetatable({ entity = entity }, Spawner) end\n"
                  u8"function Spawner:onStart()\n"
                  u8"    local id = Guid.new(\"");
    source.Append(prefabs.Canonical().AsView());
    source.Append(u8"\")\n"
                  u8"    if Guid.new(\"not-a-guid\"):IsNil() and not id:IsNil() then\n"
                  u8"        self.entity.scene:spawn(id, 1.0, 2.0, 3.0)\n"
                  u8"    end\n"
                  u8"end\n");
    RefPtr<ScriptClass> spawner = MakeClassLang(u8"luau", u8"Spawner", source.AsView(), {u8"onStart"});

    (void)bed.AddScripted(spawner, u8"spawner");
    bed.Start();
    bed.Frame();

    // spawn received the parsed id (the database knew it); the malformed spelling read as Nil.
    CHECK(PrefabSource::Spawned(bed.scene) == 1u);
    CHECK(Near(bed.scene.GetLocalTransform(bed.scene.FindEntityByName(u8"Turret")).position.y, 2.0f));
}

// An asset (Guid) editor property applies to an AngelScript behaviour when declared as a handle
// member (`Guid@ res;`), the idiomatic AngelScript spelling for a reference type. A plain value
// member is owned by AngelScript's lifecycle and cannot take a native write; the runtime logs a
// warning pointing the author to a handle (see WriteTypedAddress / SetMemberField).
TEST_CASE("script.scene: an asset (Guid) editor property applies to an AngelScript handle member")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    RefPtr<ScriptClass> cls = MakeClassLang(
        u8"angelscript", u8"Holder",
        u8"class Holder {\n"
        u8"    private Entity@ self;\n"
        u8"    Guid@ res;\n" // a reflected or resource property is a handle member
        u8"    Holder(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() { if (res !is null && res.high == 0xEE55) self.setName(\"applied\"); }\n"
        u8"}\n",
        {u8"onStart"});
    ScriptPropertyDesc p;
    p.name = String(u8"res");
    p.hash = ScriptPropertyNameHash(u8"res");
    p.type = ScriptPropertyType::Asset;
    p.assetType = String(u8"Mesh");
    p.defaultValue.kind = ScriptPropertyType::Asset;
    p.defaultValue.guid = Guid{0xEE55, 0xFF66};
    cls->properties.PushBack(p);
    const scene::EntityHandle e = bed.AddScripted(cls, u8"h");
    bed.Start();
    bed.Frame();
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"applied")); // the Guid reached the handle member
}

// The rest of the render component set also reaches script through `.of` (all pure data): Camera,
// Sprite, Decal, InstancedMesh, ReflectionProbe. Spot-check Camera and Sprite from a behaviour.
TEST_CASE("script.scene: CameraComponent.of / SpriteComponent.of set live props")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* cameras = bed.scene.AddSystem<engine::render::CameraComponentManager>();
    auto* sprites = bed.scene.AddSystem<engine::render::SpriteComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"ViewTweaker",
                  u8"class ViewTweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    ViewTweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        CameraComponent@ cam = CameraComponent::of(self);\n"
                  u8"        cam.fovYRadians = 1.2f;\n"
                  u8"        cam.primary = false;\n"
                  u8"        SpriteComponent::of(self).visible = false;\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    cameras->Add(e).primary = true;
    sprites->Add(e).visible = true;

    bed.Start();
    bed.Frame();

    REQUIRE(cameras->Get(e) != nullptr);
    CHECK(cameras->Get(e)->fovYRadians == doctest::Approx(1.2f));
    CHECK_FALSE(cameras->Get(e)->primary);
    REQUIRE(sprites->Get(e) != nullptr);
    CHECK_FALSE(sprites->Get(e)->visible);
}

// ---- Animation surface: SkeletalAnimationComponent.of / AnimationGraphComponent.of (live
//      speed/autoPlay/active, data) and SceneAnimation.of(scene) (play/stop/pause, graph params
//      setFloat/setBool/setTrigger, and setClip: world ops keyed by entity that reach the
//      manager-owned runtime player). No skeleton or graph is bound here, so the players are never
//      built and the world ops are safe no-ops; this covers the surface, forwarding and the data and
//      resource paths on both backends. Playback is covered natively in the animation tests. ----

TEST_CASE("script.scene: SkeletalAnimation/AnimationGraph .of + SceneAnimation ops are bound and "
          "control the components")
{
    engine::animation::RegisterAnimationScriptFacade();
    ScriptedScene bed;
    auto* skel = bed.scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    auto* graph = bed.scene.AddSystem<engine::animation::AnimationGraphComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"AnimTweaker",
                  u8"class AnimTweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ clipRes;\n"
                  u8"    AnimTweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        SkeletalAnimationComponent@ a = SkeletalAnimationComponent::of(self);\n"
                  u8"        a.speed = 2.0f;\n"
                  u8"        a.autoPlay = false;\n"
                  u8"        AnimationGraphComponent::of(self).active = false;\n"
                  u8"        SceneAnimation@ s = SceneAnimation::of(self.scene);\n"
                  u8"        s.setClip(self, clipRes);\n"
                  u8"        s.play(self);\n"
                  u8"        s.stop(self);\n"
                  u8"        s.setFloat(self, \"speed\", 0.5f);\n"
                  u8"        s.setBool(self, \"grounded\", true);\n"
                  u8"        s.setTrigger(self, \"jump\");\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const Guid clipId{0x9911, 0x8822};
    ScriptPropertyDesc clipProp;
    clipProp.name = String(u8"clipRes");
    clipProp.hash = ScriptPropertyNameHash(u8"clipRes");
    clipProp.type = ScriptPropertyType::Asset;
    clipProp.assetType = String(u8"AnimationClip");
    clipProp.defaultValue.kind = ScriptPropertyType::Asset;
    clipProp.defaultValue.guid = clipId;
    tweaker->properties.PushBack(clipProp);

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    skel->Add(e);
    graph->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(skel->Get(e) != nullptr);
    CHECK(skel->Get(e)->speed == doctest::Approx(2.0f)); // .of data path
    CHECK_FALSE(skel->Get(e)->autoPlay);
    CHECK(skel->Get(e)->clip.id == clipId);              // SceneAnimation.setClip resource swap
    REQUIRE(graph->Get(e) != nullptr);
    CHECK_FALSE(graph->Get(e)->active);
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted); // play/stop/setFloat/setBool/setTrigger all bound
}

TEST_CASE("script.scene: SkeletalAnimation::of + SceneAnimation ops are bound (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::animation::RegisterAnimationScriptFacade();
    ScriptedScene bed;
    auto* skel = bed.scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    (void)bed.scene.AddSystem<engine::animation::AnimationGraphComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"AnimTweaker",
        u8"class AnimTweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    AnimTweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        SkeletalAnimationComponent@ a = SkeletalAnimationComponent::of(self);\n"
        u8"        a.speed = 2.0f;\n"
        u8"        a.autoPlay = false;\n"
        u8"        SceneAnimation anim = SceneAnimation::of(self.scene);\n"
        u8"        anim.setClip(self, Guid(0x9911, 0x8822));\n"
        u8"        anim.play(self);\n"
        u8"        anim.setFloat(self, \"speed\", 0.5f);\n"
        u8"        anim.setTrigger(self, \"jump\");\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    skel->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(skel->Get(e) != nullptr);
    CHECK(skel->Get(e)->speed == doctest::Approx(2.0f));
    CHECK_FALSE(skel->Get(e)->autoPlay);
    CHECK(skel->Get(e)->clip.id == Guid{0x9911, 0x8822});
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted);
}

// ---- Render scene systems: EnvironmentSettings.of(scene) / PostProcessSettings.of(scene), a
//      re-resolving handle over the scene's one-per-scene render settings (owned by the
//      EnvironmentSystem / PostProcessSystem). A behaviour edits the live sky and ambient, exposure
//      and bloom: the scene-scoped analogue of a component's `.of`. ----

TEST_CASE("script.scene: EnvironmentSettings.of / PostProcessSettings.of edit the scene's live "
          "render settings")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* envSys = bed.scene.AddSystem<engine::render::EnvironmentSystem>();
    auto* postSys = bed.scene.AddSystem<engine::render::PostProcessSystem>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"EnvTweaker",
                  u8"class EnvTweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    EnvTweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        EnvironmentSettings@ env = EnvironmentSettings::of(self.scene);\n"
                  u8"        env.ambientIntensity = 0.75f;\n"
                  u8"        env.skyIntensity = 2.0f;\n"
                  u8"        PostProcessSettings@ post = PostProcessSettings::of(self.scene);\n"
                  u8"        post.exposureEV = 1.5f;\n"
                  u8"        post.bloomEnabled = true;\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    (void)bed.AddScripted(tweaker, u8"e");
    // Sanity: defaults differ from what the behavior sets.
    REQUIRE(envSys->Environment().ambientIntensity != doctest::Approx(0.75f));

    bed.Start();
    bed.Frame(); // onStart edits the live settings through the re-resolving handles

    CHECK(envSys->Environment().ambientIntensity == doctest::Approx(0.75f));
    CHECK(envSys->Environment().skyIntensity == doctest::Approx(2.0f));
    CHECK(postSys->Post().exposureEV == doctest::Approx(1.5f));
    CHECK(postSys->Post().bloomEnabled);
}

TEST_CASE("script.scene: EnvironmentSettings::of / PostProcessSettings::of edit the scene's live "
          "render settings (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* envSys = bed.scene.AddSystem<engine::render::EnvironmentSystem>();
    auto* postSys = bed.scene.AddSystem<engine::render::PostProcessSystem>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"EnvTweaker",
        u8"class EnvTweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    EnvTweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        EnvironmentSettings@ env = EnvironmentSettings::of(self.scene);\n"
        u8"        env.ambientIntensity = 0.75f;\n"
        u8"        PostProcessSettings@ post = PostProcessSettings::of(self.scene);\n"
        u8"        post.exposureEV = 1.5f;\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    (void)bed.AddScripted(tweaker, u8"e");
    bed.Start();
    bed.Frame();

    CHECK(envSys->Environment().ambientIntensity == doctest::Approx(0.75f));
    CHECK(postSys->Post().exposureEV == doctest::Approx(1.5f));
}

// ---- Particle surface: ParticleEffectComponent.of (live visible/meshScale/light*, data) and
//      SceneParticles.of(scene) (play/stop/restart/pause/isPlaying and setEffect: world ops keyed
//      by entity that reach the manager-owned runtime ParticleEffectInstance). No effect is attached
//      here, so the instance is never built and the world ops are safe no-ops; this covers the
//      surface, forwarding and the data and resource paths on both backends. The simulation is
//      tested natively. ----

TEST_CASE("script.scene: ParticleEffectComponent.of + SceneParticles.of ops are bound and control "
          "the component")
{
    engine::particles::RegisterParticleScriptFacade();
    ScriptedScene bed;
    auto* fx = bed.scene.AddSystem<engine::particles::ParticleEffectComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"FxTweaker",
                  u8"class FxTweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Guid@ effectRes;\n"
                  u8"    FxTweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        ParticleEffectComponent@ c = ParticleEffectComponent::of(self);\n"
                  u8"        c.visible = false;\n"
                  u8"        c.lightIntensity = 8.0f;\n"
                  u8"        SceneParticles@ p = SceneParticles::of(self.scene);\n"
                  u8"        p.setEffect(self, effectRes);\n"
                  u8"        p.play(self);\n"
                  u8"        p.pause(self, true);\n"
                  u8"        p.restart(self);\n"
                  u8"        p.stop(self);\n"
                  u8"        if (!p.isPlaying(self)) { self.setName(\"idle\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const Guid fxId{0x7711, 0x6622};
    ScriptPropertyDesc effectProp;
    effectProp.name = String(u8"effectRes");
    effectProp.hash = ScriptPropertyNameHash(u8"effectRes");
    effectProp.type = ScriptPropertyType::Asset;
    effectProp.assetType = String(u8"ParticleEffect");
    effectProp.defaultValue.kind = ScriptPropertyType::Asset;
    effectProp.defaultValue.guid = fxId;
    tweaker->properties.PushBack(effectProp);

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    fx->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(fx->Get(e) != nullptr);
    CHECK_FALSE(fx->Get(e)->visible);                             // .of data path
    CHECK(fx->Get(e)->lightIntensity == doctest::Approx(8.0f));
    CHECK(fx->Get(e)->effectAsset.id == fxId);                    // SceneParticles.setEffect resource swap
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"idle"));    // isPlaying() returned a bool
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted); // play/stop/restart/pause all bound and callable
}

TEST_CASE("script.scene: ParticleEffectComponent::of + SceneParticles::of ops are bound (AngelScript)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::particles::RegisterParticleScriptFacade();
    ScriptedScene bed;
    auto* fx = bed.scene.AddSystem<engine::particles::ParticleEffectComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"FxTweaker",
        u8"class FxTweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    FxTweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        ParticleEffectComponent@ c = ParticleEffectComponent::of(self);\n"
        u8"        c.visible = false;\n"
        u8"        c.lightIntensity = 8.0f;\n"
        u8"        SceneParticles p = SceneParticles::of(self.scene);\n"
        u8"        p.setEffect(self, Guid(0x7711, 0x6622));\n"
        u8"        p.play(self);\n"
        u8"        p.stop(self);\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    fx->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(fx->Get(e) != nullptr);
    CHECK_FALSE(fx->Get(e)->visible);
    CHECK(fx->Get(e)->lightIntensity == doctest::Approx(8.0f));
    CHECK(fx->Get(e)->effectAsset.id == Guid{0x7711, 0x6622});
    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted);
}

// Enum-typed component properties cross to script as their underlying int. LightComponent.type is
// a LightType enum (Directional=0, Point=1, Spot=2).
TEST_CASE("script.scene: an enum component property reads/writes as an int")
{
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* lights = bed.scene.AddSystem<engine::render::LightComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"EnumTest",
                  u8"class EnumTest {\n"
                  u8"    private Entity@ self;\n"
                  u8"    EnumTest(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        LightComponent@ l = LightComponent::of(self);\n"
                  u8"        if (l.type == 0) { l.type = LightType(2); }\n" // Directional(0) -> Spot(2)
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    lights->Add(e).type = engine::render::LightType::Directional; // 0

    bed.Start();
    bed.Frame();

    REQUIRE(lights->Get(e) != nullptr);
    CHECK(lights->Get(e)->type == engine::render::LightType::Spot); // read (==0) and write (=2) both crossed
}

// AngelScript gets native (named) enums. LightComponent.type is a LightType enum.
TEST_CASE("script.scene: an enum component property reads/writes as a native enum in AngelScript")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    engine::render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* lights = bed.scene.AddSystem<engine::render::LightComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"EnumTest",
        u8"class EnumTest {\n"
        u8"    private Entity@ self;\n"
        u8"    EnumTest(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        LightComponent@ l = LightComponent::of(self);\n"
        u8"        if (l.type == LightType::Directional) { l.type = LightType::Spot; }\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    lights->Add(e).type = engine::render::LightType::Directional;

    bed.Start();
    bed.Frame();

    REQUIRE(lights->Get(e) != nullptr);
    CHECK(lights->Get(e)->type == engine::render::LightType::Spot); // named-enum read and write crossed
}

// ---- Net surface: NetworkComponent.of, authority (an enum) for authority-gated gameplay.
//      Replication owns the identity: a script reads authority and id and assigns neither (the
//      property flag is pinned in Net.Replication.Tests, the missing setter in
//      Script.AngelScript.Tests; here, the gate works and the assignment never runs).
TEST_CASE("script.scene: NetworkComponent.of exposes replication authority - AngelScript (named enum)")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    foundation::net::RegisterNetworkComponentScriptFacade();
    ScriptedScene bed;
    auto* net = bed.scene.AddSystem<foundation::net::NetworkComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"NetTweaker",
        u8"class NetTweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    NetTweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        NetworkComponent@ n = NetworkComponent::of(self);\n"
        u8"        if (n.authority == NetworkAuthority::Server) { self.setName(\"authoritative\"); }\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    net->Add(e).authority = foundation::net::NetworkAuthority::Server;

    bed.Start();
    bed.Frame();

    REQUIRE(net->Get(e) != nullptr);
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"authoritative"));
    CHECK(net->Get(e)->authority == foundation::net::NetworkAuthority::Server); // read, untouched
}

TEST_CASE("script.scene: a script that assigns NetworkComponent.authority never starts - no setter exists")
{
    foundation::script::angelscript::RegisterAngelScriptBackend();
    foundation::net::RegisterNetworkComponentScriptFacade();
    ScriptedScene bed;
    auto* net = bed.scene.AddSystem<foundation::net::NetworkComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"NetWriter",
        u8"class NetWriter {\n"
        u8"    private Entity@ self;\n"
        u8"    NetWriter(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        self.setName(\"started\");\n"
        u8"        NetworkComponent::of(self).authority = NetworkAuthority::Client;\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});
    // The id too: assigning it to itself is only refused because no setter exists.
    RefPtr<ScriptClass> renamer = MakeClassLang(
        u8"angelscript", u8"IdWriter",
        u8"class IdWriter {\n"
        u8"    private Entity@ self;\n"
        u8"    IdWriter(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        self.setName(\"started\");\n"
        u8"        NetworkComponent@ n = NetworkComponent::of(self);\n"
        u8"        n.id = n.id;\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    net->Add(e).authority = foundation::net::NetworkAuthority::Server;
    const scene::EntityHandle f = bed.AddScripted(renamer, u8"f");
    net->Add(f);

    bed.Start();
    bed.Frame();

    // Both classes fail to compile (no setter), so neither onStart runs.
    REQUIRE(net->Get(e) != nullptr);
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"e"));
    CHECK(net->Get(e)->authority == foundation::net::NetworkAuthority::Server);
    CHECK(bed.scene.GetEntityName(f) == StringView(u8"f"));
}

TEST_CASE("script.scene: NetworkComponent.of authority crosses as an int")
{
    foundation::net::RegisterNetworkComponentScriptFacade();
    ScriptedScene bed;
    auto* net = bed.scene.AddSystem<foundation::net::NetworkComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"NetTweaker",
                  u8"class NetTweaker {\n"
                  u8"    private Entity@ self;\n"
                  u8"    NetTweaker(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        NetworkComponent@ n = NetworkComponent::of(self);\n"
                  u8"        if (n.authority == 0) { self.setName(\"authoritative\"); }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    net->Add(e).authority = foundation::net::NetworkAuthority::Server;

    bed.Start();
    bed.Frame();

    REQUIRE(net->Get(e) != nullptr);
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"authoritative"));
    CHECK(net->Get(e)->authority == foundation::net::NetworkAuthority::Server); // read, untouched
}

// The entity's identity, its local transform by value and the hierarchy: a behaviour walks its
// children, reorders and reparents them, and moves one by Float3 and Quaternion. Shared by both
// backends; each script writes the same outcome.
namespace
{
    void CheckEntityHierarchyScript(StringView language, StringView className, StringView source)
    {
        ScriptedScene bed;
        RefPtr<ScriptClass> walker = MakeClassLang(language, className, source, {u8"onStart"});
        const scene::EntityHandle root = bed.AddScripted(walker, u8"root");
        const scene::EntityHandle a = bed.scene.CreateEntity(u8"a");
        const scene::EntityHandle b = bed.scene.CreateEntity(u8"b");
        const scene::EntityHandle other = bed.scene.CreateEntity(u8"other");
        bed.scene.SetParent(a, root);
        bed.scene.SetParent(b, root);

        bed.Start();
        bed.Frame();

        ScriptComponent* comp = bed.components->Get(root);
        REQUIRE(comp != nullptr);
        CHECK_FALSE(comp->behaviors[0].faulted);
        // The walk read root -> a -> b with no parent above the root, and the id it read was
        // the root's own: the script names itself "walked" only then.
        CHECK(bed.scene.GetEntityName(root) == StringView(u8"walked"));
        // b moved before a; a went under `other`, keeping its local transform.
        CHECK(bed.scene.GetFirstChild(root) == b);
        CHECK(bed.scene.GetParent(a) == other);
        const Transform moved = bed.scene.GetLocalTransform(a);
        CHECK(Near(moved.position.x, 1.0f));
        CHECK(Near(moved.position.y, 2.0f));
        CHECK(Near(moved.position.z, 3.0f));
        CHECK(Near(moved.scale.y, 2.0f));
        // b took a's rotation by value (a quarter turn about +Y).
        const Quaternion turned = bed.scene.GetLocalTransform(b).rotation;
        const Quaternion source90 = bed.scene.GetLocalTransform(a).rotation;
        CHECK(Near(turned.y, source90.y));
        CHECK(Near(turned.w, source90.w));
        CHECK(Abs(turned.y) > 0.5f);
    }
}

TEST_CASE("script.scene: an AngelScript behaviour walks, reorders and reparents its children, "
          "and moves one by Float3 and Quaternion")
{
    CheckEntityHierarchyScript(
        u8"angelscript", u8"Walker",
        u8"class Walker {\n"
        u8"    private Entity@ self;\n"
        u8"    Walker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        Entity@ a = self.firstChild();\n"
        u8"        Entity@ b = a.nextSibling();\n"
        u8"        bool walked = !self.parent().isValid() && a.name() == \"a\" && b.name() == \"b\"\n"
        u8"            && !b.nextSibling().isValid() && self.findChildByName(\"b\").name() == \"b\"\n"
        u8"            && !self.id().IsNil() && self.scene.find(\"root\").id().high == self.id().high\n"
        u8"            && self.scene.find(\"root\").id().low == self.id().low;\n"
        u8"        b.moveBefore(a);\n"
        u8"        a.setParent(self.scene.find(\"other\"));\n"
        u8"        a.setPosition(Float3(1, 2, 3));\n"
        u8"        a.setScale(Float3(2, 2, 2));\n"
        u8"        a.setRotationEuler(0, 90, 0);\n"
        u8"        b.setRotation(a.rotation());\n"
        u8"        if (walked) { self.setName(\"walked\"); }\n"
        u8"    }\n"
        u8"}\n");
}

TEST_CASE("script.scene: a Luau behaviour walks, reorders and reparents its children, and moves "
          "one by Float3 and Quaternion")
{
    CheckEntityHierarchyScript(
        u8"luau", u8"Walker",
        u8"Walker = {}\n"
        u8"Walker.__index = Walker\n"
        u8"function Walker.new(entity) return setmetatable({ entity = entity }, Walker) end\n"
        u8"function Walker:onStart()\n"
        u8"    local me = self.entity\n"
        u8"    local a = me:firstChild()\n"
        u8"    local b = a:nextSibling()\n"
        u8"    local id = me:id()\n"
        u8"    local walked = not me:parent():isValid() and a:name() == \"a\" and b:name() == \"b\"\n"
        u8"        and not b:nextSibling():isValid() and me:findChildByName(\"b\"):name() == \"b\"\n"
        u8"        and not id:IsNil()\n" // Luau boxes the u64 halves: compared in AngelScript
        u8"    b:moveBefore(a)\n"
        u8"    a:setParent(me.scene:find(\"other\"))\n"
        u8"    a:setPosition(Float3.new(1, 2, 3))\n"
        u8"    a:setScale(Float3.new(2, 2, 2))\n"
        u8"    a:setRotationEuler(0, 90, 0)\n"
        u8"    b:setRotation(a:rotation())\n"
        u8"    if walked then me:setName(\"walked\") end\n"
        u8"end\n");
}

// ScenePrefabs.of(scene): a prefab by asset id at a Float3, turned by a Quaternion, under a
// parent. Both backends spawn the same Turret the same way.
namespace
{
    void CheckScenePrefabsScript(StringView language, StringView className, StringView prefix,
                                 StringView suffix)
    {
        ScriptedScene bed;
        PrefabSource prefabs(bed);
        String source(prefix);
        source.Append(prefabs.Canonical().AsView());
        source.Append(suffix);
        RefPtr<ScriptClass> spawner = MakeClassLang(language, className, source.AsView(), {u8"onStart"});
        const scene::EntityHandle holder = bed.AddScripted(spawner, u8"holder");

        bed.Start();
        bed.Frame();

        ScriptComponent* comp = bed.components->Get(holder);
        REQUIRE(comp != nullptr);
        CHECK_FALSE(comp->behaviors[0].faulted);
        // One at a root by position alone (Spawned counts roots), one under the holder, turned;
        // an unknown id spawns nothing.
        CHECK(PrefabSource::Spawned(bed.scene) == 1u);
        const scene::EntityHandle child = bed.scene.FindChildByName(holder, u8"Turret");
        REQUIRE(child.IsAssigned());
        const Transform placed = bed.scene.GetLocalTransform(child);
        CHECK(Near(placed.position.y, 2.0f));
        CHECK(Abs(placed.rotation.y) > 0.5f);
        CHECK(bed.scene.GetEntityName(holder) == StringView(u8"spawned"));
    }
}

TEST_CASE("script.scene: ScenePrefabs spawns by asset id at a position, turned, under a parent "
          "(AngelScript)")
{
    CheckScenePrefabsScript(
        u8"angelscript", u8"Spawner",
        u8"class Spawner {\n"
        u8"    private Entity@ self;\n"
        u8"    Spawner(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        Guid id = Guid(\"",
        u8"\");\n"
        u8"        ScenePrefabs prefabs = ScenePrefabs::of(self.scene);\n"
        u8"        Entity@ atRoot = prefabs.spawn(id, Float3(5, 0, 0));\n"
        u8"        self.setRotationEuler(0, 90, 0);\n"
        u8"        Entity@ under = prefabs.spawn(id, Float3(0, 2, 0), self.rotation(), self);\n"
        u8"        Entity@ none = prefabs.spawn(Guid(\"not-a-guid\"), Float3(0, 0, 0));\n"
        u8"        self.setRotationEuler(0, 0, 0);\n"
        u8"        if (atRoot.isValid() && under.isValid() && !none.isValid()\n"
        u8"            && !atRoot.parent().isValid() && under.parent().name() == \"holder\") {\n"
        u8"            self.setName(\"spawned\");\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n");
}

TEST_CASE("script.scene: ScenePrefabs spawns by asset id at a position, turned, under a parent "
          "(Luau)")
{
    CheckScenePrefabsScript(
        u8"luau", u8"Spawner",
        u8"Spawner = {}\n"
        u8"Spawner.__index = Spawner\n"
        u8"function Spawner.new(entity) return setmetatable({ entity = entity }, Spawner) end\n"
        u8"function Spawner:onStart()\n"
        u8"    local me = self.entity\n"
        u8"    local id = Guid.new(\"",
        u8"\")\n"
        u8"    local prefabs = ScenePrefabs.of(me.scene)\n"
        u8"    local atRoot = prefabs:spawn(id, Float3.new(5, 0, 0))\n"
        u8"    me:setRotationEuler(0, 90, 0)\n"
        u8"    local under = prefabs:spawn(id, Float3.new(0, 2, 0), me:rotation(), me)\n"
        u8"    local none = prefabs:spawn(Guid.new(\"not-a-guid\"), Float3.new(0, 0, 0))\n"
        u8"    me:setRotationEuler(0, 0, 0)\n"
        u8"    if atRoot:isValid() and under:isValid() and not none:isValid()\n"
        u8"        and not atRoot:parent():isValid() and under:parent():name() == \"holder\" then\n"
        u8"        me:setName(\"spawned\")\n"
        u8"    end\n"
        u8"end\n");
}

// SceneScripts.of(scene): the scene's script time, send to another entity's behaviours, emit on
// the scene's bus, and a behaviour added by class id (queued after the tick and present on the
// entity next frame; a nil id is refused).
namespace
{
    void CheckSceneScriptsScript(StringView language, StringView bossSource, StringView receiverSource)
    {
        engine::script::RegisterScriptSceneFacade();
        ScriptedScene bed;
        const Guid added{0xAB12, 0xCD34};
        RefPtr<ScriptClass> boss = MakeClassLang(language, u8"Boss", bossSource, {u8"onStart", u8"onUpdate"});
        RefPtr<ScriptClass> receiver =
            MakeClassLang(language, u8"Receiver", receiverSource, {u8"onPing", u8"onHello"});
        const scene::EntityHandle target = bed.AddScripted(receiver, u8"target");
        const scene::EntityHandle bossEntity = bed.AddScripted(boss, u8"boss");
        const scene::EntityHandle asked = bed.scene.CreateEntity(u8"flagA");
        const scene::EntityHandle heard = bed.scene.CreateEntity(u8"flagB");

        bed.Start();
        bed.Frame();
        bed.Frame();

        ScriptComponent* bossComp = bed.components->Get(bossEntity);
        REQUIRE(bossComp != nullptr);
        CHECK_FALSE(bossComp->behaviors[0].faulted);
        CHECK(bed.scene.GetEntityName(asked) == StringView(u8"asked"));   // added, nil refused
        CHECK(bed.scene.GetEntityName(target) == StringView(u8"pinged")); // send(target, "Ping", 3)
        CHECK(bed.scene.GetEntityName(heard) == StringView(u8"heard"));   // emit("Hello", 5)
        CHECK(bed.scene.GetEntityName(bossEntity) == StringView(u8"timed")); // 0.5 s frames
        // The added behaviour landed after the tick, behind the authored one.
        ScriptComponent* targetComp = bed.components->Get(target);
        REQUIRE(targetComp != nullptr);
        REQUIRE(targetComp->behaviors.Size() == 2u);
        CHECK(targetComp->behaviors[1].script.id == added);
    }
}

TEST_CASE("script.scene: SceneScripts gives the scene's script time, sends, emits and adds a "
          "behaviour by class id (AngelScript)")
{
    CheckSceneScriptsScript(
        u8"angelscript",
        u8"class Boss {\n"
        u8"    private Entity@ self;\n"
        u8"    private int frames = 0;\n"
        u8"    Boss(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        SceneScripts scripts = SceneScripts::of(self.scene);\n"
        u8"        Entity@ target = self.scene.find(\"target\");\n"
        u8"        bool added = scripts.addBehavior(target, Guid(0xAB12, 0xCD34));\n"
        u8"        bool refused = !scripts.addBehavior(target, Guid(\"not-a-guid\"));\n"
        u8"        scripts.send(target, \"Ping\", 3);\n"
        u8"        scripts.emit(\"Hello\", 5);\n"
        u8"        if (added && refused) { self.scene.find(\"flagA\").setName(\"asked\"); }\n"
        u8"    }\n"
        u8"    void onUpdate(float dt) {\n"
        u8"        SceneScripts scripts = SceneScripts::of(self.scene);\n"
        u8"        frames++;\n"
        u8"        if (frames == 2 && scripts.deltaTime() == 0.5f && scripts.elapsed() > 0.9) {\n"
        u8"            self.setName(\"timed\");\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n",
        u8"class Receiver {\n"
        u8"    private Entity@ self;\n"
        u8"    Receiver(Entity@ entity) { @self = entity; }\n"
        u8"    void onPing(int value) { if (value == 3) { self.setName(\"pinged\"); } }\n"
        u8"    void onHello(int value) { if (value == 5) { self.scene.find(\"flagB\").setName(\"heard\"); } }\n"
        u8"}\n");
}

TEST_CASE("script.scene: SceneScripts gives the scene's script time, sends, emits and adds a "
          "behaviour by class id (Luau)")
{
    CheckSceneScriptsScript(
        u8"luau",
        u8"Boss = {}\n"
        u8"Boss.__index = Boss\n"
        u8"function Boss.new(entity) return setmetatable({ entity = entity, frames = 0 }, Boss) end\n"
        u8"function Boss:onStart()\n"
        u8"    local me = self.entity\n"
        u8"    local scripts = SceneScripts.of(me.scene)\n"
        u8"    local target = me.scene:find(\"target\")\n"
        u8"    local added = scripts:addBehavior(target, Guid.new(\"00000000-0000-ab12-0000-00000000cd34\"))\n"
        u8"    local refused = not scripts:addBehavior(target, Guid.new(\"not-a-guid\"))\n"
        u8"    scripts:send(target, \"Ping\", 3)\n"
        u8"    scripts:emit(\"Hello\", 5)\n"
        u8"    if added and refused then me.scene:find(\"flagA\"):setName(\"asked\") end\n"
        u8"end\n"
        u8"function Boss:onUpdate(dt)\n"
        u8"    local scripts = SceneScripts.of(self.entity.scene)\n"
        u8"    self.frames = self.frames + 1\n"
        u8"    if self.frames == 2 and scripts:deltaTime() == 0.5 and scripts:elapsed() > 0.9 then\n"
        u8"        self.entity:setName(\"timed\")\n"
        u8"    end\n"
        u8"end\n",
        u8"Receiver = {}\n"
        u8"Receiver.__index = Receiver\n"
        u8"function Receiver.new(entity) return setmetatable({ entity = entity }, Receiver) end\n"
        u8"function Receiver:onPing(value) if value == 3 then self.entity:setName(\"pinged\") end end\n"
        u8"function Receiver:onHello(value)\n"
        u8"    if value == 5 then self.entity.scene:find(\"flagB\"):setName(\"heard\") end\n"
        u8"end\n");
}

// The scene makes, ends and finds entities by id, and names itself; Random restarts from a seed.
// The script checks what it can, the test the rest.
namespace
{
    void CheckSceneEntitiesScript(StringView language, StringView source)
    {
        ScriptedScene bed;
        RefPtr<ScriptClass> maker = MakeClassLang(language, u8"Maker", source, {u8"onStart"});
        const scene::EntityHandle holder = bed.AddScripted(maker, u8"holder");
        const u32 before = bed.scene.EntityCount();

        bed.Start();
        bed.Frame();

        ScriptComponent* comp = bed.components->Get(holder);
        REQUIRE(comp != nullptr);
        CHECK_FALSE(comp->behaviors[0].faulted);
        CHECK(bed.scene.GetEntityName(holder) == StringView(u8"made"));
        // "kept" stays; "gone" was made and destroyed, gone once the frame ended.
        CHECK(bed.scene.FindEntityByName(u8"kept").IsAssigned());
        CHECK_FALSE(bed.scene.FindEntityByName(u8"gone").IsAssigned());
        CHECK(bed.scene.EntityCount() == before + 2u); // kept + the unnamed one
    }
}

TEST_CASE("script.scene: a behaviour makes, ends and finds entities by id; Random restarts from a "
          "seed (AngelScript)")
{
    CheckSceneEntitiesScript(
        u8"angelscript",
        u8"class Maker {\n"
        u8"    private Entity@ self;\n"
        u8"    Maker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        Scene@ scene = self.scene;\n"
        u8"        uint count = scene.entityCount();\n"
        u8"        Entity@ kept = scene.createEntity(\"kept\");\n"
        u8"        Entity@ unnamed = scene.createEntity();\n"
        u8"        Entity@ gone = scene.createEntity(\"gone\");\n"
        u8"        scene.destroyEntity(gone);\n"
        u8"        Entity@ found = scene.findEntity(kept.id());\n"
        u8"        Random::seed(42);\n"
        u8"        float first = Random::value();\n"
        u8"        bool flag = Random::boolean();\n"
        u8"        Random::seed(42);\n"
        u8"        bool again = Random::value() == first && Random::boolean() == flag;\n"
        // A destroy during play lands at the frame's end: `gone` still counts here.
        u8"        if (found.isValid() && found.name() == \"kept\" && unnamed.isValid()\n"
        u8"            && scene.name() == \"script-test\" && scene.entityCount() == count + 3\n"
        u8"            && !scene.findEntity(Guid(\"not-a-guid\")).isValid() && again) {\n"
        u8"            self.setName(\"made\");\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n");
}

TEST_CASE("script.scene: a behaviour makes, ends and finds entities by id; Random restarts from a "
          "seed (Luau)")
{
    CheckSceneEntitiesScript(
        u8"luau",
        u8"Maker = {}\n"
        u8"Maker.__index = Maker\n"
        u8"function Maker.new(entity) return setmetatable({ entity = entity }, Maker) end\n"
        u8"function Maker:onStart()\n"
        u8"    local me = self.entity\n"
        u8"    local scene = me.scene\n"
        u8"    local count = scene:entityCount()\n"
        u8"    local kept = scene:createEntity(\"kept\")\n"
        u8"    local unnamed = scene:createEntity()\n"
        u8"    local gone = scene:createEntity(\"gone\")\n"
        u8"    scene:destroyEntity(gone)\n"
        u8"    local found = scene:findEntity(kept:id())\n"
        u8"    Random.seed(42)\n"
        u8"    local first = Random.value()\n"
        u8"    local flag = Random.boolean()\n"
        u8"    Random.seed(42)\n"
        u8"    local again = Random.value() == first and Random.boolean() == flag\n"
        u8"    if found:isValid() and found:name() == \"kept\" and unnamed:isValid()\n"
        u8"        and scene:name() == \"script-test\" and scene:entityCount() == count + 3\n"
        u8"        and not scene:findEntity(Guid.new(\"not-a-guid\")):isValid() and again then\n"
        u8"        me:setName(\"made\")\n"
        u8"    end\n"
        u8"end\n");
}

// ScenePhysics in its Float3 forms: a ray down onto a body (and the material it is made of), the
// overlap set by a center, an impulse by a Float3, gravity by a Float3. Both backends act the same
// way.
namespace
{
    void CheckPhysicsFloat3Script(StringView language, StringView className, StringView source)
    {
        engine::physics::RegisterPhysicsScriptFacade();
        ContactWorld world;
        const scene::EntityHandle a = world.AddBody(u8"left", Float3{-1.5f, 0, 0},
                                                    physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
        const scene::EntityHandle b = world.AddBody(u8"right", Float3{1.5f, 0, 0},
                                                    physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
        // `left` is made of a material (the ray reads its id back: RayCastHit.material).
        world.scene->GetSystem<engine::physics::RigidBodyComponentManager>()->Get(a)->material.SetId(Guid{7, 9});
        const scene::EntityHandle scanner = world.scene->CreateEntity(u8"scanner");
        world.Attach(scanner, MakeClassLang(language, className, source, {u8"onStart"}));

        world.Play(2);
        auto* sys = world.scene->GetSystem<engine::physics::PhysicsSceneSystem>();
        auto* bodies = world.scene->GetSystem<engine::physics::RigidBodyComponentManager>();
        REQUIRE(sys->World() != nullptr);
        CHECK(world.scene->GetEntityName(scanner) == StringView(u8"hit")); // the ray found `left`
        CHECK(sys->World()->Gravity().y == doctest::Approx(-20.0f));
        CHECK(sys->World()->LinearVelocity(bodies->Get(a)->body).y > 0.0f);
        CHECK(sys->World()->LinearVelocity(bodies->Get(b)->body).y > 0.0f);
    }
}

TEST_CASE("script.scene: ScenePhysics casts, overlaps, impulses and gravity by Float3 (AngelScript)")
{
    CheckPhysicsFloat3Script(
        u8"angelscript", u8"Scanner",
        u8"class Scanner {\n"
        u8"    private Entity@ self;\n"
        u8"    Scanner(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        ScenePhysics@ p = ScenePhysics::of(self.scene);\n"
        u8"        p.setGravity(Float3(0, -20, 0));\n"
        u8"        RayCastHit@ down = p.rayCast(Float3(-1.5f, 10, 0), Float3(0, -1, 0), 20.0f);\n"
        u8"        RayCastHit@ masked = p.rayCast(Float3(-1.5f, 10, 0), Float3(0, -1, 0), 20.0f, 0);\n"
        u8"        RayCastHit@ near = p.nearestOverlap(Float3(1.4f, 0, 0), 1.0f);\n"
        u8"        if (down.hit && down.entity().name() == \"left\" && down.material().high == 7 && !masked.hit\n"
        u8"            && near.hit && near.material().IsNil()\n"
        u8"            && near.entity().name() == \"right\" && p.gravity().y == -20.0f) {\n"
        u8"            self.setName(\"hit\");\n"
        u8"        }\n"
        u8"        array<Entity@>@ hits = p.overlapSphere(Float3(0, 0, 0), 3.0f);\n"
        u8"        for (uint i = 0; i < hits.length(); i++) {\n"
        u8"            p.applyImpulse(hits[i], Float3(0, 5000, 0));\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n");
}

TEST_CASE("script.scene: ScenePhysics casts, overlaps, impulses and gravity by Float3 (Luau)")
{
    CheckPhysicsFloat3Script(
        u8"luau", u8"Scanner",
        u8"Scanner = {}\n"
        u8"Scanner.__index = Scanner\n"
        u8"function Scanner.new(entity) return setmetatable({ entity = entity }, Scanner) end\n"
        u8"function Scanner:onStart()\n"
        u8"    local me = self.entity\n"
        u8"    local p = ScenePhysics.of(me.scene)\n"
        u8"    p:setGravity(Float3.new(0, -20, 0))\n"
        u8"    local down = p:rayCast(Float3.new(-1.5, 10, 0), Float3.new(0, -1, 0), 20.0)\n"
        u8"    local masked = p:rayCast(Float3.new(-1.5, 10, 0), Float3.new(0, -1, 0), 20.0, 0)\n"
        u8"    local near = p:nearestOverlap(Float3.new(1.4, 0, 0), 1.0)\n"
        u8"    if down.hit and down:entity():name() == \"left\" and down:material() == Guid.new(7, 9)\n"
        u8"        and not masked.hit and near.hit and near:material() == Guid.new(0, 0)\n"
        u8"        and near:entity():name() == \"right\" and p:gravity().y == -20 then\n"
        u8"        me:setName(\"hit\")\n"
        u8"    end\n"
        u8"    local hits = p:overlapSphere(Float3.new(0, 0, 0), 3.0)\n"
        u8"    for i = 1, #hits do\n"
        u8"        p:applyImpulse(hits[i], Float3.new(0, 5000, 0))\n"
        u8"    end\n"
        u8"end\n");
}

TEST_CASE("script.scene: ScenePhysics drives an entity's character, as Sedulous's facade does")
{
    engine::physics::RegisterPhysicsScriptFacade();
    ScriptedScene bed;
    auto* characters = bed.scene.AddSystem<engine::physics::CharacterComponentManager>();
    RefPtr<ScriptClass> driver =
        MakeClassLang(u8"angelscript", u8"Driver",
                      u8"class Driver {\n"
                      u8"    private Entity@ self;\n"
                      u8"    Driver(Entity@ entity) { @self = entity; }\n"
                      u8"    void onStart() {\n"
                      u8"        ScenePhysics@ p = ScenePhysics::of(self.scene);\n"
                      u8"        p.moveCharacter(self, 5.0f, 3.0f);\n"
                      u8"        p.jumpCharacter(self, 6.0f);\n"
                      u8"        p.launchCharacter(self, 4.0f);\n"
                      u8"        p.setCharacterPosition(self, Float3(1, 2, 3));\n"
                      u8"        CharacterComponent::of(self).setPosition(Float3(1, 2, 4));\n"
                      u8"        if (!p.isCharacterGrounded(self)) { self.setName(\"airborne\"); }\n"
                      u8"    }\n"
                      u8"}\n",
                      {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(driver, u8"e");
    characters->Add(e);

    bed.Start();
    bed.Frame();
    engine::physics::CharacterComponent* character = characters->Get(e);
    REQUIRE(character != nullptr);
    CHECK(character->moveVelocity.x == doctest::Approx(5.0f));
    CHECK(character->moveVelocity.z == doctest::Approx(3.0f));
    CHECK(character->jumpSpeed == doctest::Approx(6.0f));
    CHECK(character->launchPending);
    CHECK(character->teleportPending);
    CHECK(character->teleportTo.z == doctest::Approx(4.0f)); // the component's Float3 form, last
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"airborne"));
}

// DebugDraw in its Float3 and Color forms: points by Float3, a Color, overlay, a wire box, screen
// text. On the real render service the depth-tested and the overlay draws land in their own lists.
TEST_CASE("script.scene: DebugDraw draws by Float3 and Color, over the scene with overlay, and "
          "on screen")
{
    engine::render::RegisterRenderScriptFacade();
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(), DefaultAllocator());
    engine::render::RenderSubsystem sub{DefaultAllocator(), device, 2, dataFs};
    ScriptedScene bed;
    bed.host.SetContextConfigurator(Function<void(IScriptContext&)>{
        [&sub](IScriptContext& scriptContext) { sub.ExposeToScript(scriptContext); }});

    RefPtr<ScriptClass> gizmo =
        MakeClass(u8"Gizmo",
                  u8"class Gizmo {\n"
                  u8"    private Entity@ self;\n"
                  u8"    Gizmo(Entity@ entity) { @self = entity; }\n"
                  u8"    void onStart() {\n"
                  u8"        DebugDraw@ d = DebugDraw::of(self.scene);\n"
                  u8"        Color red = Color(1, 0, 0, 1);\n"
                  u8"        d.line(Float3(0, 0, 0), Float3(1, 2, 3), red);\n"
                  u8"        d.line(Float3(0, 0, 0), Float3(4, 5, 6), red, true);\n"
                  u8"        d.wireBox(Float3(-1, -1, -1), Float3(1, 1, 1), red);\n"
                  u8"        d.wireSphere(Float3(0, 0, 0), 1.0f, red);\n"
                  u8"        d.cross(Float3(0, 0, 0), 0.5f, red, true);\n"
                  u8"        d.arrow(Float3(0, 0, 0), Float3(0, 1, 0), red, 0.2f);\n"
                  u8"        d.ray(Float3(0, 0, 0), Float3(1, 0, 0), red);\n"
                  u8"        d.text(Float3(0, 2, 0), \"here\", red);\n"
                  u8"        d.screenText(10, 20, \"score\", red, 2.0f);\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    const scene::EntityHandle e = bed.AddScripted(gizmo, u8"gizmo");
    bed.Start();
    bed.Frame();

    ScriptComponent* comp = bed.components->Get(e);
    REQUIRE(comp != nullptr);
    CHECK_FALSE(comp->behaviors[0].faulted);
    auto& drawn = sub.DebugScene(bed.scene);
    // The first depth-tested line is the Float3 one, endpoints intact; the box, sphere, arrow
    // and ray add more. The overlay line and cross went to the overlay list.
    REQUIRE(drawn.LineVertices().Size() > 2u);
    CHECK(drawn.LineVertices()[1].position.z == doctest::Approx(3.0f));
    REQUIRE(drawn.OverlayLineVertices().Size() >= 2u);
    CHECK(drawn.OverlayLineVertices()[1].position.z == doctest::Approx(6.0f));
    CHECK(drawn.TextCommands3D().Size() == 1u);
    CHECK(drawn.Commands2D().Size() >= 1u);
}

namespace
{
    [[nodiscard]] ScriptPropertyDesc Replicated(ScriptPropertyDesc desc)
    {
        desc.replicated = true;
        return desc;
    }

    [[nodiscard]] ScriptPropertyValue Number(f64 value)
    {
        ScriptPropertyValue v;
        v.kind = ScriptPropertyType::Float;
        v.number = value;
        return v;
    }

    // A host and a client scene running the same Guest behaviour, and one replication between them
    // carried by hand (the delta the host's endpoint would send). The host's copy is the one that
    // drives (`driver` 1, set as an override: local-only); `activity` is replicated, `chatty` not.
    void ReplicateBehaviourState(StringView language, StringView source, StringView otherSource)
    {
        const RefPtr<ScriptClass> guest =
            MakeClassLang(language, u8"Guest", source, {u8"onUpdate"},
                          {Replicated(FloatProperty(u8"activity", 0.0)), FloatProperty(u8"chatty", 5.0),
                           FloatProperty(u8"driver", 0.0)});
        const RefPtr<ScriptClass> other =
            MakeClassLang(language, u8"Other", otherSource, {u8"onUpdate"}, {Replicated(FloatProperty(u8"activity", 0.0))});
        foundation::net::RegisterReplicationComponents();

        ScriptedScene host;
        foundation::net::AddNetworkSceneManagers(host.scene);
        const scene::EntityHandle onHost = host.AddScripted(guest, u8"Guest");
        host.components->Get(onHost)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"driver"),
                                                               Number(1.0));
        host.Start();
        foundation::net::StateReplication rep;
        rep.AssignNetworkId(host.scene, onHost, Guid{5, 5});

        ScriptedScene client;
        foundation::net::AddNetworkSceneManagers(client.scene);
        client.Start();
        foundation::net::StateReplication crep;
        ScriptedScene* clientScene = &client;
        RefPtr<ScriptClass> spawnClass = guest;
        crep.SetSpawnHandler( // the prefab brings the same behaviour
            [clientScene, &spawnClass](scene::Scene&, const Guid&, foundation::net::NetworkId) -> scene::EntityHandle
            { return clientScene->AddScripted(spawnClass, u8"Guest"); });
        auto send = [&]()
        {
            foundation::net::BitWriter w;
            rep.CaptureDelta(host.scene, /*peer=*/1, w);
            foundation::net::BitReader r(w.Data());
            crep.ApplyDelta(client.scene, r);
        };
        auto clientValue = [&](StringView name) -> f64
        {
            const scene::EntityHandle e = client.scene.FindEntityByName(u8"Guest");
            ScriptObject* instance = client.components->Get(e)->behaviors[0].instance.Get();
            REQUIRE(instance != nullptr);
            const Result<Variant> v = instance->GetProperty(name);
            REQUIRE(v.HasValue());
            return *v.Value().TryGet<f64>();
        };

        host.Frame(); // the host's script moves activity to 1
        send();       // spawn record: the client's behaviour has no instance yet, so it is kept
        client.Frame(); // the instance is made with the host's value
        CHECK(clientValue(u8"activity") == doctest::Approx(1.0));
        host.Frame();
        host.Frame(); // 3
        send();       // the live instance is written
        CHECK(clientValue(u8"activity") == doctest::Approx(3.0));
        CHECK(clientValue(u8"chatty") == doctest::Approx(5.0)); // not replicated: the client's own

        // A different class in that slot (not the behaviour the host meant) is left alone.
        spawnClass = other;
        const scene::EntityHandle onHost2 = host.AddScripted(guest, u8"Second");
        host.components->Get(onHost2)->behaviors[0].SetOverride(ScriptPropertyNameHash(u8"driver"),
                                                                Number(1.0));
        rep.AssignNetworkId(host.scene, onHost2, Guid{6, 6});
        host.Frame();
        host.Frame();
        send();
        client.Frame();
        bool sawOther = false;
        client.components->ForEach(
            [&](ScriptComponent& c, scene::EntityHandle)
            {
                if (c.behaviors[0].script.Get() == other.Get())
                {
                    sawOther = true;
                    const ScriptPropertyOverride* over = c.behaviors[0].FindOverride(ScriptPropertyNameHash(u8"activity"));
                    CHECK(over == nullptr); // the Guest's value never reached an Other
                }
            });
        CHECK(sawOther);
    }
}

TEST_CASE("script.scene: an AngelScript behaviour's replicated property reaches the client's instance")
{
    ReplicateBehaviourState(u8"angelscript", u8"class Guest {\n"
                                             u8"    private Entity@ self;\n"
                                             u8"    float activity; float chatty; float driver;\n"
                                             u8"    Guest(Entity@ e) { @self = e; }\n"
                                             u8"    void onUpdate(double dt) { if (driver > 0) activity = activity + 1; }\n"
                                             u8"}\n",
                            u8"class Other {\n"
                                             u8"    private Entity@ self;\n"
                                             u8"    float activity;\n"
                                             u8"    Other(Entity@ e) { @self = e; }\n"
                                             u8"    void onUpdate(double dt) {}\n"
                                             u8"}\n");
}

TEST_CASE("script.scene: a Luau behaviour's replicated property reaches the client's instance")
{
    ReplicateBehaviourState(u8"luau", u8"Guest = {}\n"
                                      u8"Guest.__index = Guest\n"
                                      u8"function Guest.new(owner) local s = setmetatable({}, Guest)\n"
                                      u8"  s.self = owner; s.activity = 0; s.chatty = 0; s.driver = 0; return s end\n"
                                      u8"function Guest:onUpdate(dt) if self.driver > 0 then self.activity = self.activity + 1 end end\n",
                            u8"Other = {}\n"
                                      u8"Other.__index = Other\n"
                                      u8"function Other.new(owner) local s = setmetatable({}, Other); s.activity = 0; return s end\n"
                                      u8"function Other:onUpdate(dt) end\n");
}
