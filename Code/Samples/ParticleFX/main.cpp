// ParticleFX - the particle-system showcase. Builds a ParticleEffect in code (an additive fountain),
// attaches it to an entity via ParticleEffectComponent, and lets draconic.engine.particles tick the
// CPU sim + draw the billboards through the dedicated ParticleRenderer. Phase 2 of the particle track
// (docs/design/particles.md): CPU sim on the existing extract->resolve->draw pipeline. GPU-compute sim,
// trails, mesh particles, and the cooked resource/editor land in later phases.

#include "Draconic.Foundation/Prelude.h"
#include "imgui.h"

import draconic.foundation;
import draconic.rhi;
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.engine.defaultapp;
import draconic.scene;
import draconic.engine.scene;
import draconic.engine.render;
import draconic.render;
import draconic.imgui;
import draconic.geometry;
import draconic.materials;
import draconic.particles;           // the CPU sim (effect/system/modules)
import draconic.engine.particles; // the ECS component + ParticleSubsystem
import draconic.particles.resource;  // cooked ParticleEffectResource + factory
import draconic.particles.editor;    // ParticleEffectAsset + bake (the authoring demo)
import draconic.editor;              // AssetBuildContext
import draconic.vfs;                 // NativeFileSystem mount for the content DB
import draconic.content;             // ContentDatabase (cooked-output DB)
import draconic.resource;            // ResourceManager + Proxy

#include "../Common/FlyCamera.h"

namespace foundation = draconic::foundation;
namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace imgui = draconic::imgui;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace particles = draconic::particles;
namespace vfs = draconic::vfs;
namespace content = draconic::content;
namespace resource = draconic::resource;
namespace editor = draconic::editor;

namespace
{
    class ParticleFXApp final : public runtime::DefaultApplication
    {
    public:
        // Run uncapped (vsync off) so the frame time reflects real particle sim/render cost, not the
        // display refresh. The image may tear; fine for a showcase/benchmark.
        graphics::RenderWindowDesc MainRenderWindow() const override
        {
            graphics::RenderWindowDesc d;
            d.presentMode = rhi::PresentMode::Immediate;
            return d;
        }

