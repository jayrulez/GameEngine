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
//   - an ImGui panel (when the extension is available - desktop and, once climbed, web) tweaking
//     exposure/sky/post (TAA/FXAA/bloom/AO/SSR) and the feature toggles
//   - debug draw (grid/axes/wire volumes/3D text via DebugScene + FPS overlay via DebugScreen)
//   - game UI: a scene-tier HUD canvas (click-counter button = pointer consumption), a billboard
//     nameplate riding the spinning cube, and a screen-tier badge (PushScreenOverlay) - together
//     they exercise both overlay passes (and their stencil attachments) + the bundled font
//
// Not exercised yet: skinning (needs a skinned asset - procedural skinned content is its own task)
// and multi-view split-screen (Sandbox covers it; this scene stays single-view light).
#ifndef DRACONIC_SAMPLES_WEBSCENE_APP_H
#define DRACONIC_SAMPLES_WEBSCENE_APP_H

#include "../Common/FlyCamera.h" // shared free-fly camera (WASD/QE + RMB-look)

#if DRACONIC_HAS_EXTENSION_IMGUI
#include "imgui.h"
#endif

namespace draconic::samples
{
    namespace foundation = draconic::foundation;
    namespace runtime = draconic::runtime;
    namespace graphics = draconic::graphics;
    namespace scene = draconic::scene;
    namespace render = draconic::render;
    namespace geometry = draconic::geometry;
    namespace materials = draconic::materials;
    namespace particles = draconic::particles;
    namespace ui = draconic::ui;
    namespace rhi = draconic::rhi;

    class WebSceneApp : public runtime::DefaultApplication
    {
    public:
#if DRACONIC_HAS_EXTENSION_IMGUI
        void Configure(runtime::IApplicationHost& host) override
        {
            runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<draconic::imgui::ImguiSubsystem>(*gfx->Raw(),
                                                                         gfx->FramesInFlight());
            }
        }
#endif

        void OnStartup(runtime::IApplicationHost& host) override
        {
            // The base wires resource-type registration AND the game-UI render bring-up
            // (UISubsystem::EnsureRenderReady) - skip it and UI silently never draws.
            runtime::DefaultApplication::OnStartup(host);
            foundation::ConsoleWrite(u8"WebScene: building the full-renderer scene...\n");
            if (host.Ctx().GetSubsystem<scene::SceneSubsystem>() == nullptr)
            {
                foundation::ConsoleWrite(u8"WebScene: no scene subsystem.\n");
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"web");

            // The exercise scene turns the optional passes ON by default - it exists to
            // exercise them (the panel can toggle everything off).
            if (auto* renderSub = host.Ctx().GetSubsystem<render::RenderSubsystem>())
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

            foundation::ConsoleWrite(u8"WebScene: started (WASD/QE move, hold right-mouse to look).\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, foundation::f32 deltaTime) override
        {
            runtime::DefaultApplication::OnUpdate(host, deltaTime);
            if (m_scene == nullptr)
            {
                return;
            }

#if DRACONIC_HAS_EXTENSION_IMGUI
            if (auto* g = host.Ctx().GetSubsystem<draconic::imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildTweakPanel(host.Ctx().GetSubsystem<render::RenderSubsystem>());
            }
#endif

            m_time += deltaTime;

            // HUD button binding, once the UI subsystem instantiated the canvas tree
            // (the PhysicsPlayground pattern): the click counter proves the browser's
            // pointer path routes into game UI and is CONSUMED there.
            if (!m_hudBound)
            {
                if (auto* canvases = m_scene->GetSystem<ui::UICanvasComponentManager>())
                {
                    if (auto* canvas = canvases->Get(m_hudEntity);
                        canvas != nullptr && canvas->root.Get() != nullptr)
                    {
                        if (auto* button = foundation::Cast<ui::ViewGroup>(canvas->root.Get())
                                               ->FindByName<ui::Button>(u8"ws-btn"))
                        {
                            WebSceneApp* self = this;
                            ui::Button* raw = button;
                            button->OnClick.Add(
                                [self, raw](ui::ButtonBase*)
                                {
                                    ++self->m_hudClicks;
                                    foundation::String text(u8"Clicks: ");
                                    const foundation::u32 n = self->m_hudClicks;
                                    if (n >= 10)
                                    {
                                        text.PushBack(
                                            static_cast<foundation::utf8char>('0' + n / 10 % 10));
                                    }
                                    text.PushBack(static_cast<foundation::utf8char>('0' + n % 10));
                                    raw->SetText(text.AsView());
                                    foundation::ConsoleWrite(u8"WebScene: HUD button clicked\n");
                                });
                            m_hudBound = true;
                        }
                    }
                }
            }

            // The spinning cube: constant motion so TAA/velocity is always exercised.
            foundation::Transform ct = m_scene->GetLocalTransform(m_cube);
            ct.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{0.0f, 1.0f, 0.0f}, m_time) *
                          foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f},
                                                          m_time * 0.35f);
            m_scene->SetLocalTransform(m_cube, ct);

