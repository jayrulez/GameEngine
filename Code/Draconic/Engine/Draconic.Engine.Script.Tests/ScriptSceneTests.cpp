// draconic.engine.script tests - the behaviors core, HEADLESS (real Wren VM, real
// Scene, zero device deps): lifecycle dispatch (deferred start, onUpdate(dt), enable/
// disable edges, onDestroy on entity destroy AND scene stop), defaults + hash-keyed
// overrides, the fault-disables-one-behavior rule, hot reload (product swap ->
// re-instantiate -> overrides re-applied, transient state reset), the entity facade
// (+ Log/Time/Random), component wire symmetry through SerializeScene, and the
// prefab-override round-trip (zero new code - the component payload carries it).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h" // DRACONIC_REFLECT_VALUE (the OPTION 1 test component)
#include <initializer_list>

import draconic.foundation;
import draconic.runtime;
import draconic.scene;
import draconic.scene.resource;
import draconic.engine.scene;
import draconic.resource;
import draconic.script;
import draconic.script.facades; // script::Entity + RegisterExtraScriptRootType/FacadeName (OPTION 1)
import draconic.script.wren;
import draconic.script.angelscript;
import draconic.script.resource;
import draconic.engine.script;
import draconic.physics;
import draconic.engine.physics;
import draconic.engine.integration; // ScriptPhysicsContactBridge (the extracted composition-root adapter)
import draconic.engine.render;      // MeshComponent/LightComponent .of (Track A, render surface)
import draconic.audio;              // AudioSourceComponent + its manager (Track A, audio surface)
import draconic.engine.audio;       // AudioSourceComponent.of + SceneAudio (Track A, audio surface)
import draconic.animation;          // animation components + managers (Track A, animation surface)
import draconic.engine.animation;   // *.of + SceneAnimation (Track A, animation surface)
import draconic.particles;          // ParticleEffectComponent + manager (Track A, particle surface)
import draconic.engine.particles;   // *.of + SceneParticles (Track A, particle surface)
import draconic.net.replication;    // NetworkComponent + manager + .of (Track A, net surface)
import draconic.engine.ui;          // world-space UI components + managers + .of (Track A, UI surface)

using namespace draconic::foundation;
using namespace draconic::script;
namespace scene = draconic::scene;
namespace physics = draconic::physics;
namespace render = draconic::render;
namespace audio = draconic::audio;
namespace anim = draconic::animation;
namespace particles = draconic::particles;
namespace ui = draconic::ui;

// ---- OPTION 1 (Fable ruling, spec Section 12): a component reached ONLY via a per-type
// `Gadget.of(entity)` factory whose DECLARED return IS the component type. Proves the whole
// script-facing pipeline: extra-emission-root -> emitted Wren class; the ReturnType-override `of`
// -> a RESOLVE Variant of the component; a property set through the re-resolving handle mutates
// the LIVE component. ----
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
DRACONIC_REFLECT_VALUE(Gadget, "draconic::script::test")
{
    builder.Property<&Gadget::power>("power");
    // The generic OPTION 1 factory: Gadget.of(entity) -> a RESOLVE handle of type Gadget.
    builder.Method<&draconic::script::ComponentOf<Gadget>, Gadget>("of");
}
namespace
{
    // Register the component once (idempotent) - AFTER the reflect macro so DraconicRegisterValue_
    // Gadget is declared. Runs its reflect builder (patches TypeOf<Gadget>), puts it in the registry
    // (both backends emit it), seeds it as an extra emission root (Wren reachability), and makes its
    // class name import-visible in Wren behavior preludes.
    void EnsureGadgetRegistered()
    {
        static const bool once = []()
        {
            DraconicRegisterValue_Gadget();
            GlobalTypeRegistry().Register(TypeOf<Gadget>());
            draconic::script::RegisterExtraScriptRootType(&TypeOf<Gadget>());
            draconic::script::RegisterExtraFacadeName(u8"Gadget");
            return true;
        }();
        (void)once;
    }
}

namespace
{
    // A cooked-product stand-in: samples/tests build ScriptClass products directly
    // (the factory test proves the cooked path; here the RUNTIME is under test).
    [[nodiscard]] RefPtr<ScriptClass>
    MakeClass(StringView className, StringView source, std::initializer_list<StringView> handlers,
              std::initializer_list<ScriptPropertyDesc> properties = {})
    {
        RefPtr<ScriptClass> cls = MakeRef<ScriptClass>(DefaultAllocator());
        cls->language = String(u8"wren");
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
        scene::Scene scene{u8"script-test"};
        ScriptRunHost host;
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
            draconic::script::wren::RegisterWrenScriptBackend();
            RegisterFoundationTypes();
            RegisterScriptComponentReflection();
            RegisterScriptFacadeReflection();
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
            scene.Update(deltaTime);
        }
    };

    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-4f; }
}

TEST_CASE("script.scene: lifecycle - onStart once (deferred to the first simulated "
          "tick), onUpdate(dt) with real dt, gated to simulation")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    construct new(entity) {\n"
                  u8"        _entity = entity\n"
                  u8"        _speed = 2.0\n"
                  u8"        _entity.setName(\"constructed\")\n"
                  u8"    }\n"
                  u8"    speed=(v) { _speed = v }\n"
                  u8"    onStart() { _entity.setName(\"started\") }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        var p = _entity.position()\n"
                  u8"        _entity.setPosition(p.x + _speed * dt, p.y, p.z)\n"
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
    bed.Frame(); // instantiate + onStart + first onUpdate, all this tick
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // 2.0 * 0.5
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    CHECK(bed.host.IsActive());
    CHECK(bed.scripts->InstanceCount() == 1u);
}

