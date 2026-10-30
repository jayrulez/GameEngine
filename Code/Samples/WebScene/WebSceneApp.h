// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WebSceneApp - the FULL-RENDERER exercise scene, shared by the desktop and web entry points
// (Main.cpp / WebMain.cpp - the PlayerApplication.h pattern: this header uses the modules the
// including TU imports). One lean, procedurally built scene that touches every renderer feature,
// so desktop --vulkan / --webgpu and the browser can be compared side by side:
//
//   - analytic (Preetham) sky -> IBL bake (env cube + SH diffuse + prefiltered specular + BRDF)
//   - directional sun with CSM shadows (TOGGLEABLE - local shadows must survive sun-off: the
//     shadowParams.y regression class), plus a spot and an orbiting point light, both shadowed
//   - a roughness x metallic sphere grid + a glossy floor (SSR) + a spinning cube (TAA motion)
//   - a chrome sphere inside a box reflection probe (parallax probe path)
//   - an instanced-mesh ring (per-instance addressing path)
//   - a projected decal + three sprites (alpha / additive / post-tonemap) off one procedural texture
//   - a particle fountain (additive billboards) + spark trails (ribbon path)
//   - an ImGui panel (when the extension is available - desktop and web alike) tweaking
//     exposure/sky/post (TAA/FXAA/bloom/AO/SSR/MSAA) and the feature toggles
//   - debug draw (grid/axes/wire volumes/3D text via DebugScene + FPS overlay via DebugScreen)
//   - game UI on the engine's GUI (Engine.GUI): a scene-tier HUD canvas (click-counter button =
//     pointer consumption), a billboard nameplate riding the spinning cube, and a passive
//     screen-tier badge on the overlay layer - together they exercise both overlay passes (and
//     their stencil attachments) + the bundled font
//
// Not exercised yet: skinning (needs a skinned asset - procedural skinned content is its own task)
// and multi-view split-screen (Sandbox covers it; this scene stays single-view light).
#ifndef SAMPLES_WEBSCENE_APP_H
#define SAMPLES_WEBSCENE_APP_H

#include "../Common/FlyCamera.h" // shared free-fly camera (WASD/QE + RMB-look)

#if OPTION_HAS_EXTENSION_IMGUI
#include "imgui.h"

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}

#endif

namespace samples
{
    namespace core = foundation::core;
    namespace runtime = foundation::runtime;
    namespace graphics = foundation::graphics;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace geometry = foundation::geometry;
    namespace materials = foundation::materials;
    namespace particles = foundation::particles;
    namespace gui = foundation::gui;
    namespace rhi = foundation::rhi;
    namespace navigation = foundation::navigation;



    class WebSceneApp : public engine::runtime::DefaultApplication
    {
    public:
#if OPTION_HAS_EXTENSION_IMGUI
        void Configure(runtime::IApplicationHost& host) override
        {
            engine::runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Subsystems().AddSubsystem<extensions::imgui::ImguiSubsystem>(
                    *gfx->Raw(), gfx->FramesInFlight(), DataFileSystem());
            }
        }
#endif

        void OnStartup(runtime::IApplicationHost& host) override
        {
            // The base wires resource-type registration AND the GUI's render bring-up
            // (GuiSubsystem::EnsureRenderReady) - skip it and the GUI silently never draws.
            engine::runtime::DefaultApplication::OnStartup(host);
            core::ConsoleWrite(u8"WebScene: building the full-renderer scene...\n");
            if (host.Subsystems().GetSubsystem<engine::scene::SceneSubsystem>() == nullptr)
            {
                core::ConsoleWrite(u8"WebScene: no scene subsystem.\n");
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"web");

            // The exercise scene turns the optional passes ON by default - it exists to
            // exercise them (the panel can toggle everything off).
            if (auto* renderSub = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>())
            {
                renderSub->SetSsrEnabled(true);
                renderSub->SetExposure(0.9f);
            }

            BuildEnvironment();
            BuildTexture(host); // the shared procedural texture (decal + sprites)
            BuildGeometry();
            BuildLights();
            BuildProbe();
            BuildInstancedRing();
            BuildDecalAndSprites();
            BuildParticles();
            BuildCamera();
            BuildGameUI(host);
            BuildNavigation();

            // Simulation ON so the navigation crowd ticks (agents cross + avoid). The showcase's
            // manual animations (cube spin, orbiting light) are driven directly in OnUpdate and are
            // unaffected; there is no physics here, so nothing else simulates.
            m_scene->Start();
            m_scene->SetSimulationEnabled(true);

            core::ConsoleWrite(u8"WebScene: started (WASD/QE move, hold right-mouse to look).\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, core::f32 deltaTime) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, deltaTime);
            if (m_scene == nullptr)
            {
                return;
            }

#if OPTION_HAS_EXTENSION_IMGUI
            if (auto* g = host.Subsystems().GetSubsystem<extensions::imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildTweakPanel(host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>());
            }
#endif

            m_time += deltaTime;

            // HUD button binding, once the GUI built the canvas's tree: the click counter proves the
            // browser's pointer path routes into the game's GUI and is CONSUMED there.
            if (!m_hudBound)
            {
                if (auto* canvases = m_scene->GetSystem<engine::gui::GuiCanvasComponentManager>())
                {
                    if (auto* canvas = canvases->Get(m_hudEntity); canvas != nullptr && canvas->root)
                    {
                        if (auto* button = core::Cast<gui::Button>(FindById(*canvas->root, u8"ws-btn")))
                        {
                            WebSceneApp* self = this;
                            button->SetOnClick(
                                [self, button]()
                                {
                                    ++self->m_hudClicks;
                                    core::String text(u8"Clicks: ");
                                    text.Append(core::Format(u8"{}", self->m_hudClicks).AsView());
                                    button->SetText(text.AsView());
                                    core::ConsoleWrite(u8"WebScene: HUD button clicked\n");
                                });
                            m_hudBound = true;
                        }
                    }
                }
            }

            // The spinning cube: constant motion so TAA/velocity is always exercised.
            core::Transform ct = m_scene->GetLocalTransform(m_cube);
            ct.rotation = core::Quaternion::FromAxisAngle(core::Float3{0.0f, 1.0f, 0.0f}, m_time) *
                          core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f},
                                                          m_time * 0.35f);
            m_scene->SetLocalTransform(m_cube, ct);

