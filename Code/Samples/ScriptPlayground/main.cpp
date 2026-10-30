// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ScriptPlayground - the foundation.script entity-behaviors consumer proof: a scene of
// cubes carrying SCRIPT BEHAVIORS (AngelScript or Luau, chosen with --script=<lang>) ticked by
// the ScriptSubsystem under simulation. Both behaviors ship a source in EACH language; the run
// resolves the context by the ScriptClass language, proving the model is backend-neutral. Two
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
// The same behaviors run in Engine.Player: point a project's scene at a ScriptComponent
// with these classes and it ticks identically (the player and this sample share the
// DefaultApplication ScriptSubsystem).
//
// Fly with WASD / hold RMB to look. No input needed - the cubes move themselves.

#include <cstring>

#include "Core/Prelude.h"
#include "Runtime.Client/AppMain.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.scene;
import engine.scene;
import engine.render;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.materials;
import foundation.materials.resource;
import foundation.script;
import foundation.script.resource;
import engine.script;

#include "../Common/FlyCamera.h"

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace shell = foundation::shell;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace script = foundation::script;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}




using core::f32;

namespace
{
    // ---- the two behavior classes (the cook harvests these `static properties`; here
    // we build the ScriptClass metadata by hand, since the sample links no cooker) ----

    // AngelScript behaviors. The harvested editor properties are plain public member fields (the
    // neutral property-apply writes `speed`/`target` by name); handlers are onStart/onUpdate. The
    // engine registers no sqrt, so the target-follow eases toward the goal (step scaled by distance)
    // rather than moving at a normalized constant speed - visually a smooth approach either way.
    constexpr core::StringView kMoverSource =
        u8"class Mover {\n"
        u8"    Entity@ self;\n"
        u8"    float speed;\n"
        u8"    Entity@ target;\n"
        u8"    Mover(Entity@ entity) { @self = entity; speed = 2.0f; @target = null; }\n"
        u8"    void onStart() { Log::info(\"Mover \" + self.name() + \" started\"); }\n"
        u8"    void onUpdate(double dt) {\n"
        u8"        float step = speed * float(dt);\n"
        u8"        Float3 p = self.position();\n"
        u8"        if (target !is null) {\n"
        u8"            Float3 t = target.worldPosition();\n"
        u8"            self.setPosition(p.x + (t.x - p.x) * step, p.y, p.z + (t.z - p.z) * step);\n"
        u8"        } else {\n"
        u8"            self.setPosition(p.x + step, p.y, p.z);\n"
        u8"        }\n"
        u8"    }\n"
        u8"}\n";

    constexpr core::StringView kSpinnerSource =
        u8"class Spinner {\n"
        u8"    Entity@ self;\n"
        u8"    float speed;\n"
        u8"    float angle;\n"
        u8"    Spinner(Entity@ entity) { @self = entity; speed = 90.0f; angle = 0.0f; }\n"
        u8"    void onUpdate(double dt) {\n"
        u8"        angle = angle + speed * float(dt);\n"
        u8"        self.setRotationEuler(0.0f, angle, 0.0f);\n"
        u8"    }\n"
        u8"}\n";

    // The SAME two behaviors in Luau (statics/methods via `:`, the entity facade is the shared
    // reflected surface). The neutral property-apply writes `speed`/`target` into the instance
    // table after construction, exactly as the AngelScript members are written by name.
    constexpr core::StringView kMoverSourceLuau =
        u8"Mover = {}\n"
        u8"Mover.__index = Mover\n"
        u8"function Mover.new(entity)\n"
        u8"    local self = setmetatable({}, Mover)\n"
        u8"    self.entity = entity\n"
        u8"    self.speed = 2.0\n"
        u8"    self.target = nil\n"
        u8"    return self\n"
        u8"end\n"
        u8"function Mover:onStart()\n"
        u8"    Log.info(\"Mover \" .. self.entity:name() .. \" started\")\n"
        u8"end\n"
        u8"function Mover:onUpdate(dt)\n"
        u8"    local step = self.speed * dt\n"
        u8"    local p = self.entity:position()\n"
        u8"    if self.target ~= nil then\n"
        u8"        local t = self.target:worldPosition()\n"
        u8"        self.entity:setPosition(p.x + (t.x - p.x) * step, p.y, p.z + (t.z - p.z) * step)\n"
        u8"    else\n"
        u8"        self.entity:setPosition(p.x + step, p.y, p.z)\n"
        u8"    end\n"
        u8"end\n";

    constexpr core::StringView kSpinnerSourceLuau =
        u8"Spinner = {}\n"
        u8"Spinner.__index = Spinner\n"
        u8"function Spinner.new(entity)\n"
        u8"    local self = setmetatable({}, Spinner)\n"
        u8"    self.entity = entity\n"
        u8"    self.speed = 90.0\n"
        u8"    self.angle = 0.0\n"
        u8"    return self\n"
        u8"end\n"
        u8"function Spinner:onUpdate(dt)\n"
        u8"    self.angle = self.angle + self.speed * dt\n"
        u8"    self.entity:setRotationEuler(0.0, self.angle, 0.0)\n"
        u8"end\n";