TEST_CASE("script.scene: harvested defaults apply; hash-keyed overrides win")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> mover =
        MakeClass(u8"Mover",
                  u8"class Mover {\n"
                  u8"    construct new(entity) {\n"
                  u8"        _entity = entity\n"
                  u8"        _speed = 0.0\n"
                  u8"    }\n"
                  u8"    speed=(v) { _speed = v }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        var p = _entity.position()\n"
                  u8"        _entity.setPosition(p.x + _speed * dt, p.y, p.z)\n"
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
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onEnable() { _entity.setName(_entity.name() + \"+on\") }\n"
                  u8"    onDisable() { _entity.setName(_entity.name() + \"+off\") }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        var p = _entity.position()\n"
                  u8"        _entity.setPosition(p.x + 1, p.y, p.z)\n"
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
    bed.Frame(); // onEnable again + ticks
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"t+on+off+on"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
}

TEST_CASE("script.scene: onDestroy fires on entity destroy AND on scene stop; stop "
          "releases every instance")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> counter =
        MakeClass(u8"Counter",
                  u8"var DestroyCount = 0\n"
                  u8"class Counter {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onUpdate(dt) {}\n"
                  u8"    onDestroy() { DestroyCount = DestroyCount + 1 }\n"
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
        const Variant count = bed.host.Context()->GetGlobal(u8"DestroyCount");
        REQUIRE(count.TryGet<f64>() != nullptr);
        CHECK(*count.TryGet<f64>() == 1.0);
    }

    bed.scene.Stop();
    CHECK(bed.scripts->InstanceCount() == 0u);
    {
        const Variant count = bed.host.Context()->GetGlobal(u8"DestroyCount");
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
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onUpdate(dt) { Fiber.abort(\"boom\") }\n"
                                           u8"}\n",
                                           {u8"onUpdate"});
    RefPtr<ScriptClass> steady = MakeClass(u8"Steady",
                                           u8"class Steady {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onUpdate(dt) {\n"
                                           u8"        var p = _entity.position()\n"
                                           u8"        _entity.setPosition(p.x + 1, p.y, p.z)\n"
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
                             u8"    construct new(entity) {\n"
                             u8"        _entity = entity\n"
                             u8"        _speed = 0.0\n"
                             u8"        _ticks = 0\n"
                             u8"    }\n"
                             u8"    speed=(v) { _speed = v }\n"
                             u8"    onStart() { _entity.setName(_entity.name() + \"+start\") }\n"
                             u8"    onUpdate(dt) {\n"
                             u8"        _ticks = _ticks + 1\n"
                             u8"        var p = _entity.position()\n"
                             u8"        _entity.setPosition(p.x + _speed * dt, p.y, p.z)\n"
                             u8"    }\n"
                             u8"}\n";
    // v2: same convention, DOUBLE speed effect - observable difference after reload.
    const char8_t* moverV2 = u8"class Mover {\n"
                             u8"    construct new(entity) {\n"
                             u8"        _entity = entity\n"
                             u8"        _speed = 0.0\n"
                             u8"    }\n"
                             u8"    speed=(v) { _speed = v }\n"
                             u8"    onStart() { _entity.setName(_entity.name() + \"+restart\") }\n"
                             u8"    onUpdate(dt) {\n"
                             u8"        var p = _entity.position()\n"
                             u8"        _entity.setPosition(p.x + 2 * _speed * dt, p.y, p.z)\n"
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

    bed.Frame(); // re-instantiate (fresh state, onStart again), override RE-APPLIED
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
                                           u8"    construct new(entity) {\n"
                                           u8"        _entity = entity\n"
                                           u8"        _target = null\n"
                                           u8"    }\n"
                                           u8"    target=(v) { _target = v }\n"
                                           u8"    onUpdate(dt) {\n"
                                           u8"        if (_target != null) {\n"
                                           u8"            var t = _target.position()\n"
                                           u8"            _entity.setPosition(t.x, t.y, t.z)\n"
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
        u8"    construct new(entity) {\n"
        u8"        _entity = entity\n"
        u8"        _ticks = 0\n"
        u8"    }\n"
        u8"    onUpdate(dt) {\n"
        u8"        _ticks = _ticks + 1\n"
        u8"        Log.info(\"tick at %(Time.now()) delta %(Time.delta())\")\n"
        u8"        var r = Random.range(1.0, 2.0)\n"
        u8"        if (r < 1.0 || r >= 2.0) { Fiber.abort(\"range broken\") }\n"
        u8"        // The run clock starts WITH the context - meaningful from tick 2 on.\n"
        u8"        if (_ticks > 1 && Time.delta() <= 0) { Fiber.abort(\"delta broken\") }\n"
        u8"        if (_ticks > 1 && Time.now() <= 0) { Fiber.abort(\"clock broken\") }\n"
        u8"        _entity.setPosition(Random.intRange(4, 4), 0, 0)\n"
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
    scene::Scene scene(u8"wire");
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

    scene::Scene loaded(u8"loaded");
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
    scene::Scene author(u8"author");
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

    // Level: two instances; ONE overrides the behavior's speed.
    scene::Scene level(u8"level");
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

    // Save -> load -> resolve prefabs (instances live as ref+deltas on the wire).
    MemoryStream saved;
    {
        BinarySerializer w(saved, SerializeMode::Write);
        scene::SerializeScene(w, level);
        REQUIRE(w.IsOk());
    }
    scene::Scene loaded(u8"loaded");
    auto* loadedScripts = loaded.AddSystem<ScriptComponentManager>();
    (void)saved.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer r(saved, SerializeMode::Read);
        scene::SerializeScene(r, loaded, &saved);
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
                                         u8"    construct new(entity) { _entity = entity }\n"
                                         u8"    onUpdate(dt) {}\n"
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
    ScriptRunHost host;
    CHECK(host.EnsureContext(u8"nolang") == nullptr);
    CHECK_FALSE(host.EnsureClassLoaded(*alien));
}

TEST_CASE("script.scene: entity.send invokes on<Message>(arg) on every declaring "
          "behavior of the target (P2 messaging)")
{
    ScriptedScene bed;

    // Receiver at index 0 so it is instantiated before the sender (index 1) fires its
    // onStart send this same tick - dispatch skips not-yet-live instances by design.
    RefPtr<ScriptClass> receiver =
        MakeClass(u8"Receiver",
                  u8"class Receiver {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onPing(amount) { _entity.setName(\"pinged:\" + amount.toString) }\n"
                  u8"}\n",
                  {u8"onPing"});
    RefPtr<ScriptClass> sender = MakeClass(u8"Sender",
                                           u8"class Sender {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onStart() { _entity.send(\"ping\", 7) }\n"
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
    bed.Frame(); // both instantiate; sender.onStart -> send "ping" -> receiver.onPing

    CHECK(bed.scene.GetEntityName(target) == u8"pinged:7");
}

TEST_CASE("script.scene: entity.send to a target with no matching handler is a safe "
          "no-op (P2 messaging)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> sender = MakeClass(u8"LoneSender",
                                           u8"class LoneSender {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onStart() { _entity.send(\"noHandler\", 1) }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    scene::EntityHandle e = bed.AddScripted(sender, u8"lone");
    bed.Start();
    bed.Frame(); // must not fault the sender
    ScriptComponent* c = bed.components->Get(e);
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->behaviors[0].faulted);
}

// ---- the "Roll Call" sample game's core loop, headless (mirrors SampleGame/Scripts/*.wren):
// proximity-collect orbs -> EMIT "OrbCollected" onto the scene bus -> the LEVEL scores -> win at
// the total. This is the Track B reference: the game rules live in the scene's Level tier, and
// orbs announce a NAMED event instead of find()-ing a "GameManager" entity and send()-ing it.
// No GameManager entity exists (the anti-pattern the tiers exist to kill). Score is observed
// through a stable "Scoreboard" entity's transform. The input/character path needs Input +
// Physics (exercised on Play), so it is out of this bed by design. ----

TEST_CASE("script.scene: the Roll Call sample game - orbs EMIT OrbCollected, the Level scores "
          "and wins, with NO GameManager entity (Track B reference)")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    // The Level (mirror of Level.wren): the scene's rules tier, total 2 for a short round. Score
    // lives in per-instance fields; onOrbCollected mirrors it onto the Scoreboard transform so the
    // test can observe it. It receives events by declaring on<Event> - no wiring, no manager entity.
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    construct new(scene) {\n"
                  u8"        _scene = scene\n"
                  u8"        _total = 2\n"
                  u8"        _collected = 0\n"
                  u8"        _won = false\n"
                  u8"    }\n"
                  u8"    onStart() {\n"
                  u8"        _collected = 0\n"
                  u8"        _won = false\n"
                  u8"    }\n"
                  u8"    onOrbCollected() {\n"
                  u8"        if (_won) {\n"
                  u8"            return\n"
                  u8"        }\n"
                  u8"        _collected = _collected + 1\n"
                  u8"        var won = 0\n"
                  u8"        if (_collected >= _total) {\n"
                  u8"            _won = true\n"
                  u8"            won = 1\n"
                  u8"        }\n"
                  u8"        _scene.find(\"Scoreboard\").setPosition(_collected, won, 0)\n"
                  u8"    }\n"
                  u8"    onPlayerFell() { _won = true }\n"
                  u8"}\n",
                  {u8"onStart", u8"onOrbCollected", u8"onPlayerFell"});
    level->Settings().script = levelClass;

    // Pickup (mirror of Pickup.wren): proximity to "Player" -> emit "OrbCollected", remove self.
    RefPtr<ScriptClass> pickup =
        MakeClass(u8"Pickup",
                  u8"class Pickup {\n"
                  u8"    construct new(entity) {\n"
                  u8"        _entity = entity\n"
                  u8"        _r = 1.2\n"
                  u8"        _taken = false\n"
                  u8"    }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        if (_taken) {\n"
                  u8"            return\n"
                  u8"        }\n"
                  u8"        var player = _entity.scene.find(\"Player\")\n"
                  u8"        if (!player.isValid()) {\n"
                  u8"            return\n"
                  u8"        }\n"
                  u8"        var p = player.position()\n"
                  u8"        var me = _entity.position()\n"
                  u8"        var dx = p.x - me.x\n"
                  u8"        var dy = p.y - me.y\n"
                  u8"        var dz = p.z - me.z\n"
                  u8"        if (dx*dx + dy*dy + dz*dz > _r*_r) {\n"
                  u8"            return\n"
                  u8"        }\n"
                  u8"        _taken = true\n"
                  u8"        _entity.scene.events.emit(\"OrbCollected\")\n"
                  u8"        _entity.destroy()\n"
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
    bed.Frame(); // instantiate + onStart; player far -> nothing collected
    CHECK(bed.scripts->InstanceCount() == 2u); // 2 orbs (the Level is not an entity behavior)
    CHECK(level->LevelActive());               // the rules tier is live, no GameManager entity
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 0.0f));

    // Walk onto orb1: it collects itself and EMITS "OrbCollected"; the Level scores this SAME frame
    // (emit publishes, the scene tick drains the bus at its top level -> onOrbCollected).
    bed.scene.SetLocalPosition(player, Float3{0.0f, 0.0f, 0.0f});
    bed.Frame(); // orb1 onUpdate: proximity hit -> emit("OrbCollected") + destroy -> Level scores
    CHECK_FALSE(bed.scene.IsValid(orb1));
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 1.0f));
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.y, 0.0f)); // not won yet

    // Walk onto orb2: collecting the last orb wins.
    bed.scene.SetLocalPosition(player, Float3{5.0f, 0.0f, 0.0f});
    bed.Frame();
    CHECK_FALSE(bed.scene.IsValid(orb2));
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.x, 2.0f)); // both collected
    CHECK(Near(bed.scene.GetLocalTransform(scoreboard).position.y, 1.0f)); // win branch ran
}

TEST_CASE("script.scene: updateInterval throttles onUpdate and delivers the accumulated "
          "dt (P3)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ticker = MakeClass(
        u8"Ticker",
        u8"class Ticker {\n"
        u8"    construct new(entity) { _entity = entity }\n"
        u8"    onUpdate(dt) {\n"
        u8"        var p = _entity.position()\n"
        u8"        _entity.setPosition(p.x + 1.0, dt, p.z)\n" // x counts calls; y = last dt
        u8"    }\n"
        u8"}\n",
        {u8"onUpdate"});
    const scene::EntityHandle e = bed.AddScripted(ticker, u8"ticker");
    bed.components->Get(e)->behaviors[0].updateInterval = 1.0f;

    bed.Start();
    bed.Frame(0.5f); // acc 0.5 < 1.0 -> no update
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // acc 1.0 -> ONE update, dt = accumulated 1.0
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.y, 1.0f)); // accumulated dt, not 0.5
    bed.Frame(0.5f);                                              // acc 0.5 -> no update
    bed.Frame(0.5f);                                              // acc 1.0 -> second update
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f)); // 2 calls over 4 ticks
}