            // The orbiting point light: moving local shadows (the spot stays static).
            const core::f32 orbit = m_time * 0.6f;
            m_scene->SetLocalPosition(m_pointLight,
                                      core::Float3{core::Cos(orbit) * 4.5f, 2.2f,
                                                   core::Sin(orbit) * 4.5f});

            // Free-fly camera (WASD/QE + RMB look) - drives the web input path end to end.
            m_fly.Update(host, deltaTime);
            core::Transform camT = m_scene->GetLocalTransform(m_camera);
            camT.position = m_fly.position;
            camT.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, camT);

            // Navigation crowd: re-target each agent when it arrives so they ping-pong across the
            // field, routing around the obstacle box and avoiding one another (Detour crowd).
            if (auto* agents = m_scene->GetSystem<engine::navigation::NavAgentComponentManager>())
            {
                for (NavAgent& a : m_navAgents)
                {
                    engine::navigation::NavAgentComponent* comp = agents->Get(a.entity);
                    if (comp != nullptr && comp->finishedNav())
                    {
                        a.goingA = !a.goingA;
                        const core::Float3 target = a.goingA ? a.home : a.away;
                        comp->navigate(target.x, target.y, target.z);
                    }
                }
            }

            // Debug draw: immediate-mode, re-issued every frame. The gizmos exercise the 3D pass
            // (lines + bitmap text through the scene camera); the FPS readout stays on regardless
            // and exercises the screen-space pass.
            if (auto* render = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>())
            {
                if (m_showDebugDraw)
                {
                    auto& dbg = render->DebugScene(*m_scene);
                    dbg.DrawGrid(core::Float3{0.0f, 0.01f, 0.0f}, 40.0f, 20,
                                 core::Color{0.25f, 0.25f, 0.30f, 1.0f});
                    dbg.DrawAxis(core::Float4x4::Identity(), 2.0f, /*overlay*/ true);
                    // The reflection probe's box volume + a label - matches BuildProbe exactly, so
                    // the probe's coverage (and the chrome sphere inside it) is visible at a glance.
                    dbg.DrawWireBoxCenter(core::Float3{4.0f, 1.0f, 2.0f},
                                          core::Float3{5.0f, 3.5f, 5.0f},
                                          core::Color{0.2f, 0.9f, 1.0f, 1.0f});
                    dbg.DrawText3D(core::Float3{4.0f, 4.7f, 2.0f}, core::StringView(u8"probe"),
                                   core::Color{0.2f, 0.9f, 1.0f, 1.0f});
                    // The orbiting point light's current position (it moves - a live gizmo).
                    dbg.DrawWireSphere(
                        core::BoundingSphere{m_scene->GetLocalTransform(m_pointLight).position,
                                             0.25f},
                        core::Color{1.0f, 0.8f, 0.2f, 1.0f});
                }
                const core::f32 inst = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
                m_fpsSmoothed =
                    (m_fpsSmoothed > 0.0f) ? (m_fpsSmoothed * 0.9f + inst * 0.1f) : inst;
                const core::f32 ms = (m_fpsSmoothed > 0.0f) ? (1000.0f / m_fpsSmoothed) : 0.0f;
                const int msWhole = static_cast<int>(ms);
                const core::String fpsText = core::Format(
                    u8"{} FPS  {}.{} ms", static_cast<int>(m_fpsSmoothed + 0.5f), msWhole,
                    static_cast<int>((ms - static_cast<core::f32>(msWhole)) * 10.0f + 0.5f));
                render->DebugScreen().DrawScreenText(12.0f, 12.0f, fpsText.AsView(),
                                                     core::Color{1.0f, 1.0f, 0.4f, 1.0f}, 2.0f);
            }
        }

        void OnShutdown(runtime::IApplicationHost& host) override
        {
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                rhi::Device& device = *gfx->Raw();
                device.WaitIdle(); // GPU must finish before freeing the shared texture
                if (m_texView != nullptr)
                {
                    device.DestroyTextureView(m_texView);
                    m_texView = nullptr;
                }
                if (m_tex != nullptr)
                {
                    device.DestroyTexture(m_tex);
                    m_tex = nullptr;
                }
            }
            engine::runtime::DefaultApplication::OnShutdown(host);
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
                {
                    if (engine::render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect = static_cast<core::f32>(frame.width) /
                                      static_cast<core::f32>(frame.height);
                    }
                }
            }
            engine::runtime::DefaultApplication::OnRenderWindow(host, frame);
