// ScriptPlayground - the draconic.script entity-behaviors P1 consumer proof: a scene of
// cubes carrying WREN BEHAVIORS ticked by the ScriptSubsystem under simulation. Two
// behaviors demonstrate the model end-to-end:
//   * Mover  - reads a `speed` float property + a `target` entity property; walks toward
//              the target (or drifts +X when unset), using the `entity` facade's
//              transform get/set and worldPosition().
//   * Spinner - reads a `speed` float; rotates about Y every frame (Time.delta()).
// Behavior instances are created in the run's ONE gameplay context, defaults applied
// then per-behavior OVERRIDES, and lifecycle handlers (onStart/onUpdate) dispatched -
// exactly the runtime a cooked ScriptClass drives; here the ScriptClass products are
// built in code (the cook path is proven by the pipeline tests).
//
// The same behaviors run in Draconic.Engine.Player: point a project's scene at a ScriptComponent
// with these classes and it ticks identically (the player and this sample share the
// DefaultApplication ScriptSubsystem).
//
// Fly with WASD / hold RMB to look. No input needed - the cubes move themselves.

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Runtime.Client/AppMain.h"

import draconic.foundation;
import draconic.runtime;
import draconic.runtime.client;
import draconic.engine.defaultapp;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.scene;
import draconic.engine.scene;
import draconic.engine.render;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.materials;
import draconic.materials.resource;
import draconic.script;
import draconic.script.resource;
import draconic.engine.script;

#include "../Common/FlyCamera.h"

namespace foundation = draconic::foundation;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace script = draconic::script;

using foundation::f32;

namespace
{
    // ---- the two behavior classes (the cook harvests these `static properties`; here
    // we build the metadata by hand to match, since the sample links no cooker) ----

    constexpr foundation::StringView kMoverSource =
        u8"import \"main\" for Float3\n"
        u8"class Mover {\n"
        u8"    static properties { {\n"
        u8"        \"speed\":  [\"float\", 2.0, \"units per second\"],\n"
        u8"        \"target\": [\"entity\", null, \"walk toward this entity\"],\n"
        u8"    } }\n"
        u8"    construct new(entity) {\n"
        u8"        _entity = entity\n"
        u8"        _speed = 2.0\n"
        u8"        _target = null\n"
        u8"    }\n"
        u8"    speed=(v) { _speed = v }\n"
        u8"    target=(v) { _target = v }\n"
        u8"    onStart() { Log.info(\"Mover %(_entity.name()) started\") }\n"
        u8"    onUpdate(dt) {\n"
        u8"        var p = _entity.position()\n"
        u8"        if (_target != null) {\n"
        u8"            var t = _target.worldPosition()\n"
        u8"            var dx = t.x - p.x\n"
        u8"            var dz = t.z - p.z\n"
        u8"            var len = (dx * dx + dz * dz).sqrt\n"
        u8"            if (len > 0.05) {\n"
        u8"                _entity.setPosition(p.x + dx / len * _speed * dt, p.y,\n"
        u8"                    p.z + dz / len * _speed * dt)\n"
        u8"            }\n"
        u8"        } else {\n"
        u8"            _entity.setPosition(p.x + _speed * dt, p.y, p.z)\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n";

    constexpr foundation::StringView kSpinnerSource =
        u8"class Spinner {\n"
        u8"    static properties { {\n"
        u8"        \"speed\": [\"float\", 90.0, \"degrees per second\"],\n"
        u8"    } }\n"
        u8"    construct new(entity) {\n"
        u8"        _entity = entity\n"
        u8"        _speed = 90.0\n"
        u8"        _angle = 0.0\n"
        u8"    }\n"
        u8"    speed=(v) { _speed = v }\n"
        u8"    onUpdate(dt) {\n"
        u8"        _angle = _angle + _speed * dt\n"
        u8"        _entity.setRotationEuler(0, _angle, 0)\n"
        u8"    }\n"
        u8"}\n";