TEST_CASE("script.scene: updateInterval survives the SerializeScene wire (P3 symmetry)")
{
    RegisterScriptComponentReflection();
    scene::Scene scene(u8"interval-wire");
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

    scene::Scene loaded(u8"loaded");
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

TEST_CASE("script.scene: Scene.spawn routes through the run spawner to the current scene "
          "and returns a live Entity (P2)")
{
    ScriptedScene bed;

    // A fake prefab spawner: creates a plain entity at the requested position (a real run
    // resolves + spawns a cooked prefab payload; the facade->binding->scene path is what
    // is under test here). Records the last call.
    int spawnCalls = 0;
    Guid lastPrefab;
    scene::EntityHandle spawnedHandle;
    bed.host.Binding().spawnPrefab =
        Function<scene::EntityHandle(scene::Scene*, const Guid&, const Float3&)>{
            [&](scene::Scene* scene, const Guid& prefabId,
                const Float3& position) -> scene::EntityHandle
            {
                ++spawnCalls;
                lastPrefab = prefabId;
                spawnedHandle = scene->CreateEntity(u8"spawned");
                scene->SetLocalPosition(spawnedHandle, position);
                return spawnedHandle;
            }};

    // The behavior spawns on start using a prefab id delivered as an asset property.
    RefPtr<ScriptClass> spawner =
        MakeClass(u8"Spawner",
                  u8"class Spawner {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    prefab=(v) { _prefab = v }\n"
                  u8"    onStart() {\n"
                  u8"        var e = _entity.scene.spawn(_prefab, 3.0, 4.0, 5.0)\n"
                  u8"        e.setName(\"child\")\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    ScriptPropertyDesc prefabProp;
    prefabProp.name = String(u8"prefab");
    prefabProp.hash = ScriptPropertyNameHash(u8"prefab");
    prefabProp.type = ScriptPropertyType::Asset;
    prefabProp.assetType = String(u8"Prefab");
    prefabProp.defaultValue.kind = ScriptPropertyType::Asset;
    prefabProp.defaultValue.guid = Guid{0xABC, 0xDEF};
    spawner->properties.PushBack(prefabProp);

    const scene::EntityHandle e = bed.AddScripted(spawner, u8"spawner");
    (void)e;
    bed.Start();
    bed.Frame();

    CHECK(spawnCalls == 1);
    CHECK(lastPrefab == Guid{0xABC, 0xDEF});
    // Scene.spawn returned the live Entity: the script renamed it and it sits at the
    // requested world position.
    REQUIRE(spawnedHandle.IsAssigned());
    CHECK(bed.scene.GetEntityName(spawnedHandle) == StringView(u8"child"));
    CHECK(Near(bed.scene.GetLocalTransform(spawnedHandle).position.x, 3.0f));
    CHECK(Near(bed.scene.GetLocalTransform(spawnedHandle).position.z, 5.0f));
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
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var t = _entity.scene.find(\"Target\")\n"
                  u8"        if (t.isValid()) { t.setName(\"found-by-name\") }\n"
                  u8"        var w = _entity.scene.findByPath(\"Player/Weapon\")\n"
                  u8"        if (w.isValid()) { w.setName(\"found-by-path\") }\n"
                  u8"        var missing = _entity.scene.find(\"Nope\")\n"
                  u8"        if (!missing.isValid()) { _entity.setName(\"miss-ok\") }\n"
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

// ---- second backend, uniformly (scripting.md §7.5): a .as behavior runs the SAME neutral
// runtime path as Wren - the RunHost resolves the backend by the class's language, assembles
// the module through the backend (AngelScript needs no prelude), instantiates, and dispatches
// lifecycle. Property harvest is deferred for AS, so this behavior carries no properties.

TEST_CASE("script.scene: an AngelScript behavior runs the neutral lifecycle path "
          "(onStart + onUpdate(dt)) - proves both backends, uniformly")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
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
    bed.Frame(); // instantiate + onStart + first onUpdate (dt 0.5): x = 1.0
    CHECK(bed.host.Language() == StringView(u8"angelscript"));
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"started"));
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f)); // 2.0 * 0.5
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 2.0f));
    ScriptComponent* c = bed.components->Get(e);
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->behaviors[0].faulted);
}

// ---- the step debugger through the subsystem (script-debugger.md P1): a breakpoint in a
// behavior handler PAUSES the game (the world holds still - behaviors stop advancing) while
// the debugger owns the suspended handler; Continue runs it to completion and ticking resumes.
// The suspension is NOT a fault. Proves the game-pause wiring + the debug-suspend handling
// end-to-end through the real ScriptSceneSystem, headless.

TEST_CASE("script.scene: a breakpoint in a behavior handler pauses the game and resumes clean")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    // Line-numbered so the breakpoint below lands inside onUpdate BEFORE x is incremented.
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
    // The cook stamps sourceName = the asset file; the runtime loads this class in its OWN
    // section named by it, so a breakpoint keyed on the SOURCE FILE (the editor's key) lines up.
    breaker->sourceName = String(u8"Breaker.as");

    bed.host.RequestDebugger(Function<void(IScriptDebugger&)>{}); // debuggable run

    const scene::EntityHandle e = bed.AddScripted(breaker, u8"walker");
    bed.Start();
    bed.Frame(); // instantiate + onUpdate (no breakpoint yet): x = 1
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));

    // Set the breakpoint on the SOURCE FILE (the editor's breakpoint key) at the increment
    // line, now that the debugger + module exist - it must line up with the class's section.
    IScriptDebugger* debugger = bed.host.Debugger();
    REQUIRE(debugger != nullptr);
    CHECK_FALSE(bed.host.IsDebugPaused());
    debugger->SetBreakpoint(u8"Breaker.as", 6);

    // Next tick: onUpdate hits the breakpoint and SUSPENDS before the increment -> the game
    // is paused, the world holds still (position frozen at 1), and the behavior is NOT faulted.
    bed.Frame();
    CHECK(bed.host.IsDebugPaused());
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 1.0f));
    CHECK_FALSE(bed.components->Get(e)->behaviors[0].faulted);

    // The suspended context is inspectable: the innermost frame is on the break line, and
    // onUpdate's `dt` local is captured.
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 6);
    CHECK(StringView(frames[0].file) == u8"Breaker.as"); // the section IS the source file
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

// A harvested AngelScript editor property (a member field) reaches the instance through the
// neutral setter-Invoke path: the subsystem Invokes `<name>=`, which the AngelScript backend
// writes to the same-named member field. Both the default and a hash-keyed override drive it.

TEST_CASE("script.scene: an AngelScript harvested float property applies (default + override) "
          "and drives onUpdate")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();

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

// ---- coroutines (scripting.md §3.3): the Wren `Behavior` base + host scheduler wired
// through the subsystem tick (AdvanceCoroutines once per frame) + cancel on disable/destroy.

TEST_CASE("script.scene: a coroutine wait(1.0) runs its body only after ~1s of ticks")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> waiter = MakeClass(u8"Waiter",
                                           u8"class Waiter is Behavior {\n"
                                           u8"    construct new(entity) { super(entity) }\n"
                                           u8"    onStart() {\n"
                                           u8"        var me = this\n"
                                           u8"        startCoroutine(Fn.new {\n"
                                           u8"            me.wait(1.0)\n"
                                           u8"            me.finish()\n"
                                           u8"        })\n"
                                           u8"    }\n"
                                           u8"    finish() { entity.setPosition(5, 0, 0) }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    waiter->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(waiter, u8"w");
    bed.Start();
    bed.Frame(0.5f); // onStart registers wait 1.0; +0.5s -> still pending
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // +0.5s -> 1.0s reached -> resume runs finish()
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
    bed.Frame(0.5f); // completed coroutine does not run again
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 5.0f));
}

TEST_CASE("script.scene: a coroutine waitUntil resumes when the predicate flips")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> gater = MakeClass(u8"Gater",
                                          u8"class Gater is Behavior {\n"
                                          u8"    construct new(entity) {\n"
                                          u8"        super(entity)\n"
                                          u8"        _open = false\n"
                                          u8"        _ticks = 0\n"
                                          u8"    }\n"
                                          u8"    onStart() {\n"
                                          u8"        var me = this\n"
                                          u8"        startCoroutine(Fn.new {\n"
                                          u8"            me.waitUntil(Fn.new { me.isOpen })\n"
                                          u8"            me.finish()\n"
                                          u8"        })\n"
                                          u8"    }\n"
                                          u8"    onUpdate(dt) {\n"
                                          u8"        _ticks = _ticks + 1\n"
                                          u8"        if (_ticks >= 3) { _open = true }\n"
                                          u8"    }\n"
                                          u8"    isOpen { _open }\n"
                                          u8"    finish() { entity.setPosition(9, 0, 0) }\n"
                                          u8"}\n",
                                          {u8"onStart", u8"onUpdate"});
    gater->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(gater, u8"g");
    bed.Start();
    bed.Frame(0.5f); // tick 1, gate closed
    bed.Frame(0.5f); // tick 2, gate closed
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 0.0f));
    bed.Frame(0.5f); // tick 3 opens the gate -> waitUntil resumes -> finish()
    CHECK(Near(bed.scene.GetLocalTransform(e).position.x, 9.0f));
}

TEST_CASE("script.scene: destroying a behavior cancels its pending coroutine (never fires)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ghost = MakeClass(u8"Ghost",
                                          u8"var GhostFired = 0\n"
                                          u8"class Ghost is Behavior {\n"
                                          u8"    construct new(entity) { super(entity) }\n"
                                          u8"    onStart() {\n"
                                          u8"        var me = this\n"
                                          u8"        startCoroutine(Fn.new {\n"
                                          u8"            me.wait(1.0)\n"
                                          u8"            me.fire()\n"
                                          u8"        })\n"
                                          u8"    }\n"
                                          u8"    fire() { GhostFired = GhostFired + 1 }\n"
                                          u8"}\n",
                                          {u8"onStart"});
    ghost->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(ghost, u8"ghost");
    bed.Start();
    bed.Frame(0.5f);            // registers wait 1.0; +0.5s pending
    bed.scene.DestroyEntity(e); // onDestroy path cancels the coroutine
    bed.Frame(0.5f);
    bed.Frame(0.5f);
    bed.Frame(0.5f); // well past 1.0s - a cancelled coroutine must not run

    const Variant fired = bed.host.Context()->GetGlobal(u8"GhostFired");
    REQUIRE(fired.TryGet<f64>() != nullptr);
    CHECK(*fired.TryGet<f64>() == 0.0);
}