            // The orbiting point light: moving local shadows (the spot stays static).
            const foundation::f32 orbit = m_time * 0.6f;
            m_scene->SetLocalPosition(m_pointLight,
                                      foundation::Float3{foundation::Cos(orbit) * 4.5f, 2.2f,
                                                   foundation::Sin(orbit) * 4.5f});

            // Free-fly camera (WASD/QE + RMB look) - drives the web input path end to end.
            m_fly.Update(host, deltaTime);
            foundation::Transform camT = m_scene->GetLocalTransform(m_camera);
            camT.position = m_fly.position;
            camT.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, camT);

            // Debug draw: immediate-mode, re-issued every frame. The gizmos exercise the 3D pass
            // (lines + bitmap text through the scene camera); the FPS readout stays on regardless
            // and exercises the screen-space pass.
            if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
            {
                if (m_showDebugDraw)
                {
                    auto& dbg = render->DebugScene(*m_scene);
                    dbg.DrawGrid(foundation::Float3{0.0f, 0.01f, 0.0f}, 40.0f, 20,
                                 foundation::Color{0.25f, 0.25f, 0.30f, 1.0f});
                    dbg.DrawAxis(foundation::Float4x4::Identity(), 2.0f, /*overlay*/ true);
                    // The reflection probe's box volume + a label - matches BuildProbe exactly, so
                    // the probe's coverage (and the chrome sphere inside it) is visible at a glance.
                    dbg.DrawWireBoxCenter(foundation::Float3{4.0f, 1.0f, 2.0f},
                                          foundation::Float3{5.0f, 3.5f, 5.0f},
                                          foundation::Color{0.2f, 0.9f, 1.0f, 1.0f});
                    dbg.DrawText3D(foundation::Float3{4.0f, 4.7f, 2.0f}, foundation::StringView(u8"probe"),
                                   foundation::Color{0.2f, 0.9f, 1.0f, 1.0f});
                    // The orbiting point light's current position (it moves - a live gizmo).
                    dbg.DrawWireSphere(
                        foundation::BoundingSphere{m_scene->GetLocalTransform(m_pointLight).position,
                                             0.25f},
                        foundation::Color{1.0f, 0.8f, 0.2f, 1.0f});
                }
                const foundation::f32 inst = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
                m_fpsSmoothed =
                    (m_fpsSmoothed > 0.0f) ? (m_fpsSmoothed * 0.9f + inst * 0.1f) : inst;
                const foundation::f32 ms = (m_fpsSmoothed > 0.0f) ? (1000.0f / m_fpsSmoothed) : 0.0f;
                const int msWhole = static_cast<int>(ms);
                const foundation::String fpsText = foundation::Format(
                    u8"{} FPS  {}.{} ms", static_cast<int>(m_fpsSmoothed + 0.5f), msWhole,
                    static_cast<int>((ms - static_cast<foundation::f32>(msWhole)) * 10.0f + 0.5f));
                render->DebugScreen().DrawScreenText(12.0f, 12.0f, fpsText.AsView(),
                                                     foundation::Color{1.0f, 1.0f, 0.4f, 1.0f}, 2.0f);
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
            runtime::DefaultApplication::OnShutdown(host);
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
                {
                    if (render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect = static_cast<foundation::f32>(frame.width) /
                                      static_cast<foundation::f32>(frame.height);
                    }
                }
            }
            runtime::DefaultApplication::OnRenderWindow(host, frame);