#if OPTION_HAS_EXTENSION_IMGUI
            // The debug panel draws over the finished scene on the backbuffer.
            if (auto* g = host.Subsystems().GetSubsystem<extensions::imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
#endif
        }

    private:
        // --- scene building ---------------------------------------------------------------

        void BuildEnvironment()
        {
            if (auto* env = m_scene->GetSystem<engine::render::EnvironmentSystem>())
            {
                engine::render::EnvironmentSettings& e = env->Environment();
                e.skyMode = render::SkyMode::Analytic; // Preetham -> IBL bake from the sky
                e.turbidity = 3.0f;
                e.ambientColor = core::Color{0.10f, 0.12f, 0.16f, 1.0f};
                e.ambientIntensity = 0.15f; // mostly IBL ambient; a little flat fill
            }
        }

        void BuildGeometry()
        {
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            // Glossy floor: low roughness so SSR has something to reflect into. Metallic and
            // roughness are LIVE-tweakable from the panel (the SSR eye test).
            m_floor = m_scene->CreateEntity(u8"floor");
            m_scene->SetLocalPosition(m_floor, core::Float3{0.0f, -0.75f, 0.0f});
            engine::render::MeshComponent& fm = meshes->Add(m_floor);
            fm.mesh = geometry::Primitives::Plane(AppRoot(), 30.0f, 30.0f);
            ApplyFloorMaterial();

            // Roughness x metallic sphere grid: the PBR response matrix.
            constexpr core::u32 kCols = 5; // roughness 0..1
            constexpr core::u32 kRows = 2; // dielectric / metal
            for (core::u32 r = 0; r < kRows; ++r)
            {
                for (core::u32 c = 0; c < kCols; ++c)
                {
                    core::String name(u8"web.sphere");
                    scene::EntityHandle e = m_scene->CreateEntity(name.AsView());
                    m_scene->SetLocalPosition(
                        e, core::Float3{-6.0f + static_cast<core::f32>(c) * 1.5f,
                                        0.0f,
                                        -4.0f - static_cast<core::f32>(r) * 1.5f});
                    engine::render::MeshComponent& mc = meshes->Add(e);
                    mc.mesh = geometry::Primitives::Sphere(AppRoot(), 0.6f);
                    const core::f32 rough =
                        0.05f + 0.9f * static_cast<core::f32>(c) / (kCols - 1);
                    mc.SetMaterial(materials::CreatePBR(
                        name.AsView(), core::Float4{0.9f, 0.6f, 0.25f, 1.0f},
                        r == 1 ? 1.0f : 0.0f, rough));
                }
            }

            // The spinning cube (TAA motion) + a chrome sphere for the probe to reflect.
            m_cube = m_scene->CreateEntity(u8"cube");
            m_scene->SetLocalPosition(m_cube, core::Float3{0.0f, 0.6f, 0.0f});
            engine::render::MeshComponent& mc = meshes->Add(m_cube);
            mc.mesh = geometry::Primitives::Cube(AppRoot(), 1.0f);
            mc.SetMaterial(materials::CreatePBR(
                u8"web.cube", core::Float4{0.85f, 0.35f, 0.28f, 1.0f}, 0.1f, 0.4f));

            scene::EntityHandle chrome = m_scene->CreateEntity(u8"chrome");
            m_scene->SetLocalPosition(chrome, core::Float3{4.0f, 0.4f, 2.0f});
            engine::render::MeshComponent& cm = meshes->Add(chrome);
            cm.mesh = geometry::Primitives::Sphere(AppRoot(), 1.1f);
            cm.SetMaterial(materials::CreatePBR(
                u8"web.chrome", core::Float4{0.95f, 0.95f, 0.97f, 1.0f}, 1.0f, 0.05f));
        }

        void BuildLights()
        {
            auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>();
            if (lights == nullptr)
            {
                return;
            }

            // The sun: CSM shadows. Toggleable from the panel - LOCAL shadows must keep
            // working with the sun (and its cascades) off.
            m_sun = m_scene->CreateEntity(u8"sun");
            core::Transform st = m_scene->GetLocalTransform(m_sun);
            st.rotation = core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, -0.9f) *
                          core::Quaternion::FromAxisAngle(core::Float3{0.0f, 1.0f, 0.0f}, 0.5f);
            m_scene->SetLocalTransform(m_sun, st);
            engine::render::LightComponent& sl = lights->Add(m_sun);
            sl.type = engine::render::LightType::Directional;
            sl.color = core::Color{1.0f, 0.95f, 0.9f, 1.0f};
            sl.intensity = 2.0f;
            sl.castsShadows = true;

            // A static spot over the sphere grid (spot shadow tile).
            scene::EntityHandle spot = m_scene->CreateEntity(u8"spot");
            core::Transform spotT = m_scene->GetLocalTransform(spot);
            spotT.position = core::Float3{-3.0f, 5.0f, -2.0f};
            spotT.rotation = core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, -1.2f);
            m_scene->SetLocalTransform(spot, spotT);
            engine::render::LightComponent& spc = lights->Add(spot);
            spc.type = engine::render::LightType::Spot;
            spc.color = core::Color{0.4f, 0.75f, 1.0f, 1.0f};
            spc.intensity = 14.0f;
            spc.range = 14.0f;
            spc.innerAngle = 0.45f;
            spc.outerAngle = 0.62f;
            spc.castsShadows = true;

            // The orbiting point light (six-face point shadows, moving).
            m_pointLight = m_scene->CreateEntity(u8"pointlight");
            m_scene->SetLocalPosition(m_pointLight, core::Float3{4.5f, 2.2f, 0.0f});
            engine::render::LightComponent& pl = lights->Add(m_pointLight);
            pl.type = engine::render::LightType::Point;
            pl.color = core::Color{1.0f, 0.55f, 0.3f, 1.0f};
            pl.intensity = 10.0f;
            pl.range = 9.0f;
            pl.castsShadows = true;
        }

        void BuildProbe()
        {
            if (auto* probes = m_scene->GetSystem<engine::render::ReflectionProbeComponentManager>())
            {
                m_probe = m_scene->CreateEntity(u8"probe");
                m_scene->SetLocalPosition(m_probe, core::Float3{4.0f, 1.0f, 2.0f});
                engine::render::ReflectionProbeComponent& pc = probes->Add(m_probe);
                pc.halfExtents = core::Float3{5.0f, 3.5f, 5.0f};
                pc.resolution = 128;
                pc.parallax = true;
            }
        }

        void BuildInstancedRing()
        {
            auto* instanced = m_scene->GetSystem<engine::render::InstancedMeshComponentManager>();
            if (instanced == nullptr)
            {
                return;
            }
            scene::EntityHandle ring = m_scene->CreateEntity(u8"ring");
            engine::render::InstancedMeshComponent& ic = instanced->Add(ring);
            ic.mesh = geometry::Primitives::Cube(AppRoot(), 0.3f);
            ic.SetMaterial(materials::CreatePBR(
                u8"web.ring", core::Float4{0.3f, 0.8f, 0.5f, 1.0f}, 0.2f, 0.5f));
            ic.instances.Clear();
            constexpr core::u32 kCount = 256;
            for (core::u32 i = 0; i < kCount; ++i)
            {
                const core::f32 a =
                    static_cast<core::f32>(i) / kCount * 6.2831853f;
                const core::f32 radius = 10.0f + 0.8f * core::Sin(a * 9.0f);
                core::Float4x4 m = core::Float4x4::Identity();
                m.m[3][0] = core::Cos(a) * radius;
                m.m[3][1] = -0.4f + 0.5f * core::Sin(a * 5.0f);
                m.m[3][2] = core::Sin(a) * radius;
                ic.instances.PushBack(m);
            }
        }

        void BuildDecalAndSprites()
        {
            if (m_texView == nullptr)
            {
                return; // texture creation failed; decal/sprites just stay absent
            }
            if (auto* decals = m_scene->GetSystem<engine::render::DecalComponentManager>())
            {
                m_decal = m_scene->CreateEntity(u8"decal");
                core::Transform dt = m_scene->GetLocalTransform(m_decal);
                dt.position = core::Float3{-2.5f, -0.2f, 2.5f};
                // Decals project along local +Z; rotate so that axis points DOWN at the floor
                // (an unrotated box projects horizontally and the angle fade removes it).
                dt.rotation =
                    core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, 1.5707963f);
                m_scene->SetLocalTransform(m_decal, dt);
                engine::render::DecalComponent& dc = decals->Add(m_decal);
                dc.texture = m_texView;
                dc.size = core::Float3{3.0f, 2.0f, 3.0f};
                dc.color = core::Color{1.0f, 0.9f, 0.4f, 0.9f};
            }
            if (auto* sprites = m_scene->GetSystem<engine::render::SpriteComponentManager>())
            {
                const core::Float3 base{-6.5f, 1.4f, 3.5f};
                const struct
                {
                    bool additive;
                    bool postTonemap;
                    core::Color tint;
                } kinds[3] = {
                    {false, false, core::Color{1.0f, 1.0f, 1.0f, 0.9f}},
                    {true, false, core::Color{0.4f, 0.8f, 1.0f, 1.0f}},
                    {false, true, core::Color{1.0f, 0.5f, 0.8f, 1.0f}},
                };
                for (core::u32 i = 0; i < 3; ++i)
                {
                    m_sprites[i] = m_scene->CreateEntity(u8"sprite");
                    m_scene->SetLocalPosition(
                        m_sprites[i],
                        base + core::Float3{static_cast<core::f32>(i) * 1.6f, 0.0f, 0.0f});
                    engine::render::SpriteComponent& sc = sprites->Add(m_sprites[i]);
                    sc.texture = m_texView;
                    sc.size = core::Float2{1.2f, 1.2f};
                    sc.tint = kinds[i].tint;
                    sc.additive = kinds[i].additive;
                    sc.postTonemap = kinds[i].postTonemap;
                }
            }
        }

        void BuildParticles()
        {
            auto* pmgr = m_scene->GetSystem<engine::particles::ParticleEffectComponentManager>();
            if (pmgr == nullptr)
            {
                return;
            }

            // A modest additive fountain (billboard path).
            {
                particles::ParticleSystem& sys = m_fountain.AddSystem(6000);
                sys.name = core::String{u8"fountain"};
                sys.blendMode = particles::ParticleBlendMode::Additive;
                sys.renderMode = particles::ParticleRenderMode::Billboard;
                sys.emitter.mode = particles::EmissionMode::Continuous;
                sys.emitter.spawnRate = 900.0f;
                sys.AddInitializer<particles::PositionInitializer>().shape =
                    particles::EmissionShape::Sphere(0.15f);
                sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                    particles::RangeFloat(1.2f, 2.2f);
                {
                    particles::VelocityInitializer& v =
                        sys.AddInitializer<particles::VelocityInitializer>();
                    v.baseVelocity = core::Float3{0.0f, 7.5f, 0.0f};
                    v.randomness = core::Float3{1.6f, 1.0f, 1.6f};
                }
                sys.AddInitializer<particles::SizeInitializer>().size =
                    particles::RangeFloat2::Constant(core::Float2{0.16f, 0.16f});
                sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                    core::Float4{1.0f, 0.6f, 0.2f, 1.0f}, core::Float4{1.0f, 0.85f, 0.4f, 1.0f});
                sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.2f;
                sys.AddBehavior<particles::ColorOverLifetimeBehavior>().curve =
                    particles::ParticleCurveColor::FadeAlpha(core::Float4{1.0f, 0.55f, 0.15f, 1.0f},
                                                             0.35f);
            }
            m_fountainEntity = m_scene->CreateEntity(u8"fountain");
            m_scene->SetLocalPosition(m_fountainEntity, core::Float3{6.5f, -0.6f, -3.5f});
            pmgr->Add(m_fountainEntity).SetEffect(m_fountain);

            // Spark trails (ribbon path).
            {
                particles::ParticleSystem& sys = m_sparks.AddSystem(800);
                sys.name = core::String{u8"sparks"};
                sys.renderMode = particles::ParticleRenderMode::Trail;
                sys.blendMode = particles::ParticleBlendMode::Additive;
                sys.emitter.mode = particles::EmissionMode::Continuous;
                sys.emitter.spawnRate = 24.0f;
                sys.AddInitializer<particles::PositionInitializer>().shape =
                    particles::EmissionShape::Sphere(0.1f);
                sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                    particles::RangeFloat(1.2f, 2.0f);
                {
                    particles::VelocityInitializer& v =
                        sys.AddInitializer<particles::VelocityInitializer>();
                    v.baseVelocity = core::Float3{0.0f, 6.0f, 0.0f};
                    v.randomness = core::Float3{4.0f, 2.0f, 4.0f};
                }
                sys.AddInitializer<particles::SizeInitializer>().size =
                    particles::RangeFloat2::Constant(core::Float2{0.15f, 0.15f});
                sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                    core::Float4{0.2f, 0.7f, 1.0f, 1.0f}, core::Float4{0.9f, 0.4f, 1.0f, 1.0f});
                sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.4f;
            }
            m_sparksEntity = m_scene->CreateEntity(u8"sparks");
            m_scene->SetLocalPosition(m_sparksEntity, core::Float3{-6.5f, -0.5f, -3.5f});
            pmgr->Add(m_sparksEntity).SetEffect(m_sparks);
        }

        void BuildCamera()
        {
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                engine::render::CameraComponent& cam = cameras->Add(m_camera);
                cam.fovYRadians = 1.04719755f; // 60 deg
                cam.nearZ = 0.1f;
                cam.farZ = 200.0f;
                cam.clearColor = core::Color{0.05f, 0.06f, 0.09f, 1.0f};
            }
            m_fly.position = core::Float3{0.0f, 2.4f, 9.5f};
            m_fly.yaw = 0.0f;
            m_fly.pitch = -0.22f;
            m_fly.moveSpeed = 5.0f;
            m_fly.fastSpeed = 14.0f;
            m_fly.focusDistance = 9.0f;
            core::Transform ct = m_scene->GetLocalTransform(m_camera);
            ct.position = m_fly.position;
            ct.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, ct);
        }

        // Game UI on the engine's GUI, all three faces of it (documents made in code, styled
        // through the markup's inline CSS):
        //   - scene tier:  a HUD canvas (title + controls + a click-counter Button)
        //   - billboard:   a nameplate riding the spinning cube (distance-scaled)
        //   - screen tier: a passive bottom-right badge on the overlay layer
        void BuildGameUI(runtime::IApplicationHost& host)
        {
            // Scene-tier HUD canvas.
            m_hudDocument = core::MakeRef<gui::GuiDocument>(AppRoot());
            m_hudDocument->markup = core::String(
                u8"<Panel flex-direction=\"column\" align-items=\"start\" padding=\"12\">"
                u8"  <Panel flex-direction=\"column\" gap=\"8\" padding=\"12\" width=\"240\""
                u8"         background-color=\"rgb(28, 32, 40)\" border-radius=\"8\">"
                u8"    <Label text=\"WebScene\" font-size=\"18\"/>"
                u8"    <Label text=\"WASD/QE move - RMB look\" font-size=\"12\"/>"
                u8"    <Button id=\"ws-btn\" text=\"Clicks: 0\" width=\"200\" height=\"36\"/>"
                u8"  </Panel>"
                u8"</Panel>");
            m_hudEntity = m_scene->CreateEntity(u8"hud");
            if (auto* canvases = m_scene->GetSystem<engine::gui::GuiCanvasComponentManager>())
                canvases->Add(m_hudEntity).document = m_hudDocument;

            // Billboard nameplate on the spinning cube (the moving anchor makes the
            // world-tracking path obvious at a glance).
            m_plateDocument = core::MakeRef<gui::GuiDocument>(AppRoot());
            m_plateDocument->markup = core::String(
                u8"<Panel padding=\"4\" background-color=\"rgb(20, 24, 30)\" border-radius=\"4\">"
                u8"  <Label text=\"cube\" font-size=\"13\"/>"
                u8"</Panel>");
            if (auto* billboards = m_scene->GetSystem<engine::gui::GuiBillboardComponentManager>())
            {
                engine::gui::GuiBillboardComponent& plate = billboards->Add(m_cube);
                plate.document = m_plateDocument;
                plate.offset = core::Float3{0.0f, 1.2f, 0.0f};
                plate.scaleMode = engine::gui::GuiBillboardScale::Distance;
                plate.referenceDistance = 12.0f;
            }

            // Screen-tier badge: OUTSIDE any scene (it survives scene swaps), in the bottom-right
            // corner. Passive (pointer-events none on its root): an overlay that takes the pointer
            // is modal, which would shield the scene's HUD from every click.
            if (auto* gameGui = host.Subsystems().GetSubsystem<engine::gui::GuiSubsystem>())
            {
                m_badgeDocument = core::MakeRef<gui::GuiDocument>(AppRoot());
                m_badgeDocument->markup = core::String(
                    u8"<Panel pointer-events=\"none\" justify-content=\"end\" align-items=\"end\" padding=\"12\">"
                    u8"  <Panel padding=\"6\" background-color=\"rgb(20, 24, 30)\" border-radius=\"6\">"
                    u8"    <Label text=\"screen tier\" font-size=\"11\"/>"
                    u8"  </Panel>"
                    u8"</Panel>");
                m_badge = gameGui->PushOverlay(*m_badgeDocument);
            }
        }

        // The node under `node` with `id` (a GUI tree from markup), or null.
        [[nodiscard]] static gui::Node* FindById(gui::Node& node, core::StringView id)
        {
            if (const auto* widget = core::Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (core::usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindById(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        // Swap in a freshly built floor material for the current tweak values. The mesh
        // renderer keys material instances by UID and prunes unreferenced ones, so material
        // replacement is the clean live-tweak path.
        void ApplyFloorMaterial()
        {
            if (auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>())
            {
                if (engine::render::MeshComponent* fm = meshes->Get(m_floor))
                {
                    fm->SetMaterial(materials::CreatePBR(u8"web.floor",
                                                         core::Float4{0.28f, 0.29f, 0.33f, 1.0f},
                                                         m_floorMetallic, m_floorRoughness));
                }
            }
        }

        // One shared procedural texture (a soft ring on a checker) for the decal + sprites,
        // uploaded through the transfer batch (the ParticleRenderer white-dot pattern).
        void BuildTexture(runtime::IApplicationHost& host)
        {
            auto* gfx = host.Graphics();
            rhi::Device* device = gfx != nullptr ? gfx->Raw() : nullptr;
            if (device == nullptr)
            {
                return;
            }
            constexpr core::u32 kSize = 64;
            static core::u8 pixels[kSize * kSize * 4];
            for (core::u32 y = 0; y < kSize; ++y)
            {
                for (core::u32 x = 0; x < kSize; ++x)
                {
                    const core::f32 fx = (static_cast<core::f32>(x) + 0.5f) / kSize - 0.5f;
                    const core::f32 fy = (static_cast<core::f32>(y) + 0.5f) / kSize - 0.5f;
                    const core::f32 d = core::Sqrt(fx * fx + fy * fy);
                    const bool check = (((x / 8) + (y / 8)) & 1u) != 0u;
                    const core::f32 ring =
                        core::Clamp(1.0f - core::Abs(d - 0.32f) * 12.0f, 0.0f, 1.0f);
                    const core::u8 base = check ? 200 : 90;
                    core::u8* p = &pixels[(y * kSize + x) * 4];
                    p[0] = static_cast<core::u8>(core::Min(255.0f, base + ring * 255.0f));
                    p[1] = static_cast<core::u8>(core::Min(255.0f, base * 0.8f + ring * 200.0f));
                    p[2] = static_cast<core::u8>(base / 2);
                    const core::f32 alpha = core::Clamp(ring + (check ? 0.55f : 0.25f), 0.0f, 1.0f);
                    p[3] = static_cast<core::u8>(alpha * 255.0f);
                }
            }

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"webscene.tex";
            if (!device->CreateTexture(td, m_tex).IsOk())
            {
                return;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            if (!device->CreateTextureView(m_tex, vd, m_texView).IsOk())
            {
                m_texView = nullptr;
                return;
            }
            if (rhi::Queue* q = device->GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* tb = nullptr;
                if (q->CreateTransferBatch(tb).IsOk() && tb != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = kSize * 4;
                    layout.rowsPerImage = kSize;
                    tb->WriteTexture(m_tex, core::Span<const core::u8>{pixels, sizeof(pixels)},
                                     layout, rhi::Extent3D{kSize, kSize, 1});
                    (void)tb->Submit();
                    q->DestroyTransferBatch(tb);
                }
            }
        }

        // --- the tweak panel --------------------------------------------------------------

#if OPTION_HAS_EXTENSION_IMGUI
        void BuildTweakPanel(engine::render::RenderSubsystem* renderSub)
        {
            if (renderSub == nullptr || m_scene == nullptr)
            {
                return;
            }
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(330, 460), ImGuiCond_FirstUseEver);
            ImGui::Begin("WebScene");

            if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
            {
                float exposure = renderSub->Exposure();
                if (ImGui::SliderFloat("Exposure", &exposure, 0.05f, 8.0f))
                {
                    renderSub->SetExposure(exposure);
                }
                if (auto* env = m_scene->GetSystem<engine::render::EnvironmentSystem>())
                {
                    int mode = static_cast<int>(env->Environment().skyMode);
                    const char* modes[] = {"Color", "Procedural", "HDR", "Cubemap", "Analytic"};
                    const int count = static_cast<int>(sizeof(modes) / sizeof(modes[0]));
                    if (ImGui::Combo("Sky mode", &mode, modes, count))
                    {
                        env->Environment().skyMode = static_cast<render::SkyMode>(mode);
                    }
                }
            }

            if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>())
                {
                    if (engine::render::LightComponent* sun = lights->Get(m_sun))
                    {
                        // THE regression case: local (spot/point) shadows must be correct
                        // with the sun - and with it, the CSM cascades - disabled.
                        ImGui::Checkbox("Sun enabled", &sun->enabled);
                        ImGui::Checkbox("Sun shadows (CSM)", &sun->castsShadows);
                        ImGui::SliderFloat("Sun intensity", &sun->intensity, 0.0f, 6.0f);
                    }
                }
            }

            if (ImGui::CollapsingHeader("Post", ImGuiTreeNodeFlags_DefaultOpen))
            {
                bool taa = renderSub->TaaEnabled();
                if (ImGui::Checkbox("TAA", &taa))
                {
                    renderSub->SetTaaEnabled(taa);
                }
                bool fxaa = renderSub->FxaaEnabled();
                if (ImGui::Checkbox("FXAA (TAA-off fallback)", &fxaa))
                {
                    renderSub->SetFxaaEnabled(fxaa);
                }
                bool ssr = renderSub->SsrEnabled();
                if (ImGui::Checkbox("SSR", &ssr))
                {
                    renderSub->SetSsrEnabled(ssr);
                }
                // Scene-pass MSAA (independent of TAA - both can be on). Levels + labels come from
                // engine::render::kMsaaLevels (the single source of truth; add 8x there and it shows
                // here). Offer ONLY device-supported counts: the valid set is not contiguous - WebGPU
                // has no 2x - so on web this shows just "Off" and "4x".
                core::u32 msaaCounts[engine::render::MsaaLevelCount()] = {};
                const char* msaaItems[engine::render::MsaaLevelCount()] = {};
                int msaaN = 0;
                int msaaIdx = 0;
                for (core::u32 i = 0; i < engine::render::MsaaLevelCount(); ++i)
                {
                    const core::u32 count = engine::render::kMsaaLevels[i].samples;
                    if (!renderSub->SupportsMsaaSamples(count))
                    {
                        continue;
                    }
                    if (renderSub->MsaaSamples() >= count)
                    {
                        msaaIdx = msaaN; // select the highest supported count <= the current setting
                    }
                    msaaCounts[msaaN] = count;
                    // The labels are compile-time literals (null-terminated), safe as ImGui C strings.
                    msaaItems[msaaN] =
                        reinterpret_cast<const char*>(engine::render::kMsaaLevels[i].label.Data());
                    ++msaaN;
                }
                if (msaaN > 0 && ImGui::Combo("MSAA", &msaaIdx, msaaItems, msaaN))
                {
                    renderSub->SetMsaaSamples(msaaCounts[msaaIdx]);
                }
                // The SSR eye test wants a tunable reflector (roughness feeds the SSR
                // cutoff/cone-gather; metallic drives reflectivity).
                bool floorChanged = false;
                floorChanged |= ImGui::SliderFloat("Floor Metallic", &m_floorMetallic, 0.0f, 1.0f);
                floorChanged |=
                    ImGui::SliderFloat("Floor Roughness", &m_floorRoughness, 0.0f, 1.0f);
                if (floorChanged)
                {
                    ApplyFloorMaterial();
                }
                int aoMode = static_cast<int>(renderSub->GetAoMode());
                const char* aoItems[] = {"Off", "SSAO", "GTAO"};
                if (ImGui::Combo("AO", &aoMode, aoItems, 3))
                {
                    renderSub->SetAoMode(static_cast<render::AoMode>(aoMode));
                }
                float bloom = renderSub->BloomIntensity();
                if (ImGui::SliderFloat("Bloom", &bloom, 0.0f, 1.5f))
                {
                    renderSub->SetBloomIntensity(bloom);
                }
            }

            if (ImGui::CollapsingHeader("Features"))
            {
                if (auto* decals = m_scene->GetSystem<engine::render::DecalComponentManager>())
                {
                    if (engine::render::DecalComponent* dc = decals->Get(m_decal))
                    {
                        ImGui::Checkbox("Decal", &dc->visible);
                    }
                }
                ImGui::Checkbox("Debug draw (gizmos)", &m_showDebugDraw);
                if (auto* probes = m_scene->GetSystem<engine::render::ReflectionProbeComponentManager>())
                {
                    if (engine::render::ReflectionProbeComponent* pc = probes->Get(m_probe))
                    {
                        // Diagnostic for the web black-probe hunt: Realtime re-captures every
                        // frame, separating "capture path broken" from "startup result lost".
                        bool realtime = pc->update == render::ProbeUpdateMode::Realtime;
                        if (ImGui::Checkbox("Probe realtime re-capture", &realtime))
                        {
                            pc->update = realtime ? render::ProbeUpdateMode::Realtime
                                                  : render::ProbeUpdateMode::Static;
                        }
                    }
                }
                if (auto* sprites = m_scene->GetSystem<engine::render::SpriteComponentManager>())
                {
                    if (engine::render::SpriteComponent* sc = sprites->Get(m_sprites[0]))
                    {
                        bool on = sc->visible;
                        if (ImGui::Checkbox("Sprites", &on))
                        {
                            for (const scene::EntityHandle& h : m_sprites)
                            {
                                if (engine::render::SpriteComponent* s = sprites->Get(h))
                                {
                                    s->visible = on;
                                }
                            }
                        }
                    }
                }
            }

            ImGui::Text("%.2f ms (%.0f fps)", 1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);
            ImGui::End();
        }
#endif

        // --- navigation: an inline-baked navmesh + a small avoiding crowd -------------------------
        // The navmesh is baked HERE at startup from the floor + an obstacle box (the runtime bake
        // primitive, foundation.navigation - the editor/cook bake is tools-only and absent on web),
        // so this same code path runs on desktop AND in the browser. The zone binds the baked
        // product directly (no content DB); agents cross the field routing around the box.
        static void AddGroundSoup(core::Array<core::Float3>& verts, core::Array<core::u32>& indices,
                                  core::f32 y, core::f32 minX, core::f32 maxX, core::f32 minZ,
                                  core::f32 maxZ)
        {
            const core::u32 base = static_cast<core::u32>(verts.Size());
            verts.PushBack(core::Float3{minX, y, minZ});
            verts.PushBack(core::Float3{maxX, y, minZ});
            verts.PushBack(core::Float3{maxX, y, maxZ});
            verts.PushBack(core::Float3{minX, y, maxZ});
            const core::u32 t[] = {base + 0, base + 3, base + 2, base + 0, base + 2, base + 1};
            for (core::u32 i : t)
            {
                indices.PushBack(i);
            }
        }
        static void AddBoxSoup(core::Array<core::Float3>& verts, core::Array<core::u32>& indices,
                               core::f32 cx, core::f32 baseY, core::f32 cz, core::f32 half,
                               core::f32 height)
        {
            auto quad = [&](core::Float3 a, core::Float3 b, core::Float3 c, core::Float3 d)
            {
                const core::u32 base = static_cast<core::u32>(verts.Size());
                verts.PushBack(a);
                verts.PushBack(b);
                verts.PushBack(c);
                verts.PushBack(d);
                const core::u32 t[] = {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3};
                for (core::u32 i : t)
                {
                    indices.PushBack(i);
                }
            };
            const core::f32 x0 = cx - half, x1 = cx + half, z0 = cz - half, z1 = cz + half;
            const core::f32 y0 = baseY, y1 = baseY + height;
            quad({x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}); // top
            quad({x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0});
            quad({x1, y0, z1}, {x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1});
            quad({x0, y0, z1}, {x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1});
            quad({x1, y0, z0}, {x1, y0, z1}, {x1, y1, z1}, {x1, y1, z0});
        }

        void BuildNavigation()
        {
            const core::f32 floorY = -0.75f; // the floor plane's height

            // Bake: the floor minus a central obstacle box (the agents route around it - it sits
            // under the spinning cube at the origin, so the detour reads against a visible object).
            core::Array<core::Float3> verts;
            core::Array<core::u32> indices;
            AddGroundSoup(verts, indices, floorY, -13.0f, 13.0f, -13.0f, 13.0f);
            AddBoxSoup(verts, indices, 0.0f, floorY, 0.0f, 1.6f, 2.5f);
            core::Array<core::byte> blob;
            const bool ok = navigation::NavigationMeshBuilder::Build(
                                core::Span<const core::Float3>{verts.Data(), verts.Size()},
                                core::Span<const core::u32>{indices.Data(), indices.Size()},
                                navigation::NavigationBakeParams{}, blob)
                                .IsOk();
            m_navZone = core::MakeRef<navigation::NavigationZoneResource>(AppRoot(),
                                                                           AppRoot());
            if (ok && !blob.IsEmpty())
            {
                (void)m_navZone->mesh.Load(
                    core::Span<const core::byte>{blob.Data(), blob.Size()});
            }

            // Overlay the navmesh surface (cyan) + agent target lines (yellow).
            if (auto* navSys = m_scene->GetSystem<engine::navigation::NavigationSceneSystem>())
            {
                navSys->Settings().debugDraw = true;
                navSys->Settings().debugDrawPaths = true;
            }

            // The zone entity (origin, identity - so zone-local == world) binding the baked navmesh.
            {
                scene::EntityHandle zoneEntity = m_scene->CreateEntity(u8"nav-zone");
                engine::navigation::NavMeshZoneComponent& zone =
                    m_scene->GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(
                        zoneEntity);
                zone.extents = core::Float3{14.0f, 4.0f, 14.0f};
                zone.zone = m_navZone; // implicit RefPtr -> Ref (like mesh = Primitives::...)
            }

            // Six agents (small spheres): three lanes each direction, crossing the field.
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            auto* agentMgr = m_scene->GetSystem<engine::navigation::NavAgentComponentManager>();
            const core::f32 lanes[] = {-6.0f, 0.0f, 6.0f};
            for (core::f32 lane : lanes)
            {
                SpawnNavAgent(*meshes, *agentMgr, core::Float3{-11.0f, floorY, lane},
                              core::Float3{11.0f, floorY, lane});
                SpawnNavAgent(*meshes, *agentMgr, core::Float3{11.0f, floorY, -lane},
                              core::Float3{-11.0f, floorY, -lane});
            }
        }

        void SpawnNavAgent(engine::render::MeshComponentManager& meshes,
                           engine::navigation::NavAgentComponentManager& agentMgr,
                           core::Float3 home, core::Float3 away)
        {
            scene::EntityHandle e = m_scene->CreateEntity(u8"nav-agent");
            m_scene->SetLocalPosition(e, home);
            meshes.Add(e).mesh = geometry::Primitives::Sphere(AppRoot(), 0.5f);
            agentMgr.Add(e); // defaults: radius 0.6, maxSpeed 3.5, MoveEntity
            m_navAgents.PushBack(NavAgent{e, home, away, false});
        }

        struct NavAgent
        {
            scene::EntityHandle entity;
            core::Float3 home;
            core::Float3 away;
            bool goingA = false;
        };

        scene::Scene* m_scene = nullptr;
        core::RefPtr<navigation::NavigationZoneResource> m_navZone; // the inline-baked navmesh (bound to the zone)
        core::Array<NavAgent> m_navAgents;
        scene::EntityHandle m_cube{};
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_sun{};
        scene::EntityHandle m_pointLight{};
        scene::EntityHandle m_floor{};
        scene::EntityHandle m_decal{};
        scene::EntityHandle m_probe{};
        bool m_showDebugDraw = true;
        core::f32 m_fpsSmoothed = 0.0f;
        scene::EntityHandle m_sprites[3] = {};
        scene::EntityHandle m_fountainEntity{};
        scene::EntityHandle m_sparksEntity{};
        particles::ParticleEffect m_fountain;
        particles::ParticleEffect m_sparks;
        rhi::Texture* m_tex = nullptr; // freed in OnShutdown (validation-clean teardown)
        rhi::TextureView* m_texView = nullptr;
        FlyCamera m_fly;
        core::f32 m_time = 0.0f;
        core::f32 m_floorMetallic = 0.0f;  // floor material tweakables (SSR eye test)
        core::f32 m_floorRoughness = 0.12f;
        // Game UI (the documents keep their markup alive; the badge kept to take off again).
        core::RefPtr<gui::GuiDocument> m_hudDocument;
        core::RefPtr<gui::GuiDocument> m_plateDocument;
        core::RefPtr<gui::GuiDocument> m_badgeDocument;
        core::RefPtr<gui::Node> m_badge;
        scene::EntityHandle m_hudEntity{};
        core::u32 m_hudClicks = 0;
        bool m_hudBound = false;
    };
}

#endif // SAMPLES_WEBSCENE_APP_H