TEST_CASE("script.scene: disabling a behavior cancels its pending coroutine (never fires)")
{
    ScriptedScene bed;
    RefPtr<ScriptClass> ghost = MakeClass(u8"Sleeper",
                                          u8"var SleeperFired = 0\n"
                                          u8"class Sleeper is Behavior {\n"
                                          u8"    construct new(entity) { super(entity) }\n"
                                          u8"    onStart() {\n"
                                          u8"        var me = this\n"
                                          u8"        startCoroutine(Fn.new {\n"
                                          u8"            me.wait(1.0)\n"
                                          u8"            me.fire()\n"
                                          u8"        })\n"
                                          u8"    }\n"
                                          u8"    onUpdate(dt) {}\n"
                                          u8"    fire() { SleeperFired = SleeperFired + 1 }\n"
                                          u8"}\n",
                                          {u8"onStart", u8"onUpdate"});
    ghost->usesCoroutines = true;

    const scene::EntityHandle e = bed.AddScripted(ghost, u8"s");
    bed.Start();
    bed.Frame(0.5f); // registers wait 1.0; +0.5s pending
    bed.components->Get(e)->behaviors[0].enabled = false;
    bed.Frame(0.5f); // onDisable edge cancels the coroutine
    bed.Frame(0.5f);
    bed.Frame(0.5f); // past 1.0s - must not fire

    const Variant fired = bed.host.Context()->GetGlobal(u8"SleeperFired");
    REQUIRE(fired.TryGet<f64>() != nullptr);
    CHECK(*fired.TryGet<f64>() == 0.0);
}

// ---- physics contact events -> behaviors (end-to-end, real Jolt + real Scene) ----
// A full runtime Context wiring SceneSubsystem + PhysicsSubsystem + ScriptSubsystem: the
// physics tick resolves contacts to entities and pushes them to the script subsystem
// (registered as an IContactListener at OnReady), which enqueues them onto the owning
// scene's deferred queue; the scene tick drains and dispatches the on<Event> handler.

namespace
{
    namespace runtime = draconic::runtime;

    // Builds a Context with all three subsystems started + a live scene, returns the scene.
    // Acts as its OWN composition root: bridges physics contacts to the script subsystem's
    // neutral DeliverContact ingress with the SAME adapter DefaultApplication uses in a real
    // run (draconic.engine.integration) - so these contact tests also exercise that bridge, and
    // the script subsystem itself keeps no physics dependency.
    struct ContactWorld
    {
        runtime::Context ctx;
        scene::SceneSubsystem* scenes = nullptr;
        physics::PhysicsSubsystem* physics = nullptr;
        ScriptSubsystem* scripts = nullptr;
        scene::Scene* scene = nullptr;
        UniquePtr<scene::SceneManager> sm; // this bed's scene group (subsystem owns none)
        draconic::integration::ScriptPhysicsContactBridge bridge; // the composition-root adapter

        ContactWorld()
        {
            draconic::script::wren::RegisterWrenScriptBackend();
            RegisterFoundationTypes();
            physics::RegisterPhysicsComponentReflection();
            RegisterScriptComponentReflection();
            RegisterScriptFacadeReflection();
            scenes = ctx.AddSubsystem<scene::SceneSubsystem>();
            physics = ctx.AddSubsystem<physics::PhysicsSubsystem>();
            scripts = ctx.AddSubsystem<ScriptSubsystem>();
            ctx.Startup();
            bridge.Install(*physics, *scripts); // the composition-root bridge (the real adapter)
            sm = MakeUnique<scene::SceneManager>(DefaultAllocator(), &scenes->AwareRegistry());
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
            auto& body = scene->GetSystem<physics::RigidBodyComponentManager>()->Add(e);
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
                ctx.BeginFrame(1.0f / 60.0f); // fixed steps: physics contacts -> script enqueue
                ctx.Update(1.0f / 60.0f);     // scene tick: drain -> behavior dispatch
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

    // Records the OTHER entity's name only if speed is non-negative and the normal is unit-ish
    // - proving all four args crossed the boundary intact.
    RefPtr<ScriptClass> bumper = MakeClass(
        u8"Bumper",
        u8"class Bumper {\n"
        u8"    construct new(entity) { _entity = entity }\n"
        u8"    onContactBegin(other, point, normal, speed) {\n"
        u8"        var len = normal.x*normal.x + normal.y*normal.y + normal.z*normal.z\n"
        u8"        if (speed >= 0 && len > 0.5) { _entity.setName(\"hit:\" + other.name()) }\n"
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
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onTriggerEnter(other) { _entity.setName(\"sensed:\" + other.name()) }\n"
                  u8"}\n",
                  {u8"onTriggerEnter"});
    world.Attach(volume, sensor);

    world.Play(240);

    CHECK(world.scene->GetEntityName(volume) == StringView(u8"sensed:faller"));
}

TEST_CASE("script.scene: behaviors tick without error when no physics subsystem is present "
          "(contact-listener registration is guarded)")
{
    namespace runtime = draconic::runtime;
    runtime::Context ctx;
    auto* scenes = ctx.AddSubsystem<scene::SceneSubsystem>();
    scene::SceneManager sm(&scenes->AwareRegistry());
    scenes->RegisterManager(&sm);
    ctx.AddSubsystem<ScriptSubsystem>(); // NO physics subsystem
    draconic::script::wren::RegisterWrenScriptBackend();
    RegisterFoundationTypes();
    RegisterScriptComponentReflection();
    RegisterScriptFacadeReflection();
    ctx.Startup(); // OnReady must not crash resolving the (absent) physics subsystem

    scene::Scene* scene = sm.CreateScene(u8"no-physics");
    RefPtr<ScriptClass> mover = MakeClass(u8"Mover",
                                          u8"class Mover {\n"
                                          u8"    construct new(entity) { _entity = entity }\n"
                                          u8"    onUpdate(dt) {\n"
                                          u8"        var p = _entity.position()\n"
                                          u8"        _entity.setPosition(p.x + 1.0, p.y, p.z)\n"
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

// ---- Scene-facade safety regression battery (Fable option B). The bound Scene carries its OWN
//      scene ptr, so there is no ambient "current scene" to misroute: every call site - update,
//      onDestroy, a resumed coroutine, cross-scene - targets the right scene by construction. These
//      pin the FORMER footgun paths (the ambient model read a stale/null currentScene from them). ----

TEST_CASE("script-facade: entity.scene() is bound to the entity's OWN scene (cross-scene isolation)")
{
    // Pure facade test - no VM. Two scenes; an entity of A resolves + finds in A, never B.
    scene::Scene a(u8"A");
    scene::Scene b(u8"B");
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
    int spawnCalls = 0;
    scene::Scene* spawnedScene = nullptr;
    scene::EntityHandle spawned;
    bed.host.Binding().spawnPrefab =
        Function<scene::EntityHandle(scene::Scene*, const Guid&, const Float3&)>{
            [&](scene::Scene* scene, const Guid&, const Float3& pos) -> scene::EntityHandle
            {
                ++spawnCalls;
                spawnedScene = scene;
                spawned = scene->CreateEntity(u8"death-spawn");
                scene->SetLocalPosition(spawned, pos);
                return spawned;
            }};

    RefPtr<ScriptClass> dier =
        MakeClass(u8"Dier",
                  u8"class Dier {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    prefab=(v) { _prefab = v }\n"
                  u8"    onDestroy() { _entity.scene.spawn(_prefab, 1.0, 2.0, 3.0) }\n"
                  u8"}\n",
                  {u8"onDestroy"});
    ScriptPropertyDesc prefabProp;
    prefabProp.name = String(u8"prefab");
    prefabProp.hash = ScriptPropertyNameHash(u8"prefab");
    prefabProp.type = ScriptPropertyType::Asset;
    prefabProp.assetType = String(u8"Prefab");
    prefabProp.defaultValue.kind = ScriptPropertyType::Asset;
    prefabProp.defaultValue.guid = Guid{0x11, 0x22};
    dier->properties.PushBack(prefabProp);

    const scene::EntityHandle e = bed.AddScripted(dier, u8"dier");
    bed.Start();
    bed.Frame(); // instantiate + (no onStart handler)
    CHECK(spawnCalls == 0);

    bed.scene.DestroyEntity(e);
    bed.Frame(); // removal detected -> onDestroy -> spawn into the entity's own scene
    CHECK(spawnCalls == 1);
    CHECK(spawnedScene == &bed.scene); // the entity's scene, resolved from the bound handle
    REQUIRE(spawned.IsAssigned());
    CHECK(bed.scene.GetEntityName(spawned) == StringView(u8"death-spawn"));
}

TEST_CASE("script.scene: entity.scene.spawn works from a resumed coroutine - a FORMER footgun")
{
    ScriptedScene bed;
    int spawnCalls = 0;
    scene::Scene* spawnedScene = nullptr;
    bed.host.Binding().spawnPrefab =
        Function<scene::EntityHandle(scene::Scene*, const Guid&, const Float3&)>{
            [&](scene::Scene* scene, const Guid&, const Float3&) -> scene::EntityHandle
            {
                ++spawnCalls;
                spawnedScene = scene;
                return scene->CreateEntity(u8"coro-spawn");
            }};

    RefPtr<ScriptClass> spawner =
        MakeClass(u8"CoroSpawner",
                  u8"class CoroSpawner is Behavior {\n"
                  u8"    construct new(entity) { super(entity) }\n"
                  u8"    prefab=(v) { _prefab = v }\n"
                  u8"    onStart() {\n"
                  u8"        var me = this\n"
                  u8"        startCoroutine(Fn.new {\n"
                  u8"            me.wait(1.0)\n"
                  u8"            me.doSpawn()\n"
                  u8"        })\n"
                  u8"    }\n"
                  u8"    doSpawn() { entity.scene.spawn(_prefab, 7, 8, 9) }\n"
                  u8"}\n",
                  {u8"onStart"});
    spawner->usesCoroutines = true;
    ScriptPropertyDesc prefabProp;
    prefabProp.name = String(u8"prefab");
    prefabProp.hash = ScriptPropertyNameHash(u8"prefab");
    prefabProp.type = ScriptPropertyType::Asset;
    prefabProp.assetType = String(u8"Prefab");
    prefabProp.defaultValue.kind = ScriptPropertyType::Asset;
    prefabProp.defaultValue.guid = Guid{0x33, 0x44};
    spawner->properties.PushBack(prefabProp);

    (void)bed.AddScripted(spawner, u8"cs");
    bed.Start();
    bed.Frame(0.5f); // registers wait(1.0); +0.5s -> pending, no spawn
    CHECK(spawnCalls == 0);
    bed.Frame(0.5f); // +0.5s -> 1.0s reached -> coroutine resumes -> spawn
    CHECK(spawnCalls == 1);
    CHECK(spawnedScene == &bed.scene); // resumed OUTSIDE any tick swap, still the right scene
}

TEST_CASE("script.scene: an AngelScript behavior spawns through self.scene() (bound Scene, 2nd backend)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    int spawnCalls = 0;
    scene::Scene* spawnedScene = nullptr;
    bed.host.Binding().spawnPrefab =
        Function<scene::EntityHandle(scene::Scene*, const Guid&, const Float3&)>{
            [&](scene::Scene* scene, const Guid&, const Float3&) -> scene::EntityHandle
            {
                ++spawnCalls;
                spawnedScene = scene;
                return scene->CreateEntity(u8"as-spawn");
            }};

    RefPtr<ScriptClass> spawner = MakeClassLang(
        u8"angelscript", u8"Spawner",
        u8"class Spawner {\n"
        u8"    private Entity@ self;\n"
        u8"    Spawner(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() { self.scene.spawn(Guid(0x55, 0x66), 1.0f, 0.0f, 0.0f); }\n"
        u8"}\n",
        {u8"onStart"});

    (void)bed.AddScripted(spawner, u8"as");
    bed.Start();
    bed.Frame();
    CHECK(spawnCalls == 1);
    CHECK(spawnedScene == &bed.scene); // the bound Scene value carried the entity's scene through AS
}

// ---- Scene-level scripting (the third tier): one `Level` object per scene, constructed with the
//      scene's BOUND handle, sim-gated, dispatched onStart/onUpdate(dt)/onFixedUpdate(dt)/onStop. ----

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
                  u8"    construct new(scene) { _scene = scene }\n"
                  u8"    onStart() { _scene.find(\"flag\").setName(\"started\") }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        var e = _scene.find(\"ticker\")\n"
                  u8"        var p = e.position()\n"
                  u8"        e.setPosition(p.x + 1, 0, 0)\n"
                  u8"    }\n"
                  u8"    onStop() { _scene.find(\"started\").setName(\"stopped\") }\n"
                  u8"}\n",
                  {u8"onStart", u8"onUpdate", u8"onStop"});
    level->Settings().script = levelClass;

    bed.Start(); // OnSceneStarted -> construct new(scene) + onStart
    CHECK(level->LevelActive());
    CHECK(bed.scene.GetEntityName(flag) == StringView(u8"started")); // onStart ran via bound scene

    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 1.0f));
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 2.0f));