    [[nodiscard]] script::ScriptPropertyDesc FloatProp(foundation::StringView name, f32 value,
                                                       foundation::StringView description)
    {
        script::ScriptPropertyDesc desc;
        desc.name = foundation::String(name);
        desc.hash = script::ScriptPropertyNameHash(name);
        desc.type = script::ScriptPropertyType::Float;
        desc.defaultValue.kind = script::ScriptPropertyType::Float;
        desc.defaultValue.number = static_cast<foundation::f64>(value);
        desc.description = foundation::String(description);
        return desc;
    }

    [[nodiscard]] foundation::RefPtr<script::ScriptClass> MakeMover()
    {
        auto cls = foundation::MakeRef<script::ScriptClass>(foundation::DefaultAllocator());
        cls->language = foundation::String(u8"wren");
        cls->className = foundation::String(u8"Mover");
        cls->source = foundation::String(kMoverSource);
        cls->properties.PushBack(FloatProp(u8"speed", 2.0f, u8"units per second"));
        script::ScriptPropertyDesc target;
        target.name = foundation::String(u8"target");
        target.hash = script::ScriptPropertyNameHash(u8"target");
        target.type = script::ScriptPropertyType::Entity;
        target.defaultValue.kind = script::ScriptPropertyType::Entity;
        cls->properties.PushBack(foundation::Move(target));
        cls->handlers.PushBack(foundation::String(u8"onStart"));
        cls->handlers.PushBack(foundation::String(u8"onUpdate"));
        cls->BuildProfileName();
        return cls;
    }

    [[nodiscard]] foundation::RefPtr<script::ScriptClass> MakeSpinner()
    {
        auto cls = foundation::MakeRef<script::ScriptClass>(foundation::DefaultAllocator());
        cls->language = foundation::String(u8"wren");
        cls->className = foundation::String(u8"Spinner");
        cls->source = foundation::String(kSpinnerSource);
        cls->properties.PushBack(FloatProp(u8"speed", 90.0f, u8"degrees per second"));
        cls->handlers.PushBack(foundation::String(u8"onUpdate"));
        cls->BuildProfileName();
        return cls;
    }

    class ScriptApp final : public runtime::DefaultApplication
    {
    public:
        ScriptApp()
        {
#ifdef DRACONIC_PLAYGROUND_FONT
            SetUIFontPath(reinterpret_cast<const foundation::utf8char*>(DRACONIC_PLAYGROUND_FONT));
#endif
        }

        void OnLaunch(runtime::IApplicationHost& host) override
        {
            // Route script logs (Log.info from behaviors, faults) to the console so the
            // sample self-reports (Mover onStart lines, any behavior fault).
            foundation::GlobalLogger().AddSink(&m_consoleSink);

            auto* scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"scripts");

            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_fly.position = foundation::Float3{0.0f, 6.0f, 16.0f};
            m_fly.pitch = -0.25f;