    // The behavior backend chosen at launch (--script=angelscript|luau; default angelscript). Set
    // by main() before the scene is built; read by MakeMover/MakeSpinner. Every built backend is
    // registered by DefaultApplication, so the run resolves this language with no sample-side setup.
    core::String g_scriptLanguage = core::String(u8"angelscript");
    [[nodiscard]] bool UseLuau() { return g_scriptLanguage.AsView() == core::StringView(u8"luau"); }

    [[nodiscard]] script::ScriptPropertyDesc FloatProp(core::StringView name, f32 value,
                                                       core::StringView description)
    {
        script::ScriptPropertyDesc desc;
        desc.name = core::String(name);
        desc.hash = script::ScriptPropertyNameHash(name);
        desc.type = script::ScriptPropertyType::Float;
        desc.defaultValue.kind = script::ScriptPropertyType::Float;
        desc.defaultValue.number = static_cast<core::f64>(value);
        desc.description = core::String(description);
        return desc;
    }

    [[nodiscard]] core::RefPtr<script::ScriptClass> MakeMover()
    {
        auto cls = core::MakeRef<script::ScriptClass>(AppRoot());
        cls->language = g_scriptLanguage;
        cls->className = core::String(u8"Mover");
        cls->source = core::String(UseLuau() ? kMoverSourceLuau : kMoverSource);
        cls->properties.PushBack(FloatProp(u8"speed", 2.0f, u8"units per second"));
        script::ScriptPropertyDesc target;
        target.name = core::String(u8"target");
        target.hash = script::ScriptPropertyNameHash(u8"target");
        target.type = script::ScriptPropertyType::Entity;
        target.defaultValue.kind = script::ScriptPropertyType::Entity;
        cls->properties.PushBack(core::Move(target));
        cls->handlers.PushBack(core::String(u8"onStart"));
        cls->handlers.PushBack(core::String(u8"onUpdate"));
        cls->BuildProfileName();
        return cls;
    }

    [[nodiscard]] core::RefPtr<script::ScriptClass> MakeSpinner()
    {
        auto cls = core::MakeRef<script::ScriptClass>(AppRoot());
        cls->language = g_scriptLanguage;
        cls->className = core::String(u8"Spinner");
        cls->source = core::String(UseLuau() ? kSpinnerSourceLuau : kSpinnerSource);
        cls->properties.PushBack(FloatProp(u8"speed", 90.0f, u8"degrees per second"));
        cls->handlers.PushBack(core::String(u8"onUpdate"));
        cls->BuildProfileName();
        return cls;
    }

    class ScriptApp final : public engine::runtime::DefaultApplication
    {
    public:
        void OnLaunch(runtime::IApplicationHost& host) override
        {
            // Route script logs (Log.info from behaviors, faults) to the console so the
            // sample self-reports (Mover onStart lines, any behavior fault).
            core::GlobalLogger().AddSink(&m_consoleSink);

            auto* scenes = host.Subsystems().GetSubsystem<engine::scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"scripts");

            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_fly.position = core::Float3{0.0f, 6.0f, 16.0f};
            m_fly.pitch = -0.25f;

            m_mover = MakeMover();
            m_spinner = MakeSpinner();
            BuildWorld();

            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            core::ConsoleWrite(UseLuau()
                                   ? u8"ScriptPlayground: cubes driven by LUAU Mover/Spinner "
                                     u8"behaviors. WASD/RMB fly.\n"
                                   : u8"ScriptPlayground: cubes driven by ANGELSCRIPT Mover/Spinner "
                                     u8"behaviors. WASD/RMB fly.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, dt); // ticks the ScriptSubsystem
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
                if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
                {
                    if (engine::render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect =
                            static_cast<f32>(frame.width) / static_cast<f32>(frame.height);
                    }
                }
            }
            RenderFrame(host, frame); // the scenes + window overlays; FinishFrame below closes the shot
            FinishFrame(host, frame); // the screenshot copy, with the overlay in it
        }