    bed.scene.SetSimulationEnabled(false);
    bed.Frame(); // sim-gated -> no onUpdate
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 2.0f));
    bed.scene.SetSimulationEnabled(true);
    bed.Frame();
    CHECK(Near(bed.scene.GetLocalTransform(ticker).position.x, 3.0f));

    bed.scene.Stop(); // OnSceneStopped -> onStop + destroy
    CHECK(bed.scene.GetEntityName(flag) == StringView(u8"stopped"));
    CHECK_FALSE(level->LevelActive());
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
                  u8"    construct new(scene) { _scene = scene }\n"
                  u8"    onFixedUpdate(dt) {\n"
                  u8"        var e = _scene.find(\"f\")\n"
                  u8"        e.setPosition(e.position().x + 1, 0, 0)\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onFixedUpdate"});
    level->Settings().script = levelClass;

    bed.Start();
    bed.scene.FixedUpdate(1.0f / 60.0f); // one fixed step -> onFixedUpdate
    CHECK(Near(bed.scene.GetLocalTransform(f).position.x, 1.0f));
    bed.scene.FixedUpdate(1.0f / 60.0f);
    CHECK(Near(bed.scene.GetLocalTransform(f).position.x, 2.0f));

    bed.scene.SetSimulationEnabled(false);
    bed.scene.FixedUpdate(1.0f / 60.0f); // sim-gated
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
                  u8"    construct new(scene) { _scene = scene }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        _scene.find(\"t\").setName(\"ticked\")\n"
                  u8"        Fiber.abort(\"boom\")\n"
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
    bed.Frame(); // faulted -> no further dispatch
    CHECK(bed.scene.GetEntityName(t) == StringView(u8"reset"));
}