            m_mover = MakeMover();
            m_spinner = MakeSpinner();
            BuildWorld();

            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            foundation::ConsoleWrite(u8"ScriptPlayground: cubes driven by Wren Mover/Spinner "
                               u8"behaviors. WASD/RMB fly.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override
        {
            runtime::DefaultApplication::OnUpdate(host, dt); // ticks the ScriptSubsystem
            m_fly.Update(host, dt);
            PushCameraToEntity();
            auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            if (input != nullptr && input->Keyboard()->IsKeyPressed(shell::KeyCode::Escape))
            {
                host.RequestExit(0);
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
                {
                    if (render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect =
                            static_cast<f32>(frame.width) / static_cast<f32>(frame.height);
                    }
                }
            }
            runtime::DefaultApplication::OnRenderWindow(host, frame);
        }

    private:
        void BuildWorld()
        {
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            auto* scripts = m_scene->GetSystem<script::ScriptComponentManager>();
            if (meshes == nullptr || scripts == nullptr)
            {
                return;
            }

            foundation::RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(0.5f);

            // A ground slab (static) for a sense of place.
            {
                scene::EntityHandle ground = m_scene->CreateEntity(u8"ground");
                m_scene->SetLocalPosition(ground, foundation::Float3{0.0f, -0.6f, 0.0f});
                foundation::RefPtr<geometry::StaticMesh> slab = geometry::Primitives::Cube(1.0f);
                render::MeshComponent& mc = meshes->Add(ground);
                mc.mesh = slab;
                mc.SetMaterial(materials::CreatePBR(
                    u8"lit", foundation::Float4{0.15f, 0.16f, 0.19f, 1.0f}, 0.0f, 0.8f));
                scene::EntityHandle g = ground;
                foundation::Transform t = m_scene->GetLocalTransform(g);
                t.scale = foundation::Float3{30.0f, 0.2f, 30.0f};
                m_scene->SetLocalTransform(g, t);
            }

            // The Mover's TARGET (a spinning beacon the movers chase).
            m_beacon = m_scene->CreateEntity(u8"beacon");
            m_scene->SetLocalPosition(m_beacon, foundation::Float3{0.0f, 0.5f, 0.0f});
            {
                render::MeshComponent& mc = meshes->Add(m_beacon);
                mc.mesh = cube;
                mc.SetMaterial(materials::CreatePBR(u8"lit", foundation::Float4{1.0f, 0.85f, 0.2f, 1.0f},
                                                    0.0f, 0.3f));
                mc.color = foundation::Color{1.0f, 0.85f, 0.2f, 1.0f};
                // The beacon SPINS (Spinner behavior, default 90 deg/s).
                script::ScriptComponent& sc = scripts->Add(m_beacon);
                script::ScriptBehavior spin;
                spin.script.SetDirect(m_spinner);
                sc.behaviors.PushBack(foundation::Move(spin));
            }

            // A ring of movers chasing the beacon, each with a distinct speed override.
            constexpr int kMovers = 8;
            for (int i = 0; i < kMovers; ++i)
            {
                const f32 angle = static_cast<f32>(i) / kMovers * 6.2831853f;
                scene::EntityHandle e = m_scene->CreateEntity(u8"mover");
                m_scene->SetLocalPosition(
                    e, foundation::Float3{foundation::Cos(angle) * 8.0f, 0.5f, foundation::Sin(angle) * 8.0f});
                render::MeshComponent& mc = meshes->Add(e);
                mc.mesh = cube;
                const f32 hue = static_cast<f32>(i) / kMovers;
                mc.SetMaterial(materials::CreatePBR(
                    u8"lit", foundation::Float4{0.3f + 0.6f * hue, 0.4f, 1.0f - 0.6f * hue, 1.0f}, 0.0f,
                    0.4f));

                script::ScriptComponent& sc = scripts->Add(e);
                // Behavior 1: Mover with a per-instance speed OVERRIDE + the target entity.
                script::ScriptBehavior mover;
                mover.script.SetDirect(m_mover);
                script::ScriptPropertyValue speed;
                speed.kind = script::ScriptPropertyType::Float;
                speed.number = 0.6 + 0.25 * i; // distinct speeds
                mover.SetOverride(script::ScriptPropertyNameHash(u8"speed"), speed);
                script::ScriptPropertyValue target;
                target.kind = script::ScriptPropertyType::Entity;
                target.guid = m_scene->GetEntityId(m_beacon);
                mover.SetOverride(script::ScriptPropertyNameHash(u8"target"), target);
                sc.behaviors.PushBack(foundation::Move(mover));
                // Behavior 2 (ordered after the mover): Spinner, slower.
                script::ScriptBehavior spin;
                spin.script.SetDirect(m_spinner);
                script::ScriptPropertyValue spinSpeed;
                spinSpeed.kind = script::ScriptPropertyType::Float;
                spinSpeed.number = 45.0;
                spin.SetOverride(script::ScriptPropertyNameHash(u8"speed"), spinSpeed);
                sc.behaviors.PushBack(foundation::Move(spin));
            }
        }

        void PushCameraToEntity()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            foundation::Transform t;
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_beacon{};
        foundation::RefPtr<script::ScriptClass> m_mover;
        foundation::RefPtr<script::ScriptClass> m_spinner;
        foundation::ConsoleSink m_consoleSink;
        draconic::samples::FlyCamera m_fly;
    };
}

DRACONIC_APP_MAIN(ScriptApp)