    private:
        void BuildWorld()
        {
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            auto* scripts = m_scene->GetSystem<engine::script::ScriptComponentManager>();
            if (meshes == nullptr || scripts == nullptr)
            {
                return;
            }

            core::RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(AppRoot(), 0.5f);

            // A ground slab (static) for a sense of place.
            {
                scene::EntityHandle ground = m_scene->CreateEntity(u8"ground");
                m_scene->SetLocalPosition(ground, core::Float3{0.0f, -0.6f, 0.0f});
                core::RefPtr<geometry::StaticMesh> slab = geometry::Primitives::Cube(AppRoot(), 1.0f);
                engine::render::MeshComponent& mc = meshes->Add(ground);
                mc.mesh = slab;
                mc.SetMaterial(materials::CreatePBR(
                    u8"lit", core::Float4{0.15f, 0.16f, 0.19f, 1.0f}, 0.0f, 0.8f));
                scene::EntityHandle g = ground;
                core::Transform t = m_scene->GetLocalTransform(g);
                t.scale = core::Float3{30.0f, 0.2f, 30.0f};
                m_scene->SetLocalTransform(g, t);
            }

            // The Mover's TARGET (a spinning beacon the movers chase).
            m_beacon = m_scene->CreateEntity(u8"beacon");
            m_scene->SetLocalPosition(m_beacon, core::Float3{0.0f, 0.5f, 0.0f});
            {
                engine::render::MeshComponent& mc = meshes->Add(m_beacon);
                mc.mesh = cube;
                mc.SetMaterial(materials::CreatePBR(u8"lit", core::Float4{1.0f, 0.85f, 0.2f, 1.0f},
                                                    0.0f, 0.3f));
                mc.color = core::Color{1.0f, 0.85f, 0.2f, 1.0f};
                // The beacon SPINS (Spinner behavior, default 90 deg/s).
                engine::script::ScriptComponent& sc = scripts->Add(m_beacon);
                engine::script::ScriptBehavior spin;
                spin.script.SetDirect(m_spinner);
                sc.behaviors.PushBack(core::Move(spin));
            }

            // A ring of movers chasing the beacon, each with a distinct speed override.
            constexpr int kMovers = 8;
            for (int i = 0; i < kMovers; ++i)
            {
                const f32 angle = static_cast<f32>(i) / kMovers * 6.2831853f;
                scene::EntityHandle e = m_scene->CreateEntity(u8"mover");
                m_scene->SetLocalPosition(
                    e, core::Float3{core::Cos(angle) * 8.0f, 0.5f, core::Sin(angle) * 8.0f});
                engine::render::MeshComponent& mc = meshes->Add(e);
                mc.mesh = cube;
                const f32 hue = static_cast<f32>(i) / kMovers;
                mc.SetMaterial(materials::CreatePBR(
                    u8"lit", core::Float4{0.3f + 0.6f * hue, 0.4f, 1.0f - 0.6f * hue, 1.0f}, 0.0f,
                    0.4f));

                engine::script::ScriptComponent& sc = scripts->Add(e);
                // Behavior 1: Mover with a per-instance speed OVERRIDE + the target entity.
                engine::script::ScriptBehavior mover;
                mover.script.SetDirect(m_mover);
                script::ScriptPropertyValue speed;
                speed.kind = script::ScriptPropertyType::Float;
                speed.number = 0.6 + 0.25 * i; // distinct speeds
                mover.SetOverride(script::ScriptPropertyNameHash(u8"speed"), speed);
                script::ScriptPropertyValue target;
                target.kind = script::ScriptPropertyType::Entity;
                target.guid = m_scene->GetEntityId(m_beacon);
                mover.SetOverride(script::ScriptPropertyNameHash(u8"target"), target);
                sc.behaviors.PushBack(core::Move(mover));
                // Behavior 2 (ordered after the mover): Spinner, slower.
                engine::script::ScriptBehavior spin;
                spin.script.SetDirect(m_spinner);
                script::ScriptPropertyValue spinSpeed;
                spinSpeed.kind = script::ScriptPropertyType::Float;
                spinSpeed.number = 45.0;
                spin.SetOverride(script::ScriptPropertyNameHash(u8"speed"), spinSpeed);
                sc.behaviors.PushBack(core::Move(spin));
            }
        }

        void PushCameraToEntity()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            core::Transform t;
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_beacon{};
        core::RefPtr<script::ScriptClass> m_mover;
        core::RefPtr<script::ScriptClass> m_spinner;
        core::ConsoleSink m_consoleSink;
        samples::FlyCamera m_fly;
    };
}

// A custom entry (not APP_MAIN) so the sample can read --script=<lang> before the app runs; the
// rest mirrors the desktop APP_MAIN body. The web build has no argv, so it keeps APP_MAIN + the
// default (angelscript) backend.
#ifdef PLATFORM_WEB
APP_MAIN(ScriptApp)
#else
int main(int argc, char** argv)
{
    // --script=angelscript|luau selects the behavior backend (default angelscript). Unknown values
    // are ignored (the default stands); graphics args (--gpu=...) are still parsed below.
    for (int i = 1; i < argc; ++i)
    {
        constexpr const char* kFlag = "--script=";
        const std::size_t flagLen = std::strlen(kFlag);
        if (std::strncmp(argv[i], kFlag, flagLen) == 0)
        {
            const core::StringView requested(
                reinterpret_cast<const core::utf8char*>(argv[i] + flagLen));
            if (requested == core::StringView(u8"luau") ||
                requested == core::StringView(u8"angelscript"))
            {
                g_scriptLanguage = core::String(requested);
            }
        }
    }

    static core::ConsoleSink appConsoleSink;
    core::GlobalLogger().AddSink(&appConsoleSink);
    auto shell = shell::CreateShell(MemoryAllocator());
    graphics::GraphicsDeviceDesc appGpuDesc = graphics::DeviceDescFromArguments(argc, argv);
    auto appGpu = graphics::CreateGraphicsDevice(appGpuDesc);
    graphics::GraphicsDevice* appDevice = appGpu.HasValue() ? appGpu.Value().Get() : nullptr;
    ScriptApp app;
    app.OnCommandLine(argc, argv); // --screenshot and whatever else the base app reads
    return runtime::RunApplication(app, *shell, appDevice);
}
#endif