TEST_CASE("script.scene: two scenes sharing one Level class get INDEPENDENT objects (additive)")
{
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    construct new(scene) { _scene = scene }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        var e = _scene.find(\"ticker\")\n"
                  u8"        e.setPosition(e.position().x + 1, 0, 0)\n"
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
    b.Frame(); // B ticks once - each Level targets ITS OWN scene
    CHECK(Near(a.scene.GetLocalTransform(ta).position.x, 2.0f));
    CHECK(Near(b.scene.GetLocalTransform(tb).position.x, 1.0f));
}

TEST_CASE("script.scene: an AngelScript Level runs the tier (2nd backend)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
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

// ---- OPTION 1 end-to-end: Gadget.of(_entity).power = 5 mutates the LIVE component (Wren). Proves
// the extra-emission-root (Gadget emitted though no signature reaches it), the ReturnType-override
// factory, and property mutation through the re-resolving handle - all together. ----
TEST_CASE("script.scene: OPTION 1 - Component.of(entity).field mutates the live component through a "
          "re-resolving handle (Wren)")
{
    EnsureGadgetRegistered(); // before any VM context is built

    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();

    RefPtr<ScriptClass> setter = MakeClass(u8"Setter",
                                           u8"class Setter {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onStart() {\n"
                                           u8"        var g = Gadget.of(_entity)\n"
                                           u8"        g.power = 5.0\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(setter, u8"e");
    gadgets->Add(e).power = 1.0f;

    bed.Start();
    bed.Frame(); // onStart: Gadget.of(_entity).power = 5.0
    REQUIRE(gadgets->Get(e) != nullptr);
    CHECK(gadgets->Get(e)->power == doctest::Approx(5.0f)); // the LIVE component was mutated
}

// OPTION 1 on the SECOND backend: AngelScript is statically typed, so the ReturnType-override is
// what makes `Gadget::of(self)` box as a Gadget handle (its declared return). Proves cross-backend
// parity of the whole entity.get shape.
TEST_CASE("script.scene: OPTION 1 - Component::of(entity).field mutates the live component "
          "(AngelScript) - cross-backend parity")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
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
    CHECK(gadgets->Get(e)->power == doctest::Approx(5.0f)); // the LIVE component was mutated
}

// OPTION 1 on a REAL engine component: RigidBodyComponent.of(entity).friction, on a live physics
// scene. Same machinery as the Gadget proof, now wired to an actual shipped component.
TEST_CASE("script.scene: OPTION 1 - RigidBodyComponent.of(entity).friction mutates the live "
          "physics component (Wren)")
{
    physics::RegisterPhysicsScriptFacade(); // register components as script classes + of() (idempotent)

    ContactWorld world;
    const scene::EntityHandle e = world.AddBody(u8"e", Float3{0, 2, 0}, physics::MotionKind::Dynamic,
                                                Float3{0.5f, 0.5f, 0.5f});
    auto* bodies = world.scene->GetSystem<physics::RigidBodyComponentManager>();
    REQUIRE(bodies->Get(e) != nullptr);
    bodies->Get(e)->friction = 0.1f;

    RefPtr<ScriptClass> setter = MakeClass(u8"Setter",
                                           u8"class Setter {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onStart() {\n"
                                           u8"        var b = RigidBodyComponent.of(_entity)\n"
                                           u8"        b.friction = 0.5\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    world.Attach(e, setter);

    world.Play(2); // onStart: RigidBodyComponent.of(_entity).friction = 0.5
    REQUIRE(bodies->Get(e) != nullptr);
    CHECK(bodies->Get(e)->friction == doctest::Approx(0.5f)); // the LIVE component was mutated
}

// scene.physics via the OPTION 1 factory ScenePhysics.of(scene): reflected physics ops act on THIS
// scene's world. Also the reflected-path parity - gravityY reads the same world the static Physics
// facade reads (both delegate to PhysicsSceneSystem->World()->Gravity()), verified against ground truth.
TEST_CASE("script.scene: ScenePhysics.of(scene) reads + writes gravity on THIS scene's world (Wren)")
{
    physics::RegisterPhysicsScriptFacade();
    ContactWorld world; // the world is created on scene Start (inside Play), not before

    const scene::EntityHandle e = world.scene->CreateEntity(u8"driver");
    RefPtr<ScriptClass> driver =
        MakeClass(u8"Driver",
                  u8"class Driver {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var p = ScenePhysics.of(_entity.scene)\n"
                  u8"        p.setGravity(0, -20, 0)\n"                  // write THIS scene's world
                  u8"        _entity.setPosition(p.gravityY(), 0, 0)\n" // read it back: x = -20
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});
    world.Attach(e, driver);

    world.Play(2); // onStart: ScenePhysics.of(scene).setGravity + gravityY
    auto* sys = world.scene->GetSystem<physics::PhysicsSceneSystem>();
    REQUIRE(sys != nullptr);
    REQUIRE(sys->World() != nullptr);
    CHECK(world.scene->GetLocalTransform(e).position.x == doctest::Approx(-20.0f)); // gravityY read
    CHECK(sys->World()->Gravity().y == doctest::Approx(-20.0f)); // setGravity hit the REAL scene world
}

// scene.physics on the SECOND backend - ScenePhysics::of returns the concrete handle type (its
// declared return), so AngelScript boxes it right. Cross-backend parity for the scene-system facade.
TEST_CASE("script.scene: ScenePhysics.of(scene) reads + writes gravity on THIS scene's world "
          "(AngelScript) - cross-backend parity")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    physics::RegisterPhysicsScriptFacade();
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
    auto* sys = world.scene->GetSystem<physics::PhysicsSceneSystem>();
    REQUIRE(sys != nullptr);
    REQUIRE(sys->World() != nullptr);
    CHECK(world.scene->GetLocalTransform(e).position.x == doctest::Approx(-20.0f)); // gravityY read
    CHECK(sys->World()->Gravity().y == doctest::Approx(-20.0f)); // setGravity hit the real world
}

// Fuller physics surface: per-entity character control via CharacterComponent.of(entity).move/jump.
// These are component-DATA ops (the tick reads moveVelocity/jumpSpeed), so they work through the
// re-resolving handle with no world access - and per ENTITY, fixing the static facade's "first
// character only" limitation.
TEST_CASE("script.scene: CharacterComponent.of(entity).move/jump - per-entity control (Wren)")
{
    physics::RegisterPhysicsScriptFacade();
    ScriptedScene bed;
    auto* characters = bed.scene.AddSystem<physics::CharacterComponentManager>();

    RefPtr<ScriptClass> driver = MakeClass(u8"Driver",
                                           u8"class Driver {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onStart() {\n"
                                           u8"        var c = CharacterComponent.of(_entity)\n"
                                           u8"        c.move(5, 3)\n"
                                           u8"        c.jump(6)\n"
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
    draconic::script::angelscript::RegisterAngelScriptBackend();
    physics::RegisterPhysicsScriptFacade();
    ScriptedScene bed;
    auto* characters = bed.scene.AddSystem<physics::CharacterComponentManager>();

    RefPtr<ScriptClass> driver =
        MakeClassLang(u8"angelscript", u8"Driver",
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
    CHECK(characters->Get(e)->jumpSpeed == doctest::Approx(6.0f));
}

// The scriptable-impulse gameplay op (Roll Call finding #1: "no scriptable impulse on a dynamic
// body"), now available: ScenePhysics.of(scene).applyImpulse(entity, x, y, z). A world op, keyed
// by entity. The upward impulse gives the dynamic body positive Y velocity.
TEST_CASE("script.scene: ScenePhysics.of(scene).applyImpulse(entity, ...) - scriptable impulse (Wren)")
{
    physics::RegisterPhysicsScriptFacade();
    ContactWorld world;
    const scene::EntityHandle e = world.AddBody(u8"ball", Float3{0, 5, 0},
                                                physics::MotionKind::Dynamic, Float3{0.5f, 0.5f, 0.5f});
    RefPtr<ScriptClass> pusher = MakeClass(u8"Pusher",
                                           u8"class Pusher {\n"
                                           u8"    construct new(entity) { _entity = entity }\n"
                                           u8"    onStart() {\n"
                                           u8"        ScenePhysics.of(_entity.scene).applyImpulse(_entity, 0, 5000, 0)\n"
                                           u8"    }\n"
                                           u8"}\n",
                                           {u8"onStart"});
    world.Attach(e, pusher);

    world.Play(2); // onStart applies the impulse; the next step integrates it
    auto* sys = world.scene->GetSystem<physics::PhysicsSceneSystem>();
    auto* bodies = world.scene->GetSystem<physics::RigidBodyComponentManager>();
    REQUIRE(sys->World() != nullptr);
    REQUIRE(bodies->Get(e) != nullptr);
    CHECK(sys->World()->LinearVelocity(bodies->Get(e)->body).y > 0.0f); // impulse dominated gravity
}

// ---- Track B: the scene event bus reaches script. A behavior publishes scene.events.emit(name, x);
//      EVERY behavior AND the Level that declare on<Name>(x) receive it (same bus, both tiers), the
//      payload surviving as a Variant. Auto-unsubscribe when a subscriber is torn down. The bus is
//      native + backend-neutral, so proving each backend on its own bus proves the pair. ----

TEST_CASE("script.scene: scene.events.emit reaches a sibling behavior AND the Level's "
          "on<Event>(payload) - both tiers, one bus (Wren)")
{
    ScriptedScene bed;
    SceneScriptSystem* level = bed.scene.AddSystem<SceneScriptSystem>();
    level->SetRunHost(&bed.host);

    RefPtr<ScriptClass> emitter =
        MakeClass(u8"Emitter",
                  u8"class Emitter {\n"
                  u8"    construct new(entity) {\n"
                  u8"        _entity = entity\n"
                  u8"        _done = false\n"
                  u8"    }\n"
                  u8"    onUpdate(dt) {\n"
                  u8"        if (!_done) {\n"
                  u8"            _entity.scene.events.emit(\"OrbCollected\", 5)\n"
                  u8"            _done = true\n"
                  u8"        }\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    RefPtr<ScriptClass> receiver =
        MakeClass(u8"Receiver",
                  u8"class Receiver {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onOrbCollected(n) { _entity.setName(\"got:\" + n.toString) }\n"
                  u8"}\n",
                  {u8"onOrbCollected"});
    RefPtr<ScriptClass> levelClass =
        MakeClass(u8"Level",
                  u8"class Level {\n"
                  u8"    construct new(scene) { _scene = scene }\n"
                  u8"    onOrbCollected(n) { _scene.find(\"levelmark\").setName(\"level:\" + n.toString) }\n"
                  u8"}\n",
                  {u8"onOrbCollected"});
    level->Settings().script = levelClass;

    const scene::EntityHandle listener = bed.AddScripted(receiver, u8"listener");
    (void)bed.AddScripted(emitter, u8"emitter");
    const scene::EntityHandle levelmark = bed.scene.CreateEntity(u8"levelmark");

    bed.Start();
    bed.Frame(); // instantiate all; emitter.onUpdate emits; bus drains same frame -> both receive
    bed.Frame(); // slack for any instantiation/subscription ordering

    // The sibling behavior received on<Event>(payload) - payload 5 survived as a Variant.
    CHECK(bed.scene.GetEntityName(listener) == StringView(u8"got:5"));
    // The Level received the SAME event on the SAME bus (P-B1 named inbox) - no GameManager entity.
    CHECK(bed.scene.GetEntityName(levelmark) == StringView(u8"level:5"));
}

TEST_CASE("script.scene: scene.events.emit reaches a sibling behavior AND the Level's "
          "on<Event>(payload) - the 2nd backend (AngelScript)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
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

TEST_CASE("script.scene: a destroyed subscriber stops receiving bus events; the emitter "
          "keeps running fault-free (teardown)")
{
    ScriptedScene bed;

    RefPtr<ScriptClass> emitter =
        MakeClass(u8"Bumper",
                  u8"class Bumper {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onUpdate(dt) { _entity.scene.events.emit(\"Bump\", 1) }\n"
                  u8"}\n",
                  {u8"onUpdate"});
    RefPtr<ScriptClass> receiver =
        MakeClass(u8"Counter",
                  u8"class Counter {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onBump(n) {\n"
                  u8"        var c = _entity.scene.find(\"counter\")\n"
                  u8"        var p = c.position()\n"
                  u8"        c.setPosition(p.x + n, 0, 0)\n"
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
    // The counter is frozen: the destroyed subscriber no longer receives, and BroadcastEvent's
    // per-behavior re-resolution simply finds nothing to deliver to (the system subscription is
    // harmlessly inert). The emitter kept publishing without faulting.
    CHECK(Near(bed.scene.GetLocalTransform(counter).position.x, whileAlive));
    ScriptComponent* emitterComp = bed.components->Get(emitterEntity);
    REQUIRE(emitterComp != nullptr);
    CHECK_FALSE(emitterComp->behaviors[0].faulted);
}

// The generic payload: a REFLECTED VALUE (here a live component handle from Gadget.of) rides the
// bus as a Variant and the receiver reads its live field. Proves the emit(name, Variant) sink +
// the ?&in / Variant-accepts-any overload path - the design's "reflected value" payload.
TEST_CASE("script.scene: scene.events.emit carries a reflected component handle as the payload; "
          "the receiver reads its live field (Wren)")
{
    EnsureGadgetRegistered();
    ScriptedScene bed;
    GadgetManager* gadgets = bed.scene.AddSystem<GadgetManager>();

    RefPtr<ScriptClass> producer =
        MakeClass(u8"Producer",
                  u8"class Producer {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() { _entity.scene.events.emit(\"Configured\", Gadget.of(_entity)) }\n"
                  u8"}\n",
                  {u8"onStart"});
    RefPtr<ScriptClass> consumer =
        MakeClass(u8"Consumer",
                  u8"class Consumer {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onConfigured(g) { Gadget.of(_entity).power = g.power }\n"
                  u8"}\n",
                  {u8"onConfigured"});

    const scene::EntityHandle producerEntity = bed.AddScripted(producer, u8"producer");
    const scene::EntityHandle consumerEntity = bed.AddScripted(consumer, u8"consumer");
    gadgets->Add(producerEntity).power = 7.0f; // the value the payload carries
    gadgets->Add(consumerEntity).power = 1.0f; // overwritten from the payload's live field

    bed.Start();
    bed.Frame(); // producer.onStart emits Gadget.of(producer); bus drains -> consumer.onConfigured
    bed.Frame();

    REQUIRE(gadgets->Get(consumerEntity) != nullptr);
    // The payload handle re-resolved to the producer's live Gadget (power 7) across the bus - a
    // reflected VALUE survived as the event payload, and the receiver read its live field.
    CHECK(gadgets->Get(consumerEntity)->power == doctest::Approx(7.0f));
}

// ---- Track A (render surface): MeshComponent.of / LightComponent.of - a behavior reaches the real
//      render components by type and mutates them live, exactly like the physics components. Pure-data
//      props (visible/intensity/range/enabled); resource-ref swaps (mesh/material) are Phase 1b. ----

TEST_CASE("script.scene: MeshComponent.of / LightComponent.of set live render props (Wren)")
{
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* meshes = bed.scene.AddSystem<render::MeshComponentManager>();
    auto* lights = bed.scene.AddSystem<render::LightComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"Tweaker",
                  u8"class Tweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        MeshComponent.of(_entity).visible = false\n"
                  u8"        var light = LightComponent.of(_entity)\n"
                  u8"        light.intensity = 4.0\n"
                  u8"        light.range = 25.0\n"
                  u8"        light.enabled = false\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    meshes->Add(e).visible = true;
    render::LightComponent& light = lights->Add(e);
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

TEST_CASE("script.scene: MeshComponent::of / LightComponent::of set live render props (AngelScript)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* meshes = bed.scene.AddSystem<render::MeshComponentManager>();
    auto* lights = bed.scene.AddSystem<render::LightComponentManager>();

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

// ---- Track A phase 1b: the resource-swap primitive. SceneRender.of(scene).setMesh(entity, id) /
//      setMaterial(entity, id) - a WORLD op keyed by entity (it needs the run's resource manager,
//      which the component data cannot reach), so it lives on the scene-handle like ScenePhysics.
//      The resource id (a Guid) reaches the behavior as an asset property (the "swap to the mesh I
//      picked" pattern). Here no ResourceManager is wired, so the swap sets the id (the script->
//      component path); Ref::Bind resolution is covered natively in the render resource-ref tests. ----

TEST_CASE("script.scene: SceneRender.setMesh / setMaterial swap a component's resource id from "
          "script (Wren)")
{
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* meshes = bed.scene.AddSystem<render::MeshComponentManager>();

    RefPtr<ScriptClass> swapper =
        MakeClass(u8"Swapper",
                  u8"class Swapper {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    meshRes=(v) { _meshRes = v }\n"
                  u8"    matRes=(v) { _matRes = v }\n"
                  u8"    onStart() {\n"
                  u8"        var r = SceneRender.of(_entity.scene)\n"
                  u8"        r.setMesh(_entity, _meshRes)\n"
                  u8"        r.setMaterial(_entity, _matRes)\n"
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
}

// ---- Track A phase 2 (audio surface): AudioSourceComponent.of (live volume/pitch/loop - DATA) +
//      SceneAudio.of(scene) (play/stop/pause/isPlaying/setClip - WORLD ops keyed by entity, reaching
//      the scene's AudioEngine). No engine is wired here (the ops are null-safe), so this proves the
//      script surface + forwarding + the data/resource paths cross-backend; actual voice playback is
//      covered natively in AudioSceneTests. ----

TEST_CASE("script.scene: AudioSourceComponent.of props + SceneAudio.of play/stop/pause/setClip "
          "are bound and control the component (Wren)")
{
    audio::RegisterAudioScriptFacade();
    ScriptedScene bed;
    auto* sources = bed.scene.AddSystem<audio::AudioSourceComponentManager>();
    (void)bed.scene.AddSystem<audio::AudioSceneSystem>(); // no engine set -> ops are safe no-ops

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"AudioTweaker",
                  u8"class AudioTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    clipRes=(v) { _clipRes = v }\n"
                  u8"    onStart() {\n"
                  u8"        var s = AudioSourceComponent.of(_entity)\n"
                  u8"        s.volume = 0.25\n"
                  u8"        s.pitch = 2.0\n"
                  u8"        s.loop = true\n"
                  u8"        var a = SceneAudio.of(_entity.scene)\n"
                  u8"        a.setClip(_entity, _clipRes)\n"
                  u8"        a.play(_entity)\n"
                  u8"        a.pause(_entity, true)\n"
                  u8"        a.stop(_entity)\n"
                  u8"        if (!a.isPlaying(_entity)) { _entity.setName(\"silent\") }\n"
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
    CHECK_FALSE(comp->behaviors[0].faulted); // play/stop/pause all bound + callable
}

TEST_CASE("script.scene: AudioSourceComponent::of props + SceneAudio::of play/stop/setClip "
          "are bound and control the component (AngelScript)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    audio::RegisterAudioScriptFacade();
    ScriptedScene bed;
    auto* sources = bed.scene.AddSystem<audio::AudioSourceComponentManager>();
    (void)bed.scene.AddSystem<audio::AudioSceneSystem>();

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

// An asset/resource (Guid) editor property applies to an AngelScript behavior when declared as a
// HANDLE member (`Guid@ res;`) - the idiomatic AngelScript spelling for a reference type. (A plain
// VALUE member is owned by AngelScript's lifecycle and can't take a native write; the runtime logs a
// warning guiding the author to a handle - see WriteTypedAddress / SetMemberField.) This closes the
// Wren/AngelScript parity gap for editor-picked resource properties.
TEST_CASE("script.scene: an asset (Guid) editor property applies to an AngelScript handle member")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    ScriptedScene bed;
    RefPtr<ScriptClass> cls = MakeClassLang(
        u8"angelscript", u8"Holder",
        u8"class Holder {\n"
        u8"    private Entity@ self;\n"
        u8"    Guid@ res;\n" // reflected/resource property -> a handle member
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

// The rest of the render component set also reaches script via `.of` (all pure-data): Camera,
// Sprite, Decal, InstancedMesh, ReflectionProbe. Spot-check Camera + Sprite from a behavior.
TEST_CASE("script.scene: CameraComponent.of / SpriteComponent.of set live props (Wren)")
{
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* cameras = bed.scene.AddSystem<render::CameraComponentManager>();
    auto* sprites = bed.scene.AddSystem<render::SpriteComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"ViewTweaker",
                  u8"class ViewTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var cam = CameraComponent.of(_entity)\n"
                  u8"        cam.fovYRadians = 1.2\n"
                  u8"        cam.primary = false\n"
                  u8"        SpriteComponent.of(_entity).visible = false\n"
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

// ---- Track A phase 3 (animation surface): SkeletalAnimationComponent.of / AnimationGraphComponent.of
//      (live speed/autoPlay/active - DATA) + SceneAnimation.of(scene) (play/stop/pause + graph params
//      setFloat/setBool/setTrigger + setClip - WORLD ops keyed by entity, reaching the manager-owned
//      runtime player). No skeleton/graph is bound here, so the players are never built and the world
//      ops are safe no-ops; this proves the surface + forwarding + data/resource paths cross-backend.
//      Actual playback is covered natively in the animation tests. ----

TEST_CASE("script.scene: SkeletalAnimation/AnimationGraph .of + SceneAnimation ops are bound and "
          "control the components (Wren)")
{
    anim::RegisterAnimationScriptFacade();
    ScriptedScene bed;
    auto* skel = bed.scene.AddSystem<anim::SkeletalAnimationComponentManager>();
    auto* graph = bed.scene.AddSystem<anim::AnimationGraphComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"AnimTweaker",
                  u8"class AnimTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    clipRes=(v) { _clipRes = v }\n"
                  u8"    onStart() {\n"
                  u8"        var a = SkeletalAnimationComponent.of(_entity)\n"
                  u8"        a.speed = 2.0\n"
                  u8"        a.autoPlay = false\n"
                  u8"        AnimationGraphComponent.of(_entity).active = false\n"
                  u8"        var s = SceneAnimation.of(_entity.scene)\n"
                  u8"        s.setClip(_entity, _clipRes)\n"
                  u8"        s.play(_entity)\n"
                  u8"        s.stop(_entity)\n"
                  u8"        s.setFloat(_entity, \"speed\", 0.5)\n"
                  u8"        s.setBool(_entity, \"grounded\", true)\n"
                  u8"        s.setTrigger(_entity, \"jump\")\n"
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
    draconic::script::angelscript::RegisterAngelScriptBackend();
    anim::RegisterAnimationScriptFacade();
    ScriptedScene bed;
    auto* skel = bed.scene.AddSystem<anim::SkeletalAnimationComponentManager>();
    (void)bed.scene.AddSystem<anim::AnimationGraphComponentManager>();

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

// ---- Track A phase 4 (render scene-systems): EnvironmentSettings.of(scene) / PostProcessSettings.of(
//      scene) - a re-resolving handle over the scene's ONE-PER-SCENE render settings (owned by the
//      EnvironmentSystem / PostProcessSystem). A behavior edits the live sky/ambient + exposure/bloom.
//      Scene-scoped analogue of a component's `.of`. (No active-camera handle - that design is the
//      user's to bless; the "first entity named Camera" convention stands.) ----

TEST_CASE("script.scene: EnvironmentSettings.of / PostProcessSettings.of edit the scene's live "
          "render settings (Wren)")
{
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* envSys = bed.scene.AddSystem<render::EnvironmentSystem>();
    auto* postSys = bed.scene.AddSystem<render::PostProcessSystem>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"EnvTweaker",
                  u8"class EnvTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var env = EnvironmentSettings.of(_entity.scene)\n"
                  u8"        env.ambientIntensity = 0.75\n"
                  u8"        env.skyIntensity = 2.0\n"
                  u8"        var post = PostProcessSettings.of(_entity.scene)\n"
                  u8"        post.exposureEV = 1.5\n"
                  u8"        post.bloomEnabled = true\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    (void)bed.AddScripted(tweaker, u8"e");
    // Sanity: defaults differ from what the behavior sets.
    REQUIRE(envSys->Environment().ambientIntensity != doctest::Approx(0.75f));

    bed.Start();
    bed.Frame(); // onStart edits the LIVE settings through the re-resolving handles

    CHECK(envSys->Environment().ambientIntensity == doctest::Approx(0.75f));
    CHECK(envSys->Environment().skyIntensity == doctest::Approx(2.0f));
    CHECK(postSys->Post().exposureEV == doctest::Approx(1.5f));
    CHECK(postSys->Post().bloomEnabled);
}

TEST_CASE("script.scene: EnvironmentSettings::of / PostProcessSettings::of edit the scene's live "
          "render settings (AngelScript)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* envSys = bed.scene.AddSystem<render::EnvironmentSystem>();
    auto* postSys = bed.scene.AddSystem<render::PostProcessSystem>();

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

// ---- Track A phase 5 (particle surface): ParticleEffectComponent.of (live visible/meshScale/light*
//      - DATA) + SceneParticles.of(scene) (play/stop/restart/pause/isPlaying + setEffect - WORLD ops
//      keyed by entity, reaching the manager-owned runtime ParticleEffectInstance). No effect is
//      attached here, so the instance is never built and the world ops are safe no-ops; this proves
//      the surface + forwarding + data/resource paths cross-backend. Real sim is native. ----

TEST_CASE("script.scene: ParticleEffectComponent.of + SceneParticles.of ops are bound and control "
          "the component (Wren)")
{
    particles::RegisterParticleScriptFacade();
    ScriptedScene bed;
    auto* fx = bed.scene.AddSystem<particles::ParticleEffectComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"FxTweaker",
                  u8"class FxTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    effectRes=(v) { _effectRes = v }\n"
                  u8"    onStart() {\n"
                  u8"        var c = ParticleEffectComponent.of(_entity)\n"
                  u8"        c.visible = false\n"
                  u8"        c.lightIntensity = 8.0\n"
                  u8"        var p = SceneParticles.of(_entity.scene)\n"
                  u8"        p.setEffect(_entity, _effectRes)\n"
                  u8"        p.play(_entity)\n"
                  u8"        p.pause(_entity, true)\n"
                  u8"        p.restart(_entity)\n"
                  u8"        p.stop(_entity)\n"
                  u8"        if (!p.isPlaying(_entity)) { _entity.setName(\"idle\") }\n"
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
    CHECK_FALSE(comp->behaviors[0].faulted); // play/stop/restart/pause all bound + callable
}

TEST_CASE("script.scene: ParticleEffectComponent::of + SceneParticles::of ops are bound (AngelScript)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    particles::RegisterParticleScriptFacade();
    ScriptedScene bed;
    auto* fx = bed.scene.AddSystem<particles::ParticleEffectComponentManager>();

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

// Enum-typed component properties cross to script (Wren: underlying int). LightComponent.type is a
// LightType enum (Directional=0, Point=1, Spot=2). Proves the Wren enum marshalling half.
TEST_CASE("script.scene: an enum component property reads/writes as an int in Wren")
{
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* lights = bed.scene.AddSystem<render::LightComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"EnumTest",
                  u8"class EnumTest {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var l = LightComponent.of(_entity)\n"
                  u8"        if (l.type == 0) { l.type = 2 }\n" // Directional(0) -> Spot(2)
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    lights->Add(e).type = render::LightType::Directional; // 0

    bed.Start();
    bed.Frame();

    REQUIRE(lights->Get(e) != nullptr);
    CHECK(lights->Get(e)->type == render::LightType::Spot); // read (==0) + write (=2) both crossed
}

// AngelScript gets NATIVE enums (named). LightComponent.type is a LightType enum.
TEST_CASE("script.scene: an enum component property reads/writes as a native enum in AngelScript")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    render::RegisterRenderScriptFacade();
    ScriptedScene bed;
    auto* lights = bed.scene.AddSystem<render::LightComponentManager>();

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
    lights->Add(e).type = render::LightType::Directional;

    bed.Start();
    bed.Frame();

    REQUIRE(lights->Get(e) != nullptr);
    CHECK(lights->Get(e)->type == render::LightType::Spot); // named-enum read + write crossed
}

// ---- Track A (net surface): NetworkComponent.of - authority (an enum) for authority-gated gameplay.
TEST_CASE("script.scene: NetworkComponent.of exposes replication authority - AngelScript (named enum)")
{
    draconic::script::angelscript::RegisterAngelScriptBackend();
    draconic::net::RegisterNetworkComponentScriptFacade();
    ScriptedScene bed;
    auto* net = bed.scene.AddSystem<draconic::net::NetworkComponentManager>();

    RefPtr<ScriptClass> tweaker = MakeClassLang(
        u8"angelscript", u8"NetTweaker",
        u8"class NetTweaker {\n"
        u8"    private Entity@ self;\n"
        u8"    NetTweaker(Entity@ entity) { @self = entity; }\n"
        u8"    void onStart() {\n"
        u8"        NetworkComponent@ n = NetworkComponent::of(self);\n"
        u8"        if (n.authority == NetworkAuthority::Server) { self.setName(\"authoritative\"); }\n"
        u8"        n.authority = NetworkAuthority::Client;\n"
        u8"    }\n"
        u8"}\n",
        {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    net->Add(e).authority = draconic::net::NetworkAuthority::Server;

    bed.Start();
    bed.Frame();

    REQUIRE(net->Get(e) != nullptr);
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"authoritative"));
    CHECK(net->Get(e)->authority == draconic::net::NetworkAuthority::Client);
}

TEST_CASE("script.scene: NetworkComponent.of authority crosses as an int in Wren")
{
    draconic::net::RegisterNetworkComponentScriptFacade();
    ScriptedScene bed;
    auto* net = bed.scene.AddSystem<draconic::net::NetworkComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"NetTweaker",
                  u8"class NetTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var n = NetworkComponent.of(_entity)\n"
                  u8"        if (n.authority == 0) { _entity.setName(\"authoritative\") }\n"
                  u8"        n.authority = 1\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    net->Add(e).authority = draconic::net::NetworkAuthority::Server;

    bed.Start();
    bed.Frame();

    REQUIRE(net->Get(e) != nullptr);
    CHECK(bed.scene.GetEntityName(e) == StringView(u8"authoritative"));
    CHECK(net->Get(e)->authority == draconic::net::NetworkAuthority::Client);
}

// ---- Track A (UI world-space surface): UICanvasComponent / UIBillboardComponent /
//      UIWorldPanelComponent get the mechanical .of (live data - order/visible/interactive/scale...).
//      The app SCREEN tier (IScreenOverlay, loading screen) is deliberately NOT exposed here.
TEST_CASE("script.scene: the world-space UI components reach script via .of (Wren)")
{
    ui::RegisterUiScriptFacade();
    ScriptedScene bed;
    auto* canvases = bed.scene.AddSystem<ui::UICanvasComponentManager>();
    auto* billboards = bed.scene.AddSystem<ui::UIBillboardComponentManager>();
    auto* panels = bed.scene.AddSystem<ui::UIWorldPanelComponentManager>();

    RefPtr<ScriptClass> tweaker =
        MakeClass(u8"UiTweaker",
                  u8"class UiTweaker {\n"
                  u8"    construct new(entity) { _entity = entity }\n"
                  u8"    onStart() {\n"
                  u8"        var c = UICanvasComponent.of(_entity)\n"
                  u8"        c.visible = false\n"
                  u8"        c.order = 5\n"
                  u8"        UIBillboardComponent.of(_entity).minScale = 0.8\n"
                  u8"        UIWorldPanelComponent.of(_entity).interactive = false\n"
                  u8"    }\n"
                  u8"}\n",
                  {u8"onStart"});

    const scene::EntityHandle e = bed.AddScripted(tweaker, u8"e");
    canvases->Add(e);
    billboards->Add(e);
    panels->Add(e);

    bed.Start();
    bed.Frame();

    REQUIRE(canvases->Get(e) != nullptr);
    CHECK_FALSE(canvases->Get(e)->visible);
    CHECK(canvases->Get(e)->order == 5);
    REQUIRE(billboards->Get(e) != nullptr);
    CHECK(billboards->Get(e)->minScale == doctest::Approx(0.8f));
    REQUIRE(panels->Get(e) != nullptr);
    CHECK_FALSE(panels->Get(e)->interactive);
}