#if DRACONIC_HAS_EXTENSION_IMGUI
            // The debug panel draws over the finished scene on the backbuffer.
            if (auto* g = host.Ctx().GetSubsystem<draconic::imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
#endif
        }

    private:
        // --- scene building ---------------------------------------------------------------

        void BuildEnvironment()
        {
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                render::EnvironmentSettings& e = env->Environment();
                e.skyMode = render::SkyMode::Analytic; // Preetham -> IBL bake from the sky
                e.turbidity = 3.0f;
                e.ambientColor = foundation::Color{0.10f, 0.12f, 0.16f, 1.0f};
                e.ambientIntensity = 0.15f; // mostly IBL ambient; a little flat fill
            }
        }

        void BuildGeometry()
        {
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            // Glossy floor: low roughness so SSR has something to reflect into. Metallic and
            // roughness are LIVE-tweakable from the panel (the SSR eye test).
            m_floor = m_scene->CreateEntity(u8"floor");
            m_scene->SetLocalPosition(m_floor, foundation::Float3{0.0f, -0.75f, 0.0f});
            render::MeshComponent& fm = meshes->Add(m_floor);
            fm.mesh = geometry::Primitives::Plane(30.0f, 30.0f);
            ApplyFloorMaterial();

            // Roughness x metallic sphere grid: the PBR response matrix.
            constexpr foundation::u32 kCols = 5; // roughness 0..1
            constexpr foundation::u32 kRows = 2; // dielectric / metal
            for (foundation::u32 r = 0; r < kRows; ++r)
            {
                for (foundation::u32 c = 0; c < kCols; ++c)
                {
                    foundation::String name(u8"web.sphere");
                    scene::EntityHandle e = m_scene->CreateEntity(name.AsView());
                    m_scene->SetLocalPosition(
                        e, foundation::Float3{-6.0f + static_cast<foundation::f32>(c) * 1.5f,
                                        0.0f,
                                        -4.0f - static_cast<foundation::f32>(r) * 1.5f});
                    render::MeshComponent& mc = meshes->Add(e);
                    mc.mesh = geometry::Primitives::Sphere(0.6f);
                    const foundation::f32 rough =
                        0.05f + 0.9f * static_cast<foundation::f32>(c) / (kCols - 1);
                    mc.SetMaterial(materials::CreatePBR(
                        name.AsView(), foundation::Float4{0.9f, 0.6f, 0.25f, 1.0f},
                        r == 1 ? 1.0f : 0.0f, rough));
                }
            }

            // The spinning cube (TAA motion) + a chrome sphere for the probe to reflect.
            m_cube = m_scene->CreateEntity(u8"cube");
            m_scene->SetLocalPosition(m_cube, foundation::Float3{0.0f, 0.6f, 0.0f});
            render::MeshComponent& mc = meshes->Add(m_cube);
            mc.mesh = geometry::Primitives::Cube(1.0f);
            mc.SetMaterial(materials::CreatePBR(
                u8"web.cube", foundation::Float4{0.85f, 0.35f, 0.28f, 1.0f}, 0.1f, 0.4f));

            scene::EntityHandle chrome = m_scene->CreateEntity(u8"chrome");
            m_scene->SetLocalPosition(chrome, foundation::Float3{4.0f, 0.4f, 2.0f});
            render::MeshComponent& cm = meshes->Add(chrome);
            cm.mesh = geometry::Primitives::Sphere(1.1f);
            cm.SetMaterial(materials::CreatePBR(
                u8"web.chrome", foundation::Float4{0.95f, 0.95f, 0.97f, 1.0f}, 1.0f, 0.05f));
        }

        void BuildLights()
        {
            auto* lights = m_scene->GetSystem<render::LightComponentManager>();
            if (lights == nullptr)
            {
                return;
            }

            // The sun: CSM shadows. Toggleable from the panel - LOCAL shadows must keep
            // working with the sun (and its cascades) off.
            m_sun = m_scene->CreateEntity(u8"sun");
            foundation::Transform st = m_scene->GetLocalTransform(m_sun);
            st.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -0.9f) *
                          foundation::Quaternion::FromAxisAngle(foundation::Float3{0.0f, 1.0f, 0.0f}, 0.5f);
            m_scene->SetLocalTransform(m_sun, st);
            render::LightComponent& sl = lights->Add(m_sun);
            sl.type = render::LightType::Directional;
            sl.color = foundation::Color{1.0f, 0.95f, 0.9f, 1.0f};
            sl.intensity = 2.0f;
            sl.castsShadows = true;

            // A static spot over the sphere grid (spot shadow tile).
            scene::EntityHandle spot = m_scene->CreateEntity(u8"spot");
            foundation::Transform spotT = m_scene->GetLocalTransform(spot);
            spotT.position = foundation::Float3{-3.0f, 5.0f, -2.0f};
            spotT.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -1.2f);
            m_scene->SetLocalTransform(spot, spotT);
            render::LightComponent& spc = lights->Add(spot);
            spc.type = render::LightType::Spot;
            spc.color = foundation::Color{0.4f, 0.75f, 1.0f, 1.0f};
            spc.intensity = 14.0f;
            spc.range = 14.0f;
            spc.innerAngle = 0.45f;
            spc.outerAngle = 0.62f;
            spc.castsShadows = true;

            // The orbiting point light (six-face point shadows, moving).
            m_pointLight = m_scene->CreateEntity(u8"pointlight");
            m_scene->SetLocalPosition(m_pointLight, foundation::Float3{4.5f, 2.2f, 0.0f});
            render::LightComponent& pl = lights->Add(m_pointLight);
            pl.type = render::LightType::Point;
            pl.color = foundation::Color{1.0f, 0.55f, 0.3f, 1.0f};
            pl.intensity = 10.0f;
            pl.range = 9.0f;
            pl.castsShadows = true;
        }

        void BuildProbe()
        {
            if (auto* probes = m_scene->GetSystem<render::ReflectionProbeComponentManager>())
            {
                m_probe = m_scene->CreateEntity(u8"probe");
                m_scene->SetLocalPosition(m_probe, foundation::Float3{4.0f, 1.0f, 2.0f});
                render::ReflectionProbeComponent& pc = probes->Add(m_probe);
                pc.halfExtents = foundation::Float3{5.0f, 3.5f, 5.0f};
                pc.resolution = 128;
                pc.parallax = true;
            }
        }

        void BuildInstancedRing()
        {
            auto* instanced = m_scene->GetSystem<render::InstancedMeshComponentManager>();
            if (instanced == nullptr)
            {
                return;
            }
            scene::EntityHandle ring = m_scene->CreateEntity(u8"ring");
            render::InstancedMeshComponent& ic = instanced->Add(ring);
            ic.mesh = geometry::Primitives::Cube(0.3f);
            ic.material = materials::CreatePBR(
                u8"web.ring", foundation::Float4{0.3f, 0.8f, 0.5f, 1.0f}, 0.2f, 0.5f);
            ic.instances.Clear();
            constexpr foundation::u32 kCount = 256;
            for (foundation::u32 i = 0; i < kCount; ++i)
            {
                const foundation::f32 a =
                    static_cast<foundation::f32>(i) / kCount * 6.2831853f;
                const foundation::f32 radius = 10.0f + 0.8f * foundation::Sin(a * 9.0f);
                foundation::Float4x4 m = foundation::Float4x4::Identity();
                m.m[3][0] = foundation::Cos(a) * radius;
                m.m[3][1] = -0.4f + 0.5f * foundation::Sin(a * 5.0f);
                m.m[3][2] = foundation::Sin(a) * radius;
                ic.instances.PushBack(m);
            }
        }

        void BuildDecalAndSprites()
        {
            if (m_texView == nullptr)
            {
                return; // texture creation failed; decal/sprites just stay absent
            }
            if (auto* decals = m_scene->GetSystem<render::DecalComponentManager>())
            {
                m_decal = m_scene->CreateEntity(u8"decal");
                foundation::Transform dt = m_scene->GetLocalTransform(m_decal);
                dt.position = foundation::Float3{-2.5f, -0.2f, 2.5f};
                // Decals project along local +Z; rotate so that axis points DOWN at the floor
                // (an unrotated box projects horizontally and the angle fade removes it).
                dt.rotation =
                    foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, 1.5707963f);
                m_scene->SetLocalTransform(m_decal, dt);
                render::DecalComponent& dc = decals->Add(m_decal);
                dc.texture = m_texView;
                dc.size = foundation::Float3{3.0f, 2.0f, 3.0f};
                dc.color = foundation::Color{1.0f, 0.9f, 0.4f, 0.9f};
            }
            if (auto* sprites = m_scene->GetSystem<render::SpriteComponentManager>())
            {
                const foundation::Float3 base{-6.5f, 1.4f, 3.5f};
                const struct
                {
                    bool additive;
                    bool postTonemap;
                    foundation::Color tint;
                } kinds[3] = {
                    {false, false, foundation::Color{1.0f, 1.0f, 1.0f, 0.9f}},
                    {true, false, foundation::Color{0.4f, 0.8f, 1.0f, 1.0f}},
                    {false, true, foundation::Color{1.0f, 0.5f, 0.8f, 1.0f}},
                };
                for (foundation::u32 i = 0; i < 3; ++i)
                {
                    m_sprites[i] = m_scene->CreateEntity(u8"sprite");
                    m_scene->SetLocalPosition(
                        m_sprites[i],
                        base + foundation::Float3{static_cast<foundation::f32>(i) * 1.6f, 0.0f, 0.0f});
                    render::SpriteComponent& sc = sprites->Add(m_sprites[i]);
                    sc.texture = m_texView;
                    sc.size = foundation::Float2{1.2f, 1.2f};
                    sc.tint = kinds[i].tint;
                    sc.additive = kinds[i].additive;
                    sc.postTonemap = kinds[i].postTonemap;
                }
            }
        }

        void BuildParticles()
        {
            auto* pmgr = m_scene->GetSystem<particles::ParticleEffectComponentManager>();
            if (pmgr == nullptr)
            {
                return;
            }

            // A modest additive fountain (billboard path).
            {
                particles::ParticleSystem& sys = m_fountain.AddSystem(6000);
                sys.name = foundation::String{u8"fountain"};
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
                    v.baseVelocity = foundation::Float3{0.0f, 7.5f, 0.0f};
                    v.randomness = foundation::Float3{1.6f, 1.0f, 1.6f};
                }
                sys.AddInitializer<particles::SizeInitializer>().size =
                    particles::RangeFloat2::Constant(foundation::Float2{0.16f, 0.16f});
                sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                    foundation::Float4{1.0f, 0.6f, 0.2f, 1.0f}, foundation::Float4{1.0f, 0.85f, 0.4f, 1.0f});
                sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.2f;
                sys.AddBehavior<particles::ColorOverLifetimeBehavior>().curve =
                    particles::ParticleCurveColor::FadeAlpha(foundation::Float4{1.0f, 0.55f, 0.15f, 1.0f},
                                                             0.35f);
            }
            m_fountainEntity = m_scene->CreateEntity(u8"fountain");
            m_scene->SetLocalPosition(m_fountainEntity, foundation::Float3{6.5f, -0.6f, -3.5f});
            pmgr->Add(m_fountainEntity).SetEffect(m_fountain);

            // Spark trails (ribbon path).
            {
                particles::ParticleSystem& sys = m_sparks.AddSystem(800);
                sys.name = foundation::String{u8"sparks"};
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
                    v.baseVelocity = foundation::Float3{0.0f, 6.0f, 0.0f};
                    v.randomness = foundation::Float3{4.0f, 2.0f, 4.0f};
                }
                sys.AddInitializer<particles::SizeInitializer>().size =
                    particles::RangeFloat2::Constant(foundation::Float2{0.15f, 0.15f});
                sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                    foundation::Float4{0.2f, 0.7f, 1.0f, 1.0f}, foundation::Float4{0.9f, 0.4f, 1.0f, 1.0f});
                sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.4f;
            }
            m_sparksEntity = m_scene->CreateEntity(u8"sparks");
            m_scene->SetLocalPosition(m_sparksEntity, foundation::Float3{-6.5f, -0.5f, -3.5f});
            pmgr->Add(m_sparksEntity).SetEffect(m_sparks);
        }

        void BuildCamera()
        {
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                render::CameraComponent& cam = cameras->Add(m_camera);
                cam.fovYRadians = 1.04719755f; // 60 deg
                cam.nearZ = 0.1f;
                cam.farZ = 200.0f;
                cam.clearColor = foundation::Color{0.05f, 0.06f, 0.09f, 1.0f};
            }
            m_fly.position = foundation::Float3{0.0f, 2.4f, 9.5f};
            m_fly.yaw = 0.0f;
            m_fly.pitch = -0.22f;
            m_fly.moveSpeed = 5.0f;
            m_fly.fastSpeed = 14.0f;
            m_fly.focusDistance = 9.0f;
            foundation::Transform ct = m_scene->GetLocalTransform(m_camera);
            ct.position = m_fly.position;
            ct.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, ct);
        }

        // Game UI, all three faces of it (runtime documents - the proven UISandbox
        // vocabulary: kebab-case attributes, EXPLICIT sizes):
        //   - scene tier:  a HUD canvas (title + controls + a click-counter Button)
        //   - billboard:   a nameplate riding the spinning cube (distance-scaled)
        //   - screen tier: a bottom-right badge pushed onto the scene-less overlay layer
        void BuildGameUI(runtime::IApplicationHost& host)
        {
            // Scene-tier HUD canvas.
            m_hudDocument = foundation::MakeRef<ui::UIDocument>(foundation::DefaultAllocator());
            m_hudDocument->markup = foundation::String(
                u8"<Flex direction=\"vertical\" align=\"start\" padding=\"12\" spacing=\"8\">"
                u8"  <Panel padding=\"12\" width=\"240\""
                u8"         style=\"background: rounded-rect(rgb(28, 32, 40), radius=8);\">"
                u8"    <Flex direction=\"vertical\" spacing=\"8\">"
                u8"      <Label text=\"WebScene\" font-size=\"18\"/>"
                u8"      <Label text=\"WASD/QE move - RMB look\" font-size=\"12\"/>"
                u8"      <Button id=\"ws-btn\" text=\"Clicks: 0\" width=\"200\" height=\"36\"/>"
                u8"    </Flex>"
                u8"  </Panel>"
                u8"</Flex>");
            m_hudEntity = m_scene->CreateEntity(u8"hud");
            if (auto* canvases = m_scene->GetSystem<ui::UICanvasComponentManager>())
            {
                ui::UICanvasComponent& canvas = canvases->Add(m_hudEntity);
                canvas.document = m_hudDocument;
            }

            // Billboard nameplate on the spinning cube (the moving anchor makes the
            // world-tracking path obvious at a glance).
            m_plateDocument = foundation::MakeRef<ui::UIDocument>(foundation::DefaultAllocator());
            m_plateDocument->markup = foundation::String(
                u8"<Panel padding=\"4\""
                u8"       style=\"background: rounded-rect(rgb(20, 24, 30), radius=4);\">"
                u8"  <Label text=\"cube\" font-size=\"13\"/>"
                u8"</Panel>");
            if (auto* billboards = m_scene->GetSystem<ui::UIBillboardComponentManager>())
            {
                ui::UIBillboardComponent& plate = billboards->Add(m_cube);
                plate.document = m_plateDocument;
                plate.offset = foundation::Float3{0.0f, 1.2f, 0.0f};
                plate.scaleMode = ui::BillboardScale::Distance;
                plate.referenceDistance = 12.0f;
            }

            // Screen-tier badge: OUTSIDE any scene (survives scene swaps), anchored
            // bottom-right by swapping the pushed root's layout params to the overlay
            // layer's frame gravity.
            if (auto* gameUi = host.Ctx().GetSubsystem<ui::UISubsystem>())
            {
                m_badgeDocument = foundation::MakeRef<ui::UIDocument>(foundation::DefaultAllocator());
                m_badgeDocument->markup = foundation::String(
                    u8"<Panel padding=\"6\""
                    u8"       style=\"background: rounded-rect(rgb(20, 24, 30), radius=6);\">"
                    u8"  <Label text=\"screen tier\" font-size=\"11\"/>"
                    u8"</Panel>");
                m_badge = gameUi->PushScreenOverlay(*m_badgeDocument);
                if (m_badge.Get() != nullptr)
                {
                    auto lp = foundation::MakeRef<ui::FrameLayoutParams>(foundation::DefaultAllocator());
                    lp->Gravity = ui::Gravity::Right | ui::Gravity::Bottom;
                    m_badge->LayoutParams = lp;
                    // Passive watermark: a hit-testable screen overlay makes the global
                    // layer MODAL (by design, for menus) - which would shield the scene
                    // HUD from every click.
                    m_badge->IsHitTestVisible = false;
                }
            }
        }

        // Swap in a freshly built floor material for the current tweak values. The mesh
        // renderer keys material instances by UID and prunes unreferenced ones, so material
        // replacement is the clean live-tweak path.
        void ApplyFloorMaterial()
        {
            if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
            {
                if (render::MeshComponent* fm = meshes->Get(m_floor))
                {
                    fm->SetMaterial(materials::CreatePBR(u8"web.floor",
                                                         foundation::Float4{0.28f, 0.29f, 0.33f, 1.0f},
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
            constexpr foundation::u32 kSize = 64;
            static foundation::u8 pixels[kSize * kSize * 4];
            for (foundation::u32 y = 0; y < kSize; ++y)
            {
                for (foundation::u32 x = 0; x < kSize; ++x)
                {
                    const foundation::f32 fx = (static_cast<foundation::f32>(x) + 0.5f) / kSize - 0.5f;
                    const foundation::f32 fy = (static_cast<foundation::f32>(y) + 0.5f) / kSize - 0.5f;
                    const foundation::f32 d = foundation::Sqrt(fx * fx + fy * fy);
                    const bool check = (((x / 8) + (y / 8)) & 1u) != 0u;
                    const foundation::f32 ring =
                        foundation::Clamp(1.0f - foundation::Abs(d - 0.32f) * 12.0f, 0.0f, 1.0f);
                    const foundation::u8 base = check ? 200 : 90;
                    foundation::u8* p = &pixels[(y * kSize + x) * 4];
                    p[0] = static_cast<foundation::u8>(foundation::Min(255.0f, base + ring * 255.0f));
                    p[1] = static_cast<foundation::u8>(foundation::Min(255.0f, base * 0.8f + ring * 200.0f));
                    p[2] = static_cast<foundation::u8>(base / 2);
                    const foundation::f32 alpha = foundation::Clamp(ring + (check ? 0.55f : 0.25f), 0.0f, 1.0f);
                    p[3] = static_cast<foundation::u8>(alpha * 255.0f);
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
                    tb->WriteTexture(m_tex, foundation::Span<const foundation::u8>{pixels, sizeof(pixels)},
                                     layout, rhi::Extent3D{kSize, kSize, 1});
                    (void)tb->Submit();
                    q->DestroyTransferBatch(tb);
                }
            }
        }

        // --- the tweak panel --------------------------------------------------------------

#if DRACONIC_HAS_EXTENSION_IMGUI
        void BuildTweakPanel(render::RenderSubsystem* renderSub)
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
                if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
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
                if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
                {
                    if (render::LightComponent* sun = lights->Get(m_sun))
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
                if (auto* decals = m_scene->GetSystem<render::DecalComponentManager>())
                {
                    if (render::DecalComponent* dc = decals->Get(m_decal))
                    {
                        ImGui::Checkbox("Decal", &dc->visible);
                    }
                }
                ImGui::Checkbox("Debug draw (gizmos)", &m_showDebugDraw);
                if (auto* probes = m_scene->GetSystem<render::ReflectionProbeComponentManager>())
                {
                    if (render::ReflectionProbeComponent* pc = probes->Get(m_probe))
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
                if (auto* sprites = m_scene->GetSystem<render::SpriteComponentManager>())
                {
                    if (render::SpriteComponent* sc = sprites->Get(m_sprites[0]))
                    {
                        bool on = sc->visible;
                        if (ImGui::Checkbox("Sprites", &on))
                        {
                            for (const scene::EntityHandle& h : m_sprites)
                            {
                                if (render::SpriteComponent* s = sprites->Get(h))
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

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_cube{};
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_sun{};
        scene::EntityHandle m_pointLight{};
        scene::EntityHandle m_floor{};
        scene::EntityHandle m_decal{};
        scene::EntityHandle m_probe{};
        bool m_showDebugDraw = true;
        foundation::f32 m_fpsSmoothed = 0.0f;
        scene::EntityHandle m_sprites[3] = {};
        scene::EntityHandle m_fountainEntity{};
        scene::EntityHandle m_sparksEntity{};
        particles::ParticleEffect m_fountain;
        particles::ParticleEffect m_sparks;
        rhi::Texture* m_tex = nullptr; // freed in OnShutdown (validation-clean teardown)
        rhi::TextureView* m_texView = nullptr;
        FlyCamera m_fly;
        foundation::f32 m_time = 0.0f;
        foundation::f32 m_floorMetallic = 0.0f;  // floor material tweakables (SSR eye test)
        foundation::f32 m_floorRoughness = 0.12f;
        // Game UI (documents keep the runtime markup alive; the badge keep-alive lets a
        // later phase pop it).
        foundation::RefPtr<ui::UIDocument> m_hudDocument;
        foundation::RefPtr<ui::UIDocument> m_plateDocument;
        foundation::RefPtr<ui::UIDocument> m_badgeDocument;
        foundation::RefPtr<ui::View> m_badge;
        scene::EntityHandle m_hudEntity{};
        foundation::u32 m_hudClicks = 0;
        bool m_hudBound = false;
    };
}

#endif // DRACONIC_SAMPLES_WEBSCENE_APP_H