        void Configure(runtime::IApplicationHost& host) override
        {
            runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight());
            }
        }

        void OnStartup(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"particlefx");

            // Dim cool ambient so the (unlit) additive particles pop against the lit floor.
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                env->Environment().ambientColor = foundation::Color{0.10f, 0.12f, 0.18f, 1.0f};
                env->Environment().ambientIntensity = 0.4f;
            }

            // Camera: high + pulled back to frame the whole 4x4 showcase grid.
            m_camera = m_scene->CreateEntity(u8"camera");
            m_scene->SetLocalPosition(m_camera, foundation::Float3{0.0f, 34.0f, 52.0f});
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                render::CameraComponent& cam = cameras->Add(m_camera);
                cam.clearColor = foundation::Color{0.02f, 0.02f, 0.04f, 1.0f};
            }
            m_fly.position = foundation::Float3{0.0f, 34.0f, 52.0f};
            m_fly.pitch = -0.55f;

            // A floor for spatial context.
            if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
            {
                scene::EntityHandle floor = m_scene->CreateEntity(u8"floor");
                m_scene->SetLocalPosition(floor, foundation::Float3{0.0f, 0.0f, 0.0f});
                render::MeshComponent& mc = meshes->Add(floor);
                mc.mesh = geometry::Primitives::Plane(80.0f, 80.0f);
                mc.SetMaterial(materials::CreatePBR(
                    u8"floor", foundation::Float4{0.20f, 0.22f, 0.26f, 1.0f}, 0.0f, 0.8f));
            }
            if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
            {
                scene::EntityHandle key = m_scene->CreateEntity(u8"key");
                foundation::Transform kt = m_scene->GetLocalTransform(key);
                kt.rotation =
                    foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -0.9f);
                m_scene->SetLocalTransform(key, kt);
                render::LightComponent& lc = lights->Add(key);
                lc.type = render::LightType::Directional;
                lc.color = foundation::Color{0.6f, 0.7f, 0.95f,
                                       1.0f}; // cool + dim so the warm ember point-lights read
                lc.intensity = 1.0f;
            }

            const foundation::Float3 obstacle = CellPos(6) + foundation::Float3{0.0f, 1.4f, 0.0f};
            const foundation::f32 obRadius = 1.4f;
            BuildFountain(m_effect);
            BuildMeshShards(m_debris);   // cell 1: opaque material (set below)
            BuildMeshShards(m_meshGlow); // cell 2: additive material (set below) - identical sim
            BuildEmbers(m_embers);
            BuildHaze(m_haze);
            BuildTrail(m_trail);
            BuildCollision(m_collide, obstacle, obRadius);
            BuildLocal(m_local);
            BuildSmoke(m_smoke);
            BuildFire(m_fire);
            BuildCampfire(m_campfire);
            BuildFireworks(m_fireworks);
            BuildTornado(m_tornado);
            BuildExplosionBlast(m_explosion);
            BuildExplosionFx(m_explosionFx);
            BuildMagicCircle(m_magic);
            BuildFireflies(m_fireflies);

            if (auto* pmgr = m_scene->GetSystem<particles::ParticleEffectComponentManager>())
            {
                // One system per cell of a 4x4 showcase grid (cells 12-15 reserved for later samples).
                // Cell 0: billboard fountain (additive soft dots).
                m_emitter = m_scene->CreateEntity(u8"fountain");
                m_scene->SetLocalPosition(m_emitter, CellPos(0));
                pmgr->Add(m_emitter).SetEffect(
                    m_effect); // no texture -> the renderer's soft-dot default

                // Cells 1 & 2: two mesh systems side by side, IDENTICAL sim - only the material differs.
                // Cell 1 = opaque solid shards; cell 2 = additive glow (routed to the Transparent pass).
                m_debrisEmitter = m_scene->CreateEntity(u8"shards-solid");
                m_scene->SetLocalPosition(m_debrisEmitter, CellPos(1));
                particles::ParticleEffectComponent& dc = pmgr->Add(m_debrisEmitter);
                dc.SetEffect(m_debris);
                dc.mesh = geometry::Primitives::Cube(1.0f);
                dc.material = materials::CreatePBR(
                    u8"shards-solid", foundation::Float4{0.35f, 0.6f, 0.9f, 1.0f}, 0.1f, 0.5f);
                dc.meshScale = 0.5f;

                m_glowEmitter = m_scene->CreateEntity(u8"shards-glow");
                m_scene->SetLocalPosition(m_glowEmitter, CellPos(2));
                particles::ParticleEffectComponent& gc = pmgr->Add(m_glowEmitter);
                gc.SetEffect(m_meshGlow);
                gc.mesh = geometry::Primitives::Cube(1.0f);
                gc.meshScale = 0.5f;
                {
                    // Additive PBR material -> the extractor routes these to the Transparent pass.
                    auto glow = materials::CreatePBR(
                        u8"shards-glow", foundation::Float4{0.4f, 0.8f, 1.0f, 1.0f}, 0.0f, 0.4f);
                    glow->pipeline.blendMode = materials::BlendMode::Additive;
                    glow->pipeline.depthMode = materials::DepthMode::ReadOnly;
                    gc.material = glow;
                }

                // Cell 3: light particles (drifting embers) - a point light per particle + a billboard glow.
                m_emberEmitter = m_scene->CreateEntity(u8"embers");
                m_scene->SetLocalPosition(m_emberEmitter, CellPos(3));
                particles::ParticleEffectComponent& ec = pmgr->Add(m_emberEmitter);
                ec.SetEffect(m_embers);
                ec.lightIntensity = 14.0f;
                ec.lightRange =
                    6.0f; // smaller range -> fewer clusters overlap -> no visible cluster grid

                // Cell 4: ground haze - the soft-particle A/B showcase.
                m_hazeEmitter = m_scene->CreateEntity(u8"haze");
                m_scene->SetLocalPosition(m_hazeEmitter,
                                          CellPos(4) + foundation::Float3{0.0f, 0.4f, 0.0f});
                pmgr->Add(m_hazeEmitter).SetEffect(m_haze);

                // Cell 5: trail sparks (camera-facing ribbons).
                m_trailEmitter = m_scene->CreateEntity(u8"trail-sparks");
                m_scene->SetLocalPosition(m_trailEmitter, CellPos(5));
                pmgr->Add(m_trailEmitter).SetEffect(m_trail);

                // Cell 6: collision rain bouncing off a rendered sphere obstacle + the world ground.
                m_collideEmitter = m_scene->CreateEntity(u8"rain");
                m_scene->SetLocalPosition(m_collideEmitter,
                                          CellPos(6) + foundation::Float3{0.0f, 6.0f, 0.0f});
                pmgr->Add(m_collideEmitter).SetEffect(m_collide);
                if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
                {
                    scene::EntityHandle ob = m_scene->CreateEntity(u8"obstacle");
                    m_scene->SetLocalPosition(ob, obstacle);
                    render::MeshComponent& omc = meshes->Add(ob);
                    omc.mesh = geometry::Primitives::Sphere(obRadius);
                    omc.SetMaterial(materials::CreatePBR(
                        u8"obstacle", foundation::Float4{0.7f, 0.7f, 0.72f, 1.0f}, 0.1f, 0.4f));
                }

                // Cell 7: local-space puff - orbits its emitter (animated in OnUpdate); cloud follows rigidly.
                m_localEmitter = m_scene->CreateEntity(u8"orbit");
                m_scene->SetLocalPosition(m_localEmitter, CellPos(7));
                pmgr->Add(m_localEmitter).SetEffect(m_local);

                // Cell 8: smoke. Cell 9: fire. Cell 10: campfire (fire + smoke). Cell 11: fireworks.
                m_smokeEmitter = m_scene->CreateEntity(u8"smoke");
                m_scene->SetLocalPosition(m_smokeEmitter, CellPos(8));
                pmgr->Add(m_smokeEmitter).SetEffect(m_smoke);

                m_fireEmitter = m_scene->CreateEntity(u8"fire");
                m_scene->SetLocalPosition(m_fireEmitter, CellPos(9));
                pmgr->Add(m_fireEmitter).SetEffect(m_fire);

                m_campfireEmitter = m_scene->CreateEntity(u8"campfire");
                m_scene->SetLocalPosition(m_campfireEmitter, CellPos(10));
                pmgr->Add(m_campfireEmitter).SetEffect(m_campfire);

                m_fireworksEmitter = m_scene->CreateEntity(u8"fireworks");
                m_scene->SetLocalPosition(m_fireworksEmitter, CellPos(11));
                pmgr->Add(m_fireworksEmitter).SetEffect(m_fireworks);

                // Cell 12: tornado (vortex/attractor swirl, local space).
                m_tornadoEmitter = m_scene->CreateEntity(u8"tornado");
                m_scene->SetLocalPosition(m_tornadoEmitter, CellPos(12));
                pmgr->Add(m_tornadoEmitter).SetEffect(m_tornado);

                // Cell 13: explosion - flipbook fireball blast (atlas texture) + debris/shockwave (soft dot).
                m_explosionEmitter = m_scene->CreateEntity(u8"explosion");
                m_scene->SetLocalPosition(m_explosionEmitter, CellPos(13));
                particles::ParticleEffectComponent& xc = pmgr->Add(m_explosionEmitter);
                xc.SetEffect(m_explosion);
                if (rhi::Device* dev =
                        (host.Graphics() != nullptr) ? host.Graphics()->Raw() : nullptr)
                {
                    xc.texture = MakeBlastAtlas(*dev);
                }
                m_explosionFxEmitter = m_scene->CreateEntity(u8"explosion-fx");
                m_scene->SetLocalPosition(m_explosionFxEmitter, CellPos(13));
                pmgr->Add(m_explosionFxEmitter).SetEffect(m_explosionFx);

                // Cell 14: magic circle (ground rune ring + rising glyph sparks).
                m_magicEmitter = m_scene->CreateEntity(u8"magic");
                m_scene->SetLocalPosition(m_magicEmitter, CellPos(14));
                pmgr->Add(m_magicEmitter).SetEffect(m_magic);

                // Cell 15: fireflies (wind + turbulence wander, twinkling).
                m_firefliesEmitter = m_scene->CreateEntity(u8"fireflies");
                m_scene->SetLocalPosition(m_firefliesEmitter, CellPos(15));
                pmgr->Add(m_firefliesEmitter).SetEffect(m_fireflies);

                // Authoring pipeline demo: author -> bake -> load a cooked effect, in front of the grid.
                SetupCookedDemo(*pmgr);

                // Sync only the soft-particle A/B systems to the slider (smoke keeps soft OFF - the fade
                // against the floor behind a rising column zeroes its alpha).
                ApplySoft(m_effect);
                ApplySoft(m_embers);
                ApplySoft(m_haze);
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            runtime::DefaultApplication::OnRenderWindow(host, frame);
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

        // Free the explosion flipbook atlas we created in OnStartup (device still alive here).
        void OnShutdown(runtime::IApplicationHost& host) override
        {
            if (rhi::Device* dev = (host.Graphics() != nullptr) ? host.Graphics()->Raw() : nullptr)
            {
                dev->WaitIdle();
                if (m_blastView != nullptr)
                {
                    dev->DestroyTextureView(m_blastView);
                    m_blastView = nullptr;
                }
                if (m_blastTex != nullptr)
                {
                    dev->DestroyTexture(m_blastTex);
                    m_blastTex = nullptr;
                }
            }
            runtime::DefaultApplication::OnShutdown(host);
        }

        void OnUpdate(runtime::IApplicationHost& host, foundation::f32 deltaTime) override
        {
            runtime::DefaultApplication::OnUpdate(host, deltaTime);
            m_frameSmooth = m_frameSmooth * 0.9f + deltaTime * 0.1f;

            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildHud();
            }
            m_fly.Update(host, deltaTime);
            if (m_scene != nullptr)
            {
                foundation::Transform camT = m_scene->GetLocalTransform(m_camera);
                camT.position = m_fly.position;
                camT.rotation = m_fly.Rotation();
                m_scene->SetLocalTransform(m_camera, camT);

                // Orbit the local-space emitter so its (Local) cloud visibly rides along as a rigid body.
                m_orbitTime += deltaTime;
                const foundation::Float3 c = CellPos(7);
                m_scene->SetLocalPosition(
                    m_localEmitter, c + foundation::Float3{2.5f * foundation::Cos(m_orbitTime * 1.5f), 1.5f,
                                                     2.5f * foundation::Sin(m_orbitTime * 1.5f)});

                // A floor label under each cell (debug-draw 3D text), so every system is identified.
                if (auto* rs = host.Ctx().GetSubsystem<render::RenderSubsystem>())
                {
                    static const foundation::StringView kNames[16] = {
                        u8"fountain", u8"mesh solid", u8"mesh glow",    u8"embers",
                        u8"haze",     u8"trail",      u8"collision",    u8"orbit (local)",
                        u8"smoke",    u8"fire",       u8"campfire",     u8"fireworks",
                        u8"tornado",  u8"explosion",  u8"magic circle", u8"fireflies"};
                    auto& dd = rs->DebugScene(*m_scene);
                    for (foundation::i32 i = 0; i < 16; ++i)
                    {
                        dd.DrawText3D(CellPos(i) + foundation::Float3{0.0f, 0.15f, kCellSpacing * 0.42f},
                                      kNames[i], foundation::Color{0.85f, 0.9f, 1.0f, 1.0f});
                    }
                    if (m_cookedProxy)
                    {
                        dd.DrawText3D(foundation::Float3{0.0f, 0.3f, 37.0f},
                                      u8"COOKED: asset -> bake -> load",
                                      foundation::Color{0.4f, 1.0f, 0.9f, 1.0f});
                    }
                }
            }
        }

    private:
        // 4x4 showcase grid, centered on the origin. Cells are indexed row-major (0..15); each holds one
        // particle system. Returns the cell's floor-level center (callers add any per-system y offset).
        static constexpr foundation::f32 kCellSpacing = 15.0f;
        static constexpr foundation::i32 kGridCols = 4;
        static foundation::Float3 CellPos(foundation::i32 index)
        {
            const foundation::f32 half = (kGridCols - 1) * 0.5f;
            const foundation::f32 col = static_cast<foundation::f32>(index % kGridCols);
            const foundation::f32 row = static_cast<foundation::f32>(index / kGridCols);
            return foundation::Float3{(col - half) * kCellSpacing, 0.2f, (row - half) * kCellSpacing};
        }

        static void BuildFountain(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(30000);
            sys.name = foundation::String{u8"fountain"};
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 3000.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Sphere(0.25f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.6f, 2.6f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 13.0f, 0.0f};
                v.randomness = foundation::Float3{3.0f, 2.0f, 3.0f};
                v.shape = particles::EmissionShape::Cone(0.4f, 0.35f);
                v.shapeDirectionSpeed = 4.0f;
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.35f, 0.35f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{1.0f, 0.55f, 0.15f, 1.0f}, foundation::Float4{1.0f, 0.85f, 0.4f, 1.0f});

            sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.4f;
            sys.AddBehavior<particles::DragBehavior>().drag = 0.25f;
            sys.AddBehavior<particles::ColorOverLifetimeBehavior>().curve =
                particles::ParticleCurveColor::FadeAlpha(foundation::Float4{1.0f, 0.5f, 0.12f, 1.0f},
                                                         0.35f);
            sys.AddBehavior<particles::SizeOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat2::Linear(foundation::Float2{0.4f, 0.4f},
                                                       foundation::Float2{0.04f, 0.04f});
        }

        // Light-mode particles: slow warm embers drifting up; each contributes a point light so they
        // paint moving pools of light on the floor (plus the additive billboard glow).
        static void BuildEmbers(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys =
                effect.AddSystem(400); // few: sparse enough not to saturate clusters
            sys.name = foundation::String{u8"embers"};
            sys.renderMode = particles::ParticleRenderMode::Light;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate =
                12.0f; // ~45 alive: all fit under the cap (stable, no popping) + light cluster load

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Box(foundation::Float3{4.0f, 0.1f, 4.0f});
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(2.5f, 4.5f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 1.4f, 0.0f};
                v.randomness = foundation::Float3{0.5f, 0.3f, 0.5f};
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.6f, 0.6f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{1.0f, 0.5f, 0.15f, 1.0f}, foundation::Float4{1.0f, 0.75f, 0.3f, 1.0f});
            sys.AddBehavior<particles::DragBehavior>().drag = 0.5f;
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.55f); // bright, then fade
        }

        // Ground haze: big, slow, camera-facing billboards centered at floor level so each quad straddles
        // the ground plane - the clearest soft-particle A/B (hard clip line vs. soft fade at the floor).
        static void BuildHaze(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(300);
            sys.name = foundation::String{u8"haze"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.softDistance = 2.0f; // wide fade band, obvious in the A/B
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 14.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Box(foundation::Float3{3.5f, 0.05f, 3.5f});
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(4.0f, 6.0f);
            sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, 0.25f, 0.0f};
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(
                    foundation::Float2{2.0f, 2.0f}); // straddles the floor for the A/B
            sys.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{0.35f, 0.28f, 0.45f, 1.0f});
            sys.AddBehavior<particles::DragBehavior>().drag = 0.6f;
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.4f);
        }

        // Trail-mode: arcing sparks, each leaving a camera-facing ribbon behind it (the Phase-5 showcase).
        static void BuildTrail(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(2000);
            sys.name = foundation::String{u8"sparks"};
            sys.renderMode = particles::ParticleRenderMode::Trail;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 40.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Sphere(0.15f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.4f, 2.4f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 8.0f, 0.0f};
                v.randomness =
                    foundation::Float3{6.0f, 3.0f, 6.0f}; // spray sideways so the ribbons curve
                v.shape = particles::EmissionShape::Cone(0.5f, 0.2f);
                v.shapeDirectionSpeed = 3.0f;
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.2f, 0.2f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{0.2f, 0.7f, 1.0f, 1.0f}, foundation::Float4{0.9f, 0.4f, 1.0f, 1.0f});

            sys.AddBehavior<particles::GravityBehavior>().multiplier =
                1.5f; // arc back down -> curved ribbons
            sys.AddBehavior<particles::ColorOverLifetimeBehavior>().curve =
                particles::ParticleCurveColor::FadeAlpha(foundation::Float4{0.5f, 0.6f, 1.0f, 1.0f},
                                                         0.3f);

            sys.trail.enabled = true;
            sys.trail.maxPoints = 32;
            sys.trail.recordInterval = 0.02f;
            sys.trail.lifetime = 0.6f; // how long each recorded point lingers (ribbon length)
            sys.trail.widthStart = 0.22f;
            sys.trail.widthEnd = 0.0f; // taper to a point at the tail
            sys.trail.minVertexDistance = 0.04f;
            sys.trail.useParticleColor = true;
        }

        // Collision showcase: rain that spawns high and bounces off both the world ground plane (y=0) and a
        // rendered sphere obstacle at `obstacle` (radius `obRadius`). World-space, so the collider centre
        // is a world position matching the drawn sphere.
        static void BuildCollision(particles::ParticleEffect& effect, foundation::Float3 obstacle,
                                   foundation::f32 obRadius)
        {
            particles::ParticleSystem& sys = effect.AddSystem(4000);
            sys.name = foundation::String{u8"rain"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Alpha;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 500.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Box(foundation::Float3{3.0f, 0.1f, 3.0f});
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(3.0f, 4.0f);
            sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, -1.0f, 0.0f};
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.12f, 0.12f});
            sys.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{0.5f, 0.75f, 1.0f, 0.9f});
            sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.0f;
            {
                particles::CollisionBehavior& col = sys.AddBehavior<particles::CollisionBehavior>();
                col.planes[0] =
                    particles::CollisionPlane{foundation::Float3{0.0f, 1.0f, 0.0f}, 0.0f}; // world ground
                col.planeCount = 1;
                col.spheres[0] =
                    particles::CollisionSphere{obstacle, obRadius}; // the drawn obstacle
                col.sphereCount = 1;
                col.radius = 0.06f; // particle radius so drops sit on the surface, not in it
                col.bounce = 0.45f;
                col.friction = 0.15f;
                col.lifetimeLoss = 0.2f; // lose some life on each hit so splashes settle
            }
        }

        // A single fire system (billboard additive): fast rising, shrinking, colour-graded white->red.
        static void ConfigureFire(particles::ParticleSystem& sys, foundation::f32 scale)
        {
            sys.name = foundation::String{u8"fire"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 160.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Circle(0.6f * scale);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(0.6f, 1.1f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 3.2f * scale, 0.0f};
                v.randomness = foundation::Float3{0.7f, 0.6f, 0.7f};
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{1.0f * scale, 1.0f * scale});
            sys.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{1.0f, 0.9f, 0.5f, 1.0f});
            sys.AddBehavior<particles::TurbulenceBehavior>().strength = 1.5f; // flicker/curl
            sys.AddBehavior<particles::SizeOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat2::Linear(foundation::Float2{1.0f * scale, 1.0f * scale},
                                                       foundation::Float2{0.15f * scale, 0.15f * scale});
            // Colour ramp: white-hot -> yellow -> orange -> red, fading out at the tip.
            particles::ParticleCurveColor ramp;
            ramp.AddKey(0.0f, foundation::Float4{1.0f, 0.95f, 0.7f, 1.0f});
            ramp.AddKey(0.35f, foundation::Float4{1.0f, 0.6f, 0.2f, 0.9f});
            ramp.AddKey(0.7f, foundation::Float4{0.9f, 0.2f, 0.05f, 0.5f});
            ramp.AddKey(1.0f, foundation::Float4{0.4f, 0.05f, 0.02f, 0.0f});
            sys.AddBehavior<particles::ColorOverLifetimeBehavior>().curve = ramp;
        }

        // A single smoke system (billboard alpha): slow rise, expanding, drifting, fading. Soft particles
        // are OFF - the soft-depth fade against the floor behind it would zero the alpha of a rising column.
        static void ConfigureSmoke(particles::ParticleSystem& sys, foundation::f32 scale, foundation::f32 rise)
        {
            sys.name = foundation::String{u8"smoke"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Alpha;
            sys.softParticles = false;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 40.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Circle(0.5f * scale);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(3.0f, 5.0f);
            sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, rise, 0.0f};
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{1.0f * scale, 1.0f * scale});
            // Near-white ambient-lit grey: light enough to read against the (grey) floor it starts over AND
            // the dark sky it rises into. Grey-on-grey-floor was the reason earlier smoke was invisible.
            sys.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{0.78f, 0.78f, 0.82f, 1.0f});
            sys.AddBehavior<particles::TurbulenceBehavior>().strength = 0.8f; // lazy drift
            sys.AddBehavior<particles::DragBehavior>().drag =
                0.12f; // light drag -> the column keeps climbing off the floor
            sys.AddBehavior<particles::SizeOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat2::Linear(foundation::Float2{1.0f * scale, 1.0f * scale},
                                                       foundation::Float2{3.5f * scale, 3.5f * scale});
            // Fade in from nothing, hold fairly opaque, fade out (billows appear then dissipate).
            particles::ParticleCurveFloat a;
            a.AddKey(0.0f, 0.0f);
            a.AddKey(0.15f, 0.9f);
            a.AddKey(0.6f, 0.7f);
            a.AddKey(1.0f, 0.0f);
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve = a;
        }

        static void BuildFire(particles::ParticleEffect& effect)
        {
            ConfigureFire(effect.AddSystem(2000), 1.0f);
        }
        static void BuildSmoke(particles::ParticleEffect& effect)
        {
            ConfigureSmoke(effect.AddSystem(1200), 1.0f, 3.0f);
        }

        // Composite effect: fire at the base + smoke rising above it, in ONE effect (two systems) - the
        // multi-system authoring case.
        static void BuildCampfire(particles::ParticleEffect& effect)
        {
            ConfigureFire(effect.AddSystem(2000), 0.7f);
            // Thin, gentle wisp scaled to the small flame (not a billowing column): smaller, slower, sparser.
            particles::ParticleSystem& smoke = effect.AddSystem(400);
            ConfigureSmoke(smoke, 0.35f, 2.0f);
            smoke.emitter.spawnRate = 12.0f;
        }

        // Fireworks: rockets shoot up and, on death, burst into a colour-inheriting spark shower via a
        // sub-emitter link (OnDeath -> child system, inherit position + colour).
        static void BuildFireworks(particles::ParticleEffect& effect)
        {
            // System 0: rockets - launch up, short life, additive with a trail.
            particles::ParticleSystem& rocket = effect.AddSystem(64);
            rocket.name = foundation::String{u8"rocket"};
            rocket.renderMode = particles::ParticleRenderMode::Trail;
            rocket.blendMode = particles::ParticleBlendMode::Additive;
            rocket.emitter.mode = particles::EmissionMode::Continuous;
            rocket.emitter.spawnRate = 3.0f; // a few launches per second
            rocket.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Circle(0.4f);
            rocket.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.0f, 1.4f);
            {
                particles::VelocityInitializer& v =
                    rocket.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 13.0f, 0.0f};
                v.randomness = foundation::Float3{1.5f, 1.5f, 1.5f};
            }
            rocket.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.25f, 0.25f});
            // Vivid warm hues (gold -> hot magenta) so the bursts pop against the dark sky - blues would
            // wash out. Sparks inherit this colour via the sub-emitter link.
            rocket.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{1.0f, 0.85f, 0.2f, 1.0f}, foundation::Float4{1.0f, 0.25f, 0.7f, 1.0f});
            rocket.AddBehavior<particles::GravityBehavior>().multiplier =
                1.0f; // arc to an apex, then die
            rocket.trail.enabled = true;
            rocket.trail.maxPoints = 20;
            rocket.trail.lifetime = 0.4f;
            rocket.trail.widthStart = 0.12f;
            rocket.trail.widthEnd = 0.0f;
            rocket.trail.recordInterval = 0.02f;

            // System 1: sparks - spawned by rocket deaths (no self-emission); explode outward, gravity, fade.
            particles::ParticleSystem& sparks = effect.AddSystem(6000);
            sparks.name = foundation::String{u8"sparks"};
            sparks.renderMode = particles::ParticleRenderMode::Billboard;
            sparks.blendMode = particles::ParticleBlendMode::Additive;
            sparks.emitter.isEmitting = false;
            {
                particles::PositionInitializer& p =
                    sparks.AddInitializer<particles::PositionInitializer>();
                p.shape = particles::EmissionShape::Point();
            }
            sparks.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(0.9f, 1.7f);
            {
                particles::VelocityInitializer& v =
                    sparks.AddInitializer<particles::VelocityInitializer>();
                v.shape = particles::EmissionShape::Sphere(1.0f, /*shell*/ true); // radial burst
                v.shapeDirectionSpeed = 7.0f;
                v.randomness = foundation::Float3{1.0f, 1.0f, 1.0f};
            }
            sparks.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.16f, 0.16f});
            sparks.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{1.0f, 1.0f, 1.0f, 1.0f});
            sparks.AddBehavior<particles::GravityBehavior>().multiplier = 1.3f;
            sparks.AddBehavior<particles::DragBehavior>().drag = 0.6f;
            sparks.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.15f);

            particles::SubEmitterLink link = particles::SubEmitterLink::Default();
            link.trigger = particles::ParticleEventType::OnDeath;
            link.childSystemIndex = 1;
            link.spawnCount = 80;
            link.probability = 1.0f;
            link.inheritPosition = true;
            link.inheritColor =
                true; // sparks take the rocket's colour (needs the SpawnAt inherit fix)
            effect.AddSubEmitterLink(link);
        }

        // Tornado: a dust column that swirls up a vertical axis. Local space keeps the vortex + attractor
        // centres at the origin (re-based to the cell at extract), so the funnel spins around itself.
        static void BuildTornado(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(4000);
            sys.name = foundation::String{u8"tornado"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.softParticles = false;
            sys.simulationSpace =
                particles::ParticleSpace::Local; // vortex/attractor centres sit at the local origin
            sys.prewarmTime = 2.0f;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 600.0f;
            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Circle(1.6f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.6f, 2.8f);
            sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, 4.5f, 0.0f};
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.4f, 0.4f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{0.55f, 0.5f, 0.42f, 1.0f}, foundation::Float4{0.4f, 0.36f, 0.3f, 1.0f});
            sys.AddBehavior<particles::VortexBehavior>().strength =
                12.0f; // tangential swirl about local Y
            sys.AddBehavior<particles::AttractorBehavior>().strength =
                3.5f; // pull inward toward the axis (tighten the funnel)
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.1f);
        }

        // Explosion blast core: burst-looping fireballs that play a 4x4 flipbook atlas over their short life
        // (the atlas texture is set on the component). RadialForce spreads the puffs into a cluster.
        static void BuildExplosionBlast(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(200);
            sys.name = foundation::String{u8"blast"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.softParticles = true; // soften where the fireballs meet the ground/obstacles
            sys.softDistance = 1.0f;
            sys.emitter.mode = particles::EmissionMode::Burst;
            sys.emitter.burstCount = 5;
            sys.emitter.burstInterval = 1.7f; // repeating boom
            sys.emitter.burstCycles = 0;      // forever
            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Sphere(0.5f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(0.7f, 0.9f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 1.5f, 0.0f};
                v.randomness = foundation::Float3{2.0f, 1.0f, 2.0f};
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{3.5f, 3.5f});
            sys.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{1.0f, 1.0f, 1.0f, 1.0f});
            sys.AddBehavior<particles::RadialForceBehavior>().strength =
                5.0f; // spread the cluster outward
            sys.AddBehavior<particles::DragBehavior>().drag = 1.5f;
            sys.flipbook.enabled = true;
            sys.flipbook.columns = 4;
            sys.flipbook.rows = 4;
            sys.flipbook.overLifetime = true; // play all 16 frames across the particle's life
        }

        // Explosion secondary FX (no texture -> soft dot): velocity-stretched debris streaks + a flat ground
        // shockwave disc. Burst-synced to the blast.
        static void BuildExplosionFx(particles::ParticleEffect& effect)
        {
            // Debris streaks (StretchedBillboard: the quad stretches along the particle velocity).
            particles::ParticleSystem& deb = effect.AddSystem(1200);
            deb.name = foundation::String{u8"debris"};
            deb.renderMode = particles::ParticleRenderMode::StretchedBillboard;
            deb.blendMode = particles::ParticleBlendMode::Additive;
            deb.softParticles = false;
            deb.emitter.mode = particles::EmissionMode::Burst;
            deb.emitter.burstCount = 140;
            deb.emitter.burstInterval = 1.7f;
            deb.emitter.burstCycles = 0;
            deb.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Sphere(0.3f);
            deb.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(0.5f, 1.0f);
            {
                particles::VelocityInitializer& v =
                    deb.AddInitializer<particles::VelocityInitializer>();
                v.shape = particles::EmissionShape::Sphere(1.0f, /*shell*/ true); // radial burst
                v.shapeDirectionSpeed = 13.0f;
                v.randomness = foundation::Float3{2.0f, 2.0f, 2.0f};
            }
            deb.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.14f, 0.14f});
            deb.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{1.0f, 0.85f, 0.35f, 1.0f}, foundation::Float4{1.0f, 0.35f, 0.1f, 1.0f});
            deb.AddBehavior<particles::GravityBehavior>().multiplier = 1.4f;
            deb.AddBehavior<particles::DragBehavior>().drag = 1.0f;
            deb.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.2f);

            // Shockwave: a single flat disc that expands and fades on the ground each boom.
            particles::ParticleSystem& ring = effect.AddSystem(16);
            ring.name = foundation::String{u8"shock"};
            ring.renderMode = particles::ParticleRenderMode::HorizontalBillboard; // ground-flat
            ring.blendMode = particles::ParticleBlendMode::Additive;
            ring.softParticles = false;
            ring.emitter.mode = particles::EmissionMode::Burst;
            ring.emitter.burstCount = 1;
            ring.emitter.burstInterval = 1.7f;
            ring.emitter.burstCycles = 0;
            ring.AddInitializer<particles::PositionInitializer>();
            ring.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(0.6f, 0.6f);
            ring.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{1.0f, 1.0f});
            ring.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{1.0f, 0.7f, 0.3f, 1.0f});
            ring.AddBehavior<particles::SizeOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat2::Linear(foundation::Float2{1.0f, 1.0f},
                                                       foundation::Float2{9.0f, 9.0f});
            ring.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.0f);
        }

        // Magic circle: a slow-rotating ground-flat rune ring (HorizontalBillboard + Ring emission) with
        // glyph sparks rising off it.
        static void BuildMagicCircle(particles::ParticleEffect& effect)
        {
            // Ground rune glyphs on a ring, lying flat, spinning.
            particles::ParticleSystem& rune = effect.AddSystem(400);
            rune.name = foundation::String{u8"rune"};
            rune.renderMode = particles::ParticleRenderMode::HorizontalBillboard;
            rune.blendMode = particles::ParticleBlendMode::Additive;
            rune.softParticles = false;
            rune.prewarmTime = 1.5f;
            rune.emitter.mode = particles::EmissionMode::Continuous;
            rune.emitter.spawnRate = 60.0f;
            rune.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Ring(2.6f);
            rune.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.4f, 1.8f);
            rune.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.5f, 0.5f});
            rune.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{0.3f, 0.8f, 1.0f, 1.0f});
            rune.AddInitializer<particles::RotationInitializer>();
            rune.AddBehavior<particles::RotationOverLifetimeBehavior>(); // slow spin
            rune.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::PeakAt(0.5f, 1.0f);

            // Glyph sparks rising off the ring.
            particles::ParticleSystem& spark = effect.AddSystem(600);
            spark.name = foundation::String{u8"glyph"};
            spark.renderMode = particles::ParticleRenderMode::Billboard;
            spark.blendMode = particles::ParticleBlendMode::Additive;
            spark.softParticles = false;
            spark.emitter.mode = particles::EmissionMode::Continuous;
            spark.emitter.spawnRate = 40.0f;
            spark.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Ring(2.6f);
            spark.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.0f, 1.8f);
            spark.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, 1.6f, 0.0f};
            spark.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.18f, 0.18f});
            spark.AddInitializer<particles::ColorInitializer>().color =
                particles::RangeColor::Constant(foundation::Float4{0.4f, 0.9f, 1.0f, 1.0f});
            spark.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.2f);
        }

        // Fireflies: glowing points wandering on a gentle wind + turbulence, twinkling via an oscillating
        // alpha curve. Showcases WindBehavior.
        static void BuildFireflies(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(700);
            sys.name = foundation::String{u8"fireflies"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.softParticles = false;
            sys.prewarmTime = 2.0f;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 40.0f;
            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Box(foundation::Float3{3.0f, 2.0f, 3.0f});
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(2.5f, 4.0f);
            sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, 0.2f, 0.0f};
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.16f, 0.16f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{0.8f, 1.0f, 0.3f, 1.0f}, foundation::Float4{1.0f, 0.9f, 0.2f, 1.0f});
            {
                particles::WindBehavior& w = sys.AddBehavior<particles::WindBehavior>();
                w.force = foundation::Float3{0.5f, 0.0f, 0.3f};
                w.turbulence = 1.5f;
            }
            sys.AddBehavior<particles::TurbulenceBehavior>().strength = 1.2f; // wander
            {
                particles::ParticleCurveFloat a; // twinkle: fade in, flicker, fade out
                a.AddKey(0.0f, 0.0f);
                a.AddKey(0.15f, 1.0f);
                a.AddKey(0.35f, 0.25f);
                a.AddKey(0.55f, 1.0f);
                a.AddKey(0.75f, 0.3f);
                a.AddKey(0.9f, 0.9f);
                a.AddKey(1.0f, 0.0f);
                sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve = a;
            }
        }

        // Builds the 4x4 flipbook atlas for the explosion blast: 16 frames of an expanding, dissipating
        // fiery puff, generated on the CPU (like the renderer's soft dot) and uploaded once. Stores the
        // texture/view on the app and returns the view to bind on the blast component.
        rhi::TextureView* MakeBlastAtlas(rhi::Device& dev)
        {
            constexpr foundation::u32 kCols = 4, kRows = 4, kF = 64;
            constexpr foundation::u32 W = kCols * kF, H = kRows * kF;
            foundation::Array<foundation::u8> px(foundation::DefaultAllocator());
            px.Resize(static_cast<foundation::usize>(W) * H * 4);
            for (foundation::u32 f = 0; f < kCols * kRows; ++f)
            {
                const foundation::u32 fc = f % kCols, fr = f / kCols;
                const foundation::f32 t =
                    static_cast<foundation::f32>(f) / static_cast<foundation::f32>(kCols * kRows - 1);
                const foundation::f32 rad = 0.18f + 0.85f * t;
                const foundation::f32 fade = foundation::Pow(1.0f - t, 0.6f);
                for (foundation::u32 y = 0; y < kF; ++y)
                    for (foundation::u32 x = 0; x < kF; ++x)
                    {
                        const foundation::f32 nx = (static_cast<foundation::f32>(x) + 0.5f) / kF * 2.0f - 1.0f;
                        const foundation::f32 ny = (static_cast<foundation::f32>(y) + 0.5f) / kF * 2.0f - 1.0f;
                        foundation::f32 d = foundation::Sqrt(nx * nx + ny * ny);
                        d += 0.11f * foundation::Sin(nx * 9.0f + static_cast<foundation::f32>(f) * 0.8f) +
                             0.11f * foundation::Cos(ny * 9.0f - static_cast<foundation::f32>(f) *
                                                               0.6f); // break the circle
                        foundation::f32 core = foundation::Clamp(1.0f - d / foundation::Max(rad, 1e-3f), 0.0f, 1.0f);
                        core *= core;
                        const foundation::f32 a = core * fade;
                        const foundation::f32 g = foundation::Clamp(0.35f + 0.65f * core, 0.0f,
                                                        1.0f); // white-hot core -> orange rim
                        const foundation::f32 b = foundation::Clamp(0.12f * core * core, 0.0f, 1.0f);
                        const foundation::usize i =
                            (static_cast<foundation::usize>(fr * kF + y) * W + (fc * kF + x)) * 4;
                        px[i + 0] = 255;
                        px[i + 1] = static_cast<foundation::u8>(g * 255.0f);
                        px[i + 2] = static_cast<foundation::u8>(b * 255.0f);
                        px[i + 3] = static_cast<foundation::u8>(a * 255.0f);
                    }
            }
            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = W;
            td.height = H;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"particle.blastatlas";
            if (!dev.CreateTexture(td, m_blastTex).IsOk())
            {
                return nullptr;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!dev.CreateTextureView(m_blastTex, vd, m_blastView).IsOk())
            {
                return nullptr;
            }
            if (rhi::Queue* q = dev.GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* tb = nullptr;
                if (q->CreateTransferBatch(tb).IsOk() && tb != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = W * 4;
                    layout.rowsPerImage = H;
                    tb->WriteTexture(m_blastTex, foundation::Span<const foundation::u8>{px.Data(), px.Size()},
                                     layout, rhi::Extent3D{W, H, 1});
                    (void)tb->Submit();
                    q->DestroyTransferBatch(tb);
                }
            }
            return m_blastView;
        }

        // Local-space showcase: a tight puff that rigidly follows its orbiting emitter (see OnUpdate).
        static void BuildLocal(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(2000);
            sys.name = foundation::String{u8"orbit"};
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.simulationSpace =
                particles::ParticleSpace::Local; // cloud moves as a rigid body with the emitter
            sys.prewarmTime = 1.5f;              // start already-populated
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 200.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Sphere(0.3f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.0f, 1.6f);
            sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
                foundation::Float3{0.0f, 0.4f, 0.0f};
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.18f, 0.18f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{1.0f, 0.8f, 0.3f, 1.0f}, foundation::Float4{1.0f, 0.4f, 0.1f, 1.0f});
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.2f);
        }

        // Shared mesh-particle config: tumbling shards that rise and spread. Used for BOTH mesh cells -
        // identical simulation; the only difference is the component's material (opaque solid vs. additive
        // glow, which routes to the Transparent pass). Demonstrates the mesh blend->category path.
        static void BuildMeshShards(particles::ParticleEffect& effect)
        {
            particles::ParticleSystem& sys = effect.AddSystem(2000);
            sys.name = foundation::String{u8"shards"};
            sys.renderMode = particles::ParticleRenderMode::Mesh;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 80.0f;

            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Sphere(0.4f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.6f, 2.6f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 3.5f, 0.0f};
                v.randomness = foundation::Float3{1.5f, 1.0f, 1.5f};
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.3f, 0.3f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{0.3f, 0.9f, 1.0f, 1.0f}, foundation::Float4{0.5f, 0.3f, 1.0f, 1.0f});
            sys.AddInitializer<particles::RotationInitializer>();
            sys.AddInitializer<particles::MeshOrientationInitializer>();
            sys.AddBehavior<particles::RotationOverLifetimeBehavior>(); // tumble
            sys.AddBehavior<particles::DragBehavior>().drag = 0.5f;
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.1f);
        }

        // The effect for the cooked-pipeline demo: a bright cyan additive fountain (distinct from cell 0).
        static void BuildCookedEffect(particles::ParticleEffect& fx)
        {
            particles::ParticleSystem& sys = fx.AddSystem(4000);
            sys.name = foundation::String{u8"cooked"};
            sys.blendMode = particles::ParticleBlendMode::Additive;
            sys.renderMode = particles::ParticleRenderMode::Billboard;
            sys.emitter.mode = particles::EmissionMode::Continuous;
            sys.emitter.spawnRate = 400.0f;
            sys.AddInitializer<particles::PositionInitializer>().shape =
                particles::EmissionShape::Cone(0.3f, 0.5f);
            sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                particles::RangeFloat(1.2f, 2.0f);
            {
                particles::VelocityInitializer& v =
                    sys.AddInitializer<particles::VelocityInitializer>();
                v.baseVelocity = foundation::Float3{0.0f, 10.0f, 0.0f};
                v.randomness = foundation::Float3{2.5f, 1.0f, 2.5f};
            }
            sys.AddInitializer<particles::SizeInitializer>().size =
                particles::RangeFloat2::Constant(foundation::Float2{0.3f, 0.3f});
            sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                foundation::Float4{0.2f, 1.0f, 0.9f, 1.0f}, foundation::Float4{0.4f, 0.6f, 1.0f, 1.0f});
            sys.AddBehavior<particles::GravityBehavior>().multiplier = 1.2f;
            sys.AddBehavior<particles::AlphaOverLifetimeBehavior>().curve =
                particles::ParticleCurveFloat::FadeOut(1.0f, 0.3f);
        }

        // The full authoring pipeline, live: author a ParticleEffectAsset in code -> bake it with the
        // builder into a cooked ParticleEffectResource in the per-sample content DB -> Bind it through the
        // ResourceManager -> drive a component with the cooked Proxy. This is exactly the editor->runtime
        // path (an editor would bake offline; here we bake at startup), proving it end to end in the app.
        void SetupCookedDemo(particles::ParticleEffectComponentManager& pmgr)
        {
            const foundation::StringView outputDir(
                reinterpret_cast<const foundation::utf8char*>(DRACONIC_PARTICLEFX_OUTPUT_DIR));
            if (outputDir.IsEmpty())
            {
                return;
            }
            particles::
                RegisterParticleEffectAsset(); // register cooked/asset/module types + serializable factories

            m_contentFs =
                foundation::MakeUnique<vfs::NativeFileSystem>(foundation::DefaultAllocator(), outputDir);
            m_contentDb = foundation::MakeUnique<content::ContentDatabase>(
                foundation::DefaultAllocator(), *m_contentFs, foundation::BinarySerializerFactory(),
                u8".rasset");

            // AUTHOR -> BAKE: cook the authored asset into a content-DB ParticleEffectResource.
            content::Instance* inst = m_contentDb->RootGroup()->CreateInstance(
                u8"cooked_demo", particles::ParticleEffectResource::StaticType());
            particles::ParticleEffectAsset asset;
            BuildCookedEffect(asset.Effect());
            particles::ParticleEffectAssetBuilder builder;
            editor::AssetBuildContext ctx;
            ctx.output = inst;
            ctx.db = m_contentDb.Get();
            if (!builder.Build(asset, ctx).IsOk())
            {
                return;
            }

            // LOAD: bind the cooked resource back through the manager + factory (runtime path).
            m_resources =
                foundation::MakeUnique<resource::ResourceManager>(foundation::DefaultAllocator(), *m_contentDb);
            m_resources->AddFactory(&m_pfxFactory);
            m_cookedProxy = m_resources->Bind<particles::ParticleEffectResource>(inst->Id());
            if (!m_cookedProxy)
            {
                return;
            }

            // DEMO: a component driven by the cooked resource (in front of the grid).
            m_cookedEmitter = m_scene->CreateEntity(u8"cooked");
            m_scene->SetLocalPosition(m_cookedEmitter, foundation::Float3{0.0f, 0.2f, 34.0f});
            pmgr.Add(m_cookedEmitter).SetEffect(m_cookedProxy);
        }

        // Push the current soft-particle distance to every system in an effect (0 => disabled).
        void ApplySoft(particles::ParticleEffect& fx)
        {
            for (foundation::i32 i = 0; i < fx.SystemCount(); ++i)
            {
                if (particles::ParticleSystem* s = fx.GetSystem(i))
                {
                    s->softParticles = m_softOn;
                    s->softDistance = m_softDistance;
                }
            }
        }

        void BuildHud()
        {
            ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("ParticleFX"))
            {
                particles::ParticleSystem* sys = m_effect.GetSystem(0);
                ImGui::Text("alive: %d", sys != nullptr ? sys->AliveCount() : 0);
                ImGui::Text("fps: %.0f", 1.0f / foundation::Max(m_frameSmooth, 0.0001f));
                if (sys != nullptr)
                {
                    bool emit = sys->emitter.isEmitting;
                    if (ImGui::Checkbox("emit", &emit))
                    {
                        sys->emitter.isEmitting = emit;
                    }
                    ImGui::SliderFloat("rate", &sys->emitter.spawnRate, 0.0f, 12000.0f, "%.0f/s");
                    // Fountain blend mode - live (read at extract), so all four PSO variants are eyeballable.
                    const char* kBlends[] = {"Alpha", "Additive", "Premultiplied", "Multiply"};
                    int blend = static_cast<int>(sys->blendMode);
                    if (ImGui::Combo("blend", &blend, kBlends, 4))
                    {
                        sys->blendMode = static_cast<particles::ParticleBlendMode>(blend);
                    }
                }
                // Soft-particle A/B (live - read every frame at extract). Checkbox = on/off; slider tunes
                // the fade band (kept while off, so toggling restores it). Applies to all billboard systems.
                bool changed = ImGui::Checkbox("soft particles", &m_softOn);
                changed |= ImGui::SliderFloat("soft dist", &m_softDistance, 0.0f, 4.0f, "%.2f");
                if (changed)
                {
                    ApplySoft(m_effect);
                    ApplySoft(m_embers);
                    ApplySoft(m_haze);
                }
                ImGui::TextDisabled("WASD/RMB fly. 4x4 grid, row by row:");
                ImGui::TextDisabled("fountain / mesh-solid / mesh-glow / embers");
                ImGui::TextDisabled("haze / trail / collision / orbit");
                ImGui::TextDisabled("smoke / fire / campfire / fireworks");
                ImGui::TextDisabled("tornado / explosion / magic-circle / fireflies");
            }
            ImGui::End();
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera;
        scene::EntityHandle m_emitter;
        scene::EntityHandle m_debrisEmitter;
        scene::EntityHandle m_emberEmitter;
        scene::EntityHandle m_hazeEmitter;
        scene::EntityHandle m_trailEmitter;
        scene::EntityHandle m_collideEmitter;
        scene::EntityHandle m_localEmitter;
        scene::EntityHandle m_glowEmitter;
        scene::EntityHandle m_smokeEmitter;
        scene::EntityHandle m_fireEmitter;
        scene::EntityHandle m_campfireEmitter;
        scene::EntityHandle m_fireworksEmitter;
        scene::EntityHandle m_tornadoEmitter;
        scene::EntityHandle m_explosionEmitter;
        scene::EntityHandle m_explosionFxEmitter;
        scene::EntityHandle m_magicEmitter;
        scene::EntityHandle m_firefliesEmitter;
        particles::ParticleEffect m_effect;
        particles::ParticleEffect m_debris;
        particles::ParticleEffect m_embers;
        particles::ParticleEffect m_haze;
        particles::ParticleEffect m_trail;
        particles::ParticleEffect m_collide;
        particles::ParticleEffect m_local;
        particles::ParticleEffect m_meshGlow;
        particles::ParticleEffect m_smoke;
        particles::ParticleEffect m_fire;
        particles::ParticleEffect m_campfire;
        particles::ParticleEffect m_fireworks;
        particles::ParticleEffect m_tornado;
        particles::ParticleEffect m_explosion;
        particles::ParticleEffect m_explosionFx;
        particles::ParticleEffect m_magic;
        particles::ParticleEffect m_fireflies;
        rhi::Texture* m_blastTex = nullptr; // flipbook atlas for the explosion (owned)
        rhi::TextureView* m_blastView = nullptr;
        // Cooked-pipeline demo: content DB + manager + factory + the bound cooked effect.
        foundation::UniquePtr<vfs::NativeFileSystem> m_contentFs;
        foundation::UniquePtr<content::ContentDatabase> m_contentDb;
        foundation::UniquePtr<resource::ResourceManager> m_resources;
        particles::ParticleEffectFactory m_pfxFactory;
        resource::Proxy<particles::ParticleEffectResource> m_cookedProxy;
        scene::EntityHandle m_cookedEmitter;
        samples::FlyCamera m_fly;
        foundation::f32 m_frameSmooth = 0.016f;
        foundation::f32 m_orbitTime = 0.0f;    // drives the local-space emitter orbit
        bool m_softOn = true;            // soft-particle on/off (HUD checkbox)
        foundation::f32 m_softDistance = 2.0f; // soft-particle fade band (HUD slider)
    };
}

int main(int, char**)
{
    auto shell = shell::CreateShell();
    graphics::GraphicsDeviceDesc gpuDesc{};
    auto gpu = graphics::CreateGraphicsDevice(gpuDesc);
    graphics::GraphicsDevice* device = gpu.HasValue() ? gpu.Value().Get() : nullptr;

    ParticleFXApp app;
    return runtime::RunApplication(app, *shell, device);
}
