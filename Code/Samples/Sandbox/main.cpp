// Sandbox - the running dev harness. It extends DefaultApplication (which registers
// the SceneSubsystem + RenderSubsystem and renders active scenes each frame), creates a
// scene with two spinning cube grids (instanced + distinct), and lets the engine draw it.
// As the renderer grows, this is where we exercise it.

#include "Draconic.Foundation/Prelude.h"
#include "imgui.h" // Dear ImGui (debug UI) - used directly; the engine integration is draconic.imgui

import draconic.foundation;
import draconic.rhi; // offscreen render target (Texture / ResourceState / Blit)
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.engine.defaultapp; // DefaultApplication (scene + render subsystems)
import draconic.scene;
import draconic.engine.scene;
import draconic.engine.render; // MeshComponent / CameraComponent + their managers
import draconic.engine.animation; // SkeletalAnimation/AnimationGraph components (engine-driven skinning)
import draconic.imgui;               // ImguiSubsystem (debug UI)
import draconic.particles;           // ParticleEffect + the CPU sim (the campfire demo)
import draconic.engine.particles;    // ParticleEffectComponent + the subsystem
import draconic.image;               // Image (HDR equirect pixels)
import draconic.image.io;            // LoadImage
import draconic.render;              // ViewCamera / ViewportRect (split-screen overrides)
import draconic.geometry;
import draconic.geometry.resource; // StaticMeshFactory + StaticMesh product
import draconic.materials;
import draconic.materials.resource; // MaterialFactory (cooked materials)
import draconic.texture.resource;   // TextureFactory (cooked textures)
import draconic.texture.editor;     // TextureImporter / TextureAssetBuilder
import draconic.editor;             // AssetBuildContext (cook an asset into a content instance)
import draconic.animation.resource; // Skeleton/AnimationClip factories
import draconic.vfs;                // NativeFileSystem mount for the content DB
import draconic.content;            // ContentDatabase (cooked-resource output)
import draconic.resource;           // ResourceManager + Proxy
import draconic.model;              // ModelLoadResult
import draconic.modelimporter;      // LoadAndCook + ImportedModel manifest
import draconic.animation;          // AnimationPlayer (drives GPU skinning)

#include "../Common/FlyCamera.h" // shared free-fly camera (uses the imported runtime/core types)

#ifndef DRACONIC_SANDBOX_MODEL_DIR
#define DRACONIC_SANDBOX_MODEL_DIR ""
#endif
#ifndef DRACONIC_SANDBOX_OUTPUT_DIR
#define DRACONIC_SANDBOX_OUTPUT_DIR ""
#endif

namespace foundation = draconic::foundation;
namespace samples = draconic::samples;
namespace rhi = draconic::rhi;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace texture = draconic::texture;
namespace vfs = draconic::vfs;
namespace content = draconic::content;
namespace resource = draconic::resource;
namespace model = draconic::model;
namespace modelimporter = draconic::modelimporter;
namespace animation = draconic::animation;
namespace imgui = draconic::imgui;
namespace particles = draconic::particles;

namespace
{
    class SandboxApp final : public runtime::DefaultApplication
    {
    public:
        // Run uncapped (vsync off) so the FPS/frame-ms readout reflects real CPU+GPU cost, not the
        // display refresh - matches AnimStressTest. The image may tear; fine for a dev sandbox.
        graphics::RenderWindowDesc MainRenderWindow() const override
        {
            graphics::RenderWindowDesc d;
            d.presentMode = rhi::PresentMode::Immediate;
            return d;
        }

        // Register the standard subsystems (DefaultApplication) + the ImGui debug UI on top.
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
            // CreateScene triggers the RenderSubsystem to inject the render managers. The scene lives
            // on the primary instance's group (the app renders instance scenes; there is no default).
            m_scene = PrimaryScenes().CreateScene(u8"sandbox");

            // Per-scene environment: drives IBL (split-sum ambient from a procedural sky) + the flat
            // fallback. The procedural sky's gradient + sun feed the SH9 diffuse + prefiltered specular.
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                render::EnvironmentSettings& e = env->Environment();
                e.ambientColor = foundation::Color{0.12f, 0.16f, 0.28f, 1.0f}; // flat fallback (IBL off)
                e.ambientIntensity = 0.35f;
                e.skyMode = render::SkyMode::Procedural;
                e.skyIntensity = 0.7f; // dimmer sky -> less washed-out IBL ambient
                e.skyHorizon = foundation::Color{0.62f, 0.70f, 0.85f, 1.0f};
                e.skyZenith = foundation::Color{0.18f, 0.34f, 0.68f, 1.0f};
                e.skyGround = foundation::Color{0.28f, 0.26f, 0.24f, 1.0f};
                e.sunIntensity = 1.0f;
            }

            // Load an HDR equirectangular environment so F5 can cycle to it (mode starts Procedural).
            if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
            {
                // Default exposure below 1.0 - the procedural sky + IBL ambient are bright, so the AgX
                // tonemap washes out at 1.0. Tune live via the Environment window's Exposure slider.
                render->SetExposure(0.5f);
                foundation::String hdrPath = foundation::Format(
                    u8"{}/BlueSky.hdr", foundation::StringView(reinterpret_cast<const foundation::utf8char*>(
                                            DRACONIC_SANDBOX_ENV_DIR)));
                draconic::image::Image img;
                if (draconic::image::io::LoadImage(hdrPath.AsView(), img).IsOk() &&
                    img.Format() == draconic::image::PixelFormat::RGBA32F)
                {
                    const foundation::Span<const foundation::u8> px = img.PixelData();
                    const foundation::Span<const foundation::f32> rgba{
                        reinterpret_cast<const foundation::f32*>(px.Data()),
                        px.Size() / sizeof(foundation::f32)};
                    render->SetSkyEquirect(img.Width(), img.Height(), rgba);
                    foundation::ConsoleWrite(
                        foundation::Format(
                            u8"Sandbox: loaded HDR sky {}x{} (F5 / Sky Mode combo to use it)\n",
                            img.Width(), img.Height())
                            .AsView());
                }
                else
                {
                    foundation::ConsoleWrite(foundation::Format(u8"Sandbox: FAILED to load HDR sky from {}\n",
                                                    hdrPath.AsView())
                                           .AsView());
                }

                // Cubemap source: point at ONE face; the importer detects the other 5 (px/nx/...) and
                // loads + combines them. (Explicit 6-path LoadCubemap also works.)
                foundation::String oneFace =
                    foundation::Format(u8"{}/cube_sky/px.png",
                                 foundation::StringView(reinterpret_cast<const foundation::utf8char*>(
                                     DRACONIC_SANDBOX_ENV_DIR)));
                foundation::Array<foundation::String> facePaths;
                if (texture::TextureImporter::DetectCubemapFaces(oneFace.AsView(), facePaths)
                        .IsOk() &&
                    facePaths.Size() == 6)
                {
                    foundation::StringView faceViews[6];
                    for (int i = 0; i < 6; ++i)
                    {
                        faceViews[i] = facePaths[static_cast<foundation::usize>(i)].AsView();
                    }
                    foundation::Array<foundation::u8> cube;
                    foundation::u32 cubeFace = 0;
                    if (texture::TextureImporter::LoadCubemap(
                            foundation::Span<const foundation::StringView>{faceViews, 6}, cube, cubeFace)
                            .IsOk())
                    {
                        render->SetSkyCubemap(cubeFace,
                                              foundation::Span<const foundation::u8>{cube.Data(), cube.Size()});
                        foundation::ConsoleWrite(foundation::Format(u8"Sandbox: loaded cubemap sky {}x{} x6\n",
                                                        cubeFace, cubeFace)
                                               .AsView());
                    }
                }
            }

            // camera, pulled back along +Z looking at the origin (down -Z by default)
            // Raised + pitched down so the horizontal floor (lights above it) is clearly in view,
            // with the cube grids standing on it. Pitch ~28 deg below horizontal (looks toward the
            // scene center). Default camera looks down -Z; rotating about +X by -pitch tilts it down.
            m_camera = m_scene->CreateEntity(u8"camera");
            m_scene->SetLocalPosition(m_camera, foundation::Float3{0.0f, 21.0f, 30.0f});
            foundation::Transform camT = m_scene->GetLocalTransform(m_camera);
            camT.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -0.48f);
            m_scene->SetLocalTransform(m_camera, camT);
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                render::CameraComponent& cam = cameras->Add(m_camera); // default 60deg perspective
                cam.clearColor =
                    foundation::Color{0.02f, 0.02f, 0.03f, 1.0f}; // dark backdrop so the lit scene reads
            }

            // A large horizontal floor (Plane normal = +Y) under the scene - the point lights hover
            // above it and cast visible pools on it. The two cube grids stand on the floor.
            if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
            {
                m_floorEntity = m_scene->CreateEntity(u8"floor");
                m_scene->SetLocalPosition(m_floorEntity, foundation::Float3{0.0f, 0.0f, 0.0f});
                render::MeshComponent& fmc = meshes->Add(m_floorEntity);
                fmc.mesh = geometry::Primitives::Plane(120.0f, 120.0f);
                // Semi-glossy DIELECTRIC green floor (non-metallic, moderate roughness): shadows read
                // clearly (not washed out by a mirror-metal reflection) while SSR still shows softly.
                // Metallic/roughness are LIVE-tweakable from the Environment window (SSR eye test).
                ApplyFloorMaterial();

                foundation::RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(0.35f);
                BuildGrid(*meshes, cube, /*originX*/ -8.0f, /*instanced*/ true);
                BuildGrid(*meshes, cube, /*originX*/ 8.0f, /*instanced*/ false);

                // A row of cubes resting EXACTLY on the floor (bottom face flush at y=-7) - a static
                // reference for judging shadow contact / peter-panning (the grids float in the air).
                constexpr foundation::f32 kBoxSize = 2.5f, kFloorY = 0.0f;
                foundation::RefPtr<geometry::StaticMesh> box = geometry::Primitives::Cube(kBoxSize);
                foundation::RefPtr<materials::Material> boxMat = materials::CreatePBR(
                    u8"lit", foundation::Float4{0.85f, 0.55f, 0.2f, 1.0f}, 0.0f, 0.5f);
                for (int k = 0; k < 4; ++k)
                {
                    scene::EntityHandle b = m_scene->CreateEntity(u8"floorBox");
                    m_scene->SetLocalPosition(b,
                                              foundation::Float3{-7.5f + 5.0f * static_cast<foundation::f32>(k),
                                                           kFloorY + kBoxSize * 0.5f, 10.0f});
                    render::MeshComponent& bmc = meshes->Add(b);
                    bmc.mesh = box;
                    bmc.SetMaterial(boxMat);
                }

                // Spheres resting ON the floor (bottom flush) - their contact shadow is mostly hidden
                // under the sphere, so you see only the "half" extending away from the sun (vs the
                // floating grids, whose full shadow ellipse is visible on the ground).
                constexpr foundation::f32 kBallR = 1.25f;
                foundation::RefPtr<geometry::StaticMesh> ball =
                    geometry::Primitives::Sphere(kBallR, 24, 12);
                // Metal spheres with INCREASING roughness across the row (0.05 -> 0.59), all fully metallic,
                // so the probe reflection goes mirror-sharp -> blurry - showcasing the GGX roughness prefilter.
                for (int k = 0; k < 4; ++k)
                {
                    foundation::RefPtr<materials::Material> ballMat =
                        materials::CreatePBR(u8"lit", foundation::Float4{0.90f, 0.90f, 0.92f, 1.0f}, 1.0f,
                                             0.05f + 0.18f * static_cast<foundation::f32>(k));
                    scene::EntityHandle s = m_scene->CreateEntity(u8"floorBall");
                    m_scene->SetLocalPosition(s,
                                              foundation::Float3{-7.5f + 5.0f * static_cast<foundation::f32>(k),
                                                           kFloorY + kBallR, 16.0f});
                    render::MeshComponent& smc = meshes->Add(s);
                    smc.mesh = ball;
                    smc.SetMaterial(ballMat);
                }

                // Transparent (alpha-blended) spheres hovering in front of the opaque row - exercises the
                // transparent path: routed to the Transparent category (blend mode), sorted back-to-front,
                // blended with depth-test/no-write. Base-color alpha (<1) makes them see-through.
                foundation::RefPtr<materials::Material> glassMat = materials::CreatePBR(
                    u8"lit", foundation::Float4{0.35f, 0.6f, 0.95f, 0.4f}, 0.0f, 0.12f);
                glassMat->pipeline.blendMode = materials::BlendMode::AlphaBlend;
                glassMat->pipeline.depthMode =
                    materials::DepthMode::ReadOnly; // test against opaque depth, don't write
                for (int k = 0; k < 3; ++k)
                {
                    scene::EntityHandle g = m_scene->CreateEntity(u8"glassBall");
                    // Moved well off to the left (x ~ -26, outside the probe box) + higher, to isolate them
                    // from the chrome spheres while inspecting reflections.
                    m_scene->SetLocalPosition(
                        g, foundation::Float3{-26.0f, kFloorY + 6.0f + 3.0f * static_cast<foundation::f32>(k),
                                        16.0f});
                    render::MeshComponent& gmc = meshes->Add(g);
                    gmc.mesh = ball;
                    gmc.SetMaterial(glassMat);
                }

                // Masked (alpha-tested) spheres: a checkerboard alpha-cutout texture drives the discard,
                // so they render with real holes (opaque where solid, gone where the texture alpha is 0).
                if (rhi::Device* dev =
                        (host.Graphics() != nullptr) ? host.Graphics()->Raw() : nullptr)
                {
                    if (rhi::TextureView* cutout = CreateCutoutTexture(*dev))
                    {
                        foundation::RefPtr<materials::Material> maskMat = materials::CreatePBR(
                            u8"lit", foundation::Float4{0.95f, 0.8f, 0.3f, 1.0f}, 0.0f, 0.45f);
                        maskMat->pipeline.blendMode = materials::BlendMode::Masked;
                        maskMat->SetDefaultTexture(u8"AlbedoMap", cutout); // alpha holes -> discard
                        for (int k = 0; k < 3; ++k)
                        {
                            scene::EntityHandle m = m_scene->CreateEntity(u8"maskedBall");
                            m_scene->SetLocalPosition(
                                m, foundation::Float3{-5.0f + 5.0f * static_cast<foundation::f32>(k),
                                                kFloorY + 3.5f, 24.0f});
                            render::MeshComponent& mmc = meshes->Add(m);
                            mmc.mesh = ball;
                            mmc.SetMaterial(maskMat);
                        }
                    }
                }
            }

            // Lights: a dim directional key (down-forward) + a bright point light that orbits the
            // grid in OnUpdate, so the per-light forward shade is visible (moving highlight).
            if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
            {
                scene::EntityHandle key = m_scene->CreateEntity(u8"keyLight");
                m_keyLight = key;
                ApplyKeyLightDir(); // pitch/yaw -> entity rotation (steeper downward tilt by default)
                render::LightComponent& kl = lights->Add(key);
                kl.type = render::LightType::Directional;
                kl.color = foundation::Color{0.4f, 0.5f, 0.7f, 1.0f};
                kl.intensity = 0.5f;    // key light: bright enough that its shadow reads
                kl.castsShadows = true; // directional CSM (5.2) + spot (5.3a) + point cube (5.3b)

                // A field of point lights hovering above the floor (X-Z grid) - the clustered
                // light-culling demo. Each fragment only evaluates the lights in its froxel, so this
                // scales far better than an all-lights loop. Each casts a colored pool on the floor.
                constexpr int kCols = 6, kRows = 3; // 18 point lights over the floor
                for (int j = 0; j < kRows; ++j)
                {
                    for (int i = 0; i < kCols; ++i)
                    {
                        scene::EntityHandle e = m_scene->CreateEntity(u8"pointLight");
                        const foundation::f32 fi = static_cast<foundation::f32>(i) / (kCols - 1);
                        const foundation::f32 fj = static_cast<foundation::f32>(j) / (kRows - 1);
                        const foundation::Float3 base{-15.0f + 30.0f * fi, 5.5f,
                                                -2.0f + 16.0f * fj}; // hover above floor
                        m_scene->SetLocalPosition(e, base);
                        render::LightComponent& pl = lights->Add(e);
                        pl.type = render::LightType::Point;
                        pl.color =
                            foundation::Color{0.4f + 0.6f * fi, 0.4f + 0.6f * fj, 1.0f - 0.6f * fi, 1.0f};
                        pl.intensity = 14.0f;
                        pl.range = 8.0f; // floor pool radius ~= sqrt(range^2 - dist^2)
                        m_pointLights.PushBack(e);
                        m_lightBases.PushBack(base);
                    }
                }

                // A bright spot light overhead, aimed down at the floor boxes/spheres - the phase 5.3
                // atlas spot-shadow demo. Its cone casts sharp shadows of the resting boxes onto the
                // floor (distinct from the directional CSM), packed into the local-shadow atlas.
                scene::EntityHandle spot = m_scene->CreateEntity(u8"spotLight");
                m_scene->SetLocalPosition(
                    spot, foundation::Float3{0.0f, 14.0f,
                                       13.0f}); // between box row (z=10) and sphere row (z=16)
                foundation::Transform st = m_scene->GetLocalTransform(spot);
                st.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f},
                                                              -1.5f); // nearly straight down
                m_scene->SetLocalTransform(spot, st);
                render::LightComponent& sl = lights->Add(spot);
                sl.type = render::LightType::Spot;
                sl.color = foundation::Color{1.0f, 0.92f, 0.78f, 1.0f}; // warm, to contrast the blue key
                sl.intensity = 120.0f; // inverse-square over ~14u to the floor
                sl.range = 30.0f;
                sl.innerAngle = 0.55f;
                sl.outerAngle = 0.75f;  // wide cone: cover both the box + sphere rows
                sl.castsShadows = true; // spot atlas shadow caster (5.3a)
                sl.shadowUpdate =
                    render::ShadowUpdateMode::Static; // static scene -> cached atlas layer (5.4b)

                // A shadow-casting POINT light hovering among the floor boxes/spheres - the phase 5.3b
                // cube-shadow demo. Its 6 atlas faces cast shadows radially (onto the floor + box sides).
                scene::EntityHandle pt = m_scene->CreateEntity(u8"shadowPoint");
                m_scene->SetLocalPosition(pt, foundation::Float3{4.0f, 5.0f, 13.0f});
                render::LightComponent& pls = lights->Add(pt);
                pls.type = render::LightType::Point;
                pls.color =
                    foundation::Color{0.5f, 1.0f, 0.6f, 1.0f}; // green, distinct from the warm spot
                pls.intensity = 28.0f;
                pls.range = 16.0f;
                pls.castsShadows = true; // point cube atlas caster (5.3b)
            }

            // TWO reflection probes, side by side with an overlap in the middle, to show multi-probe blending:
            // each captures from its own center (so their local reflections differ), and fragments in the
            // overlap blend the two by influence weight. Realtime -> round-robin re-captures one per frame.
            if (auto* probes = m_scene->GetSystem<render::ReflectionProbeComponentManager>())
            {
                m_probeEntity = m_scene->CreateEntity(
                    u8"reflectionProbeL"); // left probe (F6/UI toggles this one)
                m_scene->SetLocalPosition(m_probeEntity, foundation::Float3{-8.0f, 6.0f, 16.0f});
                render::ReflectionProbeComponent& rpL = probes->Add(m_probeEntity);
                rpL.halfExtents = foundation::Float3{12.0f, 12.0f, 14.0f}; // covers x[-20,4]
                rpL.blendDistance = 4.0f;
                rpL.update = render::ProbeUpdateMode::Realtime;

                scene::EntityHandle probeR =
                    m_scene->CreateEntity(u8"reflectionProbeR"); // right probe
                m_scene->SetLocalPosition(probeR, foundation::Float3{8.0f, 6.0f, 16.0f});
                render::ReflectionProbeComponent& rpR = probes->Add(probeR);
                rpR.halfExtents =
                    foundation::Float3{12.0f, 12.0f, 14.0f}; // covers x[-4,20] -> overlap x[-4,4]
                rpR.blendDistance = 4.0f;
                rpR.update = render::ProbeUpdateMode::Realtime;
            }

            LoadImportedModel(host); // cook + spawn a glTF model through the resource pipeline

            BuildParticleDemo(); // campfire + sparks: the particle path in the full-scene mix

            foundation::ConsoleWrite(
                u8"Sandbox: split-screen - click a half to fly its camera (WASD/QE, Shift fast); "
                u8"RMB-drag looks in the hovered half. G cycles the Character graph. Close to "
                u8"exit.\n");
        }

        // Particles in the full-scene mix (the renderer-exercise gap Sandbox had): a campfire
        // (additive billboards + color/size-over-life) with spark TRAILS (the ribbon path),
        // sharing the frame with meshes/shadows/AO/TAA/probes - so particle regressions show
        // up here, not only in the dedicated ParticleFX showcase.
        void BuildParticleDemo()
        {
            auto* pmgr = m_scene->GetSystem<particles::ParticleEffectComponentManager>();
            if (pmgr == nullptr)
            {
                return;
            }
            {
                particles::ParticleSystem& sys = m_campfire.AddSystem(5000);
                sys.name = foundation::String{u8"campfire"};
                sys.blendMode = particles::ParticleBlendMode::Additive;
                sys.renderMode = particles::ParticleRenderMode::Billboard;
                sys.emitter.mode = particles::EmissionMode::Continuous;
                sys.emitter.spawnRate = 700.0f;
                sys.AddInitializer<particles::PositionInitializer>().shape =
                    particles::EmissionShape::Sphere(0.35f);
                sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                    particles::RangeFloat(0.9f, 1.8f);
                {
                    particles::VelocityInitializer& v =
                        sys.AddInitializer<particles::VelocityInitializer>();
                    v.baseVelocity = foundation::Float3{0.0f, 3.2f, 0.0f};
                    v.randomness = foundation::Float3{0.8f, 0.8f, 0.8f};
                }
                sys.AddInitializer<particles::SizeInitializer>().size =
                    particles::RangeFloat2::Constant(foundation::Float2{0.5f, 0.5f});
                sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                    foundation::Float4{1.0f, 0.55f, 0.15f, 1.0f}, foundation::Float4{1.0f, 0.8f, 0.35f, 1.0f});
                sys.AddBehavior<particles::ColorOverLifetimeBehavior>().curve =
                    particles::ParticleCurveColor::FadeAlpha(foundation::Float4{1.0f, 0.45f, 0.1f, 1.0f},
                                                             0.3f);
                sys.AddBehavior<particles::SizeOverLifetimeBehavior>().curve =
                    particles::ParticleCurveFloat2::Linear(foundation::Float2{0.55f, 0.55f},
                                                           foundation::Float2{0.08f, 0.08f});
            }
            {
                particles::ParticleSystem& sys = m_campfire.AddSystem(600);
                sys.name = foundation::String{u8"campfire-sparks"};
                sys.renderMode = particles::ParticleRenderMode::Trail;
                sys.blendMode = particles::ParticleBlendMode::Additive;
                sys.emitter.mode = particles::EmissionMode::Continuous;
                sys.emitter.spawnRate = 16.0f;
                sys.AddInitializer<particles::PositionInitializer>().shape =
                    particles::EmissionShape::Sphere(0.2f);
                sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
                    particles::RangeFloat(1.0f, 1.8f);
                {
                    particles::VelocityInitializer& v =
                        sys.AddInitializer<particles::VelocityInitializer>();
                    v.baseVelocity = foundation::Float3{0.0f, 5.0f, 0.0f};
                    v.randomness = foundation::Float3{2.5f, 1.5f, 2.5f};
                }
                sys.AddInitializer<particles::SizeInitializer>().size =
                    particles::RangeFloat2::Constant(foundation::Float2{0.12f, 0.12f});
                sys.AddInitializer<particles::ColorInitializer>().color = particles::RangeColor(
                    foundation::Float4{1.0f, 0.7f, 0.2f, 1.0f}, foundation::Float4{1.0f, 0.45f, 0.1f, 1.0f});
                sys.AddBehavior<particles::GravityBehavior>().multiplier = 0.9f;
            }
            m_campfireEntity = m_scene->CreateEntity(u8"campfire");
            m_scene->SetLocalPosition(m_campfireEntity, foundation::Float3{-10.0f, 0.2f, 8.0f});
            pmgr->Add(m_campfireEntity).SetEffect(m_campfire);
        }

        // The model-import seam: open the cooked-resource output DB, register the geometry factory,
        // load+cook a glTF file through the importer, then spawn its node hierarchy as entities whose
        // MeshComponents reference the cooked StaticMesh resources. This is the clean runtime cook seam
        // the design calls for - an editor would cook offline and the runtime would only Bind, but the
        // wiring (factory -> Bind -> render) is identical.
        void LoadImportedModel(runtime::IApplicationHost& host)
        {
            const foundation::StringView outputDir(
                reinterpret_cast<const foundation::utf8char*>(DRACONIC_SANDBOX_OUTPUT_DIR));
            const foundation::StringView modelDir(
                reinterpret_cast<const foundation::utf8char*>(DRACONIC_SANDBOX_MODEL_DIR));
            if (outputDir.IsEmpty() || modelDir.IsEmpty())
            {
                return;
            }

            // Output DB (cooked resources) + resource manager + the factories. ModelFactory builds the
            // manifest into a ModelResource, resolving its meshes/materials/textures (dependency edges).
            m_contentFs =
                foundation::MakeUnique<vfs::NativeFileSystem>(foundation::DefaultAllocator(), outputDir);
            m_contentDb = foundation::MakeUnique<content::ContentDatabase>(
                foundation::DefaultAllocator(), *m_contentFs, foundation::BinarySerializerFactory(),
                u8".rasset");
            m_resources =
                foundation::MakeUnique<resource::ResourceManager>(foundation::DefaultAllocator(), *m_contentDb);
            m_resources->AddFactory(&m_meshFactory);
            m_resources->AddFactory(&m_skinnedMeshFactory);
            m_resources->AddFactory(&m_modelFactory);
            m_resources->AddFactory(&m_materialFactory);
            m_resources->AddFactory(&m_skeletonFactory);
            m_resources->AddFactory(&m_clipFactory);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                m_textureFactory = foundation::MakeUnique<texture::TextureFactory>(
                    foundation::DefaultAllocator(), *gfx->Raw());
                m_resources->AddFactory(m_textureFactory.Get());
            }
            model::RegisterModelResourceTypes(); // make the cooked types deserializable

            // A few imported models side by side (runtime cook seam; an editor would cook offline + Bind).
            SpawnModel(u8"Duck", foundation::Format(u8"{}/Duck/glTF/Duck.gltf", modelDir).AsView(),
                       foundation::Float3{-5.0f, 3.0f, 6.0f});
            SpawnModel(u8"Fox", foundation::Format(u8"{}/Fox/glTF/Fox.gltf", modelDir).AsView(),
                       foundation::Float3{5.0f, 0.0f, 6.0f});
            // The Character is driven by an AnimationGraph (a state machine over its clips) rather than a
            // single clip - press G to fire the graph's "Next" trigger and cross-fade to the next state.
            SpawnModel(
                u8"Char",
                foundation::Format(u8"{}/QuaterniusCharacter/glTF/Character.gltf", modelDir).AsView(),
                foundation::Float3{0.0f, 0.0f, 12.0f}, /*useGraph=*/true);

            SpawnSpriteDemo(); // billboards (all orientation + blend modes) using the imported logo
            SpawnDecalDemo();  // project the same logo onto the floor (screen-space decal)
        }

        // Project the imported logo onto the floor as a screen-space decal: a box straddling the floor,
        // oriented so its local +Z points straight down (-Y). The decal "sprays" onto whatever the box
        // covers (here, the floor at y=0).
        void SpawnDecalDemo()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            auto* decals = m_scene->GetSystem<render::DecalComponentManager>();
            if (decals == nullptr)
            {
                return;
            }
            rhi::TextureView* logo = m_logoTex ? m_logoTex->View() : LoadLogoTexture();
            if (logo == nullptr)
            {
                return;
            }

            scene::EntityHandle e = m_scene->CreateEntity(u8"floorDecal");
            render::DecalComponent& dc = decals->Add(e);
            dc.texture = logo;
            dc.size =
                foundation::Float3{8.0f, 8.0f, 6.0f}; // 8x8 floor footprint; 6-unit box depth spans y=0
            dc.fadeStart = 0.0f;
            dc.fadeEnd = 1.4f; // ~80deg - fully faded on near-vertical surfaces
            // Sit on a clear patch of floor (props start at z>=6) and rotate so +Z projects down.
            foundation::Transform t;
            t.position = foundation::Float3{0.0f, 0.0f, -3.0f};
            t.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f},
                                                         1.5707963f); // +90deg about X
            m_scene->SetLocalTransform(e, t);
        }

        // Import the Draconic logo PNG through the asset pipeline (TextureImporter -> cook into the content
        // DB -> Bind the runtime Texture) and return its GPU view. The Proxy is stored to keep it alive.
        [[nodiscard]] rhi::TextureView* LoadLogoTexture()
        {
            if (m_resources.Get() == nullptr || m_textureFactory.Get() == nullptr)
            {
                return nullptr;
            }
            const foundation::StringView imageDir(
                reinterpret_cast<const foundation::utf8char*>(DRACONIC_SANDBOX_IMAGE_DIR));

            texture::TextureAsset asset;
            texture::TextureImporter::Import2D(u8"draconic_logo_no_text.png",
                                               draconic::image::ImageColorSpace::Srgb,
                                               asset); // sRGB albedo

            // Create a content instance of the cooked record type, then cook the asset into it.
            content::Group* root = m_contentDb->RootGroup();
            content::Instance* inst =
                root->CreateInstance(u8"logo.tex", texture::TextureResource::StaticType());
            if (inst == nullptr)
            {
                return nullptr;
            }
            texture::TextureAssetBuilder builder;
            draconic::vfs::NativeFileSystem imageMount(imageDir);
            draconic::editor::AssetBuildContext ctx;
            ctx.sources = &imageMount; // the mount resolves the PNG
            ctx.output = inst;
            if (!builder.Build(asset, ctx).IsOk())
            {
                return nullptr;
            }

            // The runtime factory product is a texture::Texture (not the cooked record); it owns the GPU view.
            m_logoTex = m_resources->Bind<texture::Texture>(inst->Id());
            return m_logoTex ? m_logoTex->View() : nullptr;
        }

        // One textured billboard per (orientation, blend) combination, in a row, so the modes are easy to
        // compare by flying the camera around (click a split half to fly its camera):
        //   0 camera-facing (full), 1 camera-facing about world-Y, 2 world-aligned (fixed XY); + additive.
        void SpawnSpriteDemo()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            auto* sprites = m_scene->GetSystem<render::SpriteComponentManager>();
            if (sprites == nullptr)
            {
                return;
            }
            rhi::TextureView* logo = LoadLogoTexture();
            if (logo == nullptr)
            {
                foundation::ConsoleWrite(u8"Sandbox: sprite demo skipped (logo import failed)\n");
                return;
            }

            using render::SpriteOrientation;
            struct Variant
            {
                const char8_t* name;
                SpriteOrientation mode;
                bool additive;
                foundation::Color tint;
            };
            const Variant variants[] = {
                {u8"spriteFace", SpriteOrientation::CameraFacing, false,
                 foundation::Color{1.0f, 1.0f, 1.0f, 1.0f}},
                {u8"spriteFaceY", SpriteOrientation::CameraFacingY, false,
                 foundation::Color{1.0f, 1.0f, 1.0f, 1.0f}},
                {u8"spriteWorld", SpriteOrientation::WorldAligned, false,
                 foundation::Color{1.0f, 1.0f, 1.0f, 1.0f}},
                {u8"spriteAdd", SpriteOrientation::CameraFacing, true,
                 foundation::Color{2.6f, 2.2f, 1.4f,
                             1.0f}}, // additive glow (HDR tint so it reads over the bright scene)
                {u8"spriteTint", SpriteOrientation::CameraFacing, false,
                 foundation::Color{0.4f, 0.9f, 1.0f, 1.0f}}, // tinted cyan
            };
            const foundation::f32 spacing = 4.5f;
            const foundation::f32 x0 =
                -0.5f * spacing *
                static_cast<foundation::f32>((sizeof(variants) / sizeof(variants[0])) - 1);
            for (foundation::usize i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i)
            {
                const Variant& v = variants[i];
                scene::EntityHandle e = m_scene->CreateEntity(v.name);
                render::SpriteComponent& sp = sprites->Add(e);
                sp.texture = logo;
                sp.size = foundation::Float2{3.0f, 3.0f};
                sp.tint = v.tint;
                sp.orientation = v.mode;
                sp.additive = v.additive;
                // Above the cube grids (which top out at y=13) and in front of them (z=-8), so the row
                // reads as a clear banner over the scene and the billboard modes are easy to compare.
                m_scene->SetLocalPosition(
                    e, foundation::Float3{x0 + spacing * static_cast<foundation::f32>(i), 16.0f, -8.0f});
            }
        }

        // Cook + bind + spawn one model, placed at `position` and auto-fit to a target size. Each model
        // spawns its node hierarchy (local TRS + parent links) under a scaled model-root entity; mesh
        // nodes get a MeshComponent referencing the cooked StaticMesh + material.
        void SpawnModel(foundation::StringView prefix, foundation::StringView path, foundation::Float3 position,
                        bool useGraph = false)
        {
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            if (meshes == nullptr || m_contentDb.Get() == nullptr)
            {
                return;
            }

            foundation::Guid modelGuid;
            const model::ModelLoadResult r =
                modelimporter::LoadAndCook(path, *m_contentDb, prefix, modelGuid);
            if (r != model::ModelLoadResult::Ok)
            {
                foundation::ConsoleWrite(foundation::Format(u8"Sandbox: model import failed ({}) for {}\n",
                                                static_cast<foundation::u32>(r), prefix));
                return;
            }
            resource::Proxy<model::ModelResource> model =
                m_resources->Bind<model::ModelResource>(modelGuid);
            if (!model)
            {
                foundation::ConsoleWrite(u8"Sandbox: model bind failed\n");
                return;
            }

            // Auto-fit: the model-root scales the model's largest extent to a target size (models come in
            // wildly different unit scales - the Duck is ~100 units, the Fox ~150).
            constexpr foundation::f32 kTargetSize = 6.0f;
            const foundation::Float3 extent = model->boundsMax - model->boundsMin;
            const foundation::f32 maxExtent = foundation::Max(extent.x, foundation::Max(extent.y, extent.z));
            const foundation::f32 fit = (maxExtent > 0.0001f) ? (kTargetSize / maxExtent) : 1.0f;
            scene::EntityHandle modelRoot = m_scene->CreateEntity(prefix);
            foundation::Transform rootT;
            rootT.position = position;
            rootT.scale = foundation::Float3{fit, fit, fit};
            m_scene->SetLocalTransform(modelRoot, rootT);

            // All the model's materials, indexed by SubMesh::materialIndex (= model material index) for
            // per-submesh (multi-material) rendering. The resource manager keeps them alive via m_models.
            foundation::Array<foundation::RefPtr<materials::Material>> modelMats;
            modelMats.Reserve(model->materials.Size());
            for (auto& mp : model->materials)
            {
                modelMats.PushBack(foundation::RefPtr<materials::Material>(mp.Get()));
            }

            foundation::Array<scene::EntityHandle> entities;
            foundation::Array<scene::EntityHandle> skinnedEntities;
            entities.Reserve(model->nodes.Size());
            for (const modelimporter::ModelNode& node : model->nodes)
            {
                scene::EntityHandle e = m_scene->CreateEntity(node.name.AsView());
                m_scene->SetLocalTransform(e, node.localTransform);
                entities.PushBack(e);
            }
            for (foundation::usize i = 0; i < model->nodes.Size(); ++i)
            {
                const modelimporter::ModelNode& node = model->nodes[i];
                if (node.parentIndex >= 0 &&
                    static_cast<foundation::usize>(node.parentIndex) < entities.Size())
                {
                    m_scene->SetParent(entities[i],
                                       entities[static_cast<foundation::usize>(node.parentIndex)]);
                }
                else
                {
                    m_scene->SetParent(entities[i],
                                       modelRoot); // top-level node -> the scaled model root
                }
                if (node.meshIndex < 0 ||
                    static_cast<foundation::usize>(node.meshIndex) >= model->meshes.Size())
                {
                    continue;
                }
                geometry::StaticMesh* mesh =
                    model->meshes[static_cast<foundation::usize>(node.meshIndex)].Get();
                if (mesh == nullptr)
                {
                    continue;
                }
                render::MeshComponent& mc = meshes->Add(entities[i]);
                mc.mesh = foundation::RefPtr<geometry::StaticMesh>(
                    mesh); // hold a ref (manager owns the handle)
                mc.color = foundation::Color{1.0f, 1.0f, 1.0f, 1.0f};

                // The unified material list: submeshes index it by SubMesh::materialIndex,
                // slot 0 covers anything out of range.
                mc.SetMaterials(modelMats);
                if (mesh->IsSkinned())
                {
                    skinnedEntities.PushBack(entities[i]);
                }
            }
            m_models.PushBack(model); // keep the model (and its resources) alive

            // If the model is skinned + animated, hand it to the animation subsystem: attach a component
            // to the model root + list its skinned mesh nodes as the feed targets. The subsystem ticks
            // the player each frame and writes the skinning matrices into those MeshComponents - no
            // per-frame driving in app code. A graph-driven model gets an AnimationGraphComponent (a
            // state machine over its clips); everything else gets a single-clip SkeletalAnimationComponent.
            if (model->skeleton && model->animations.Size() > 0 && model->animations[0] &&
                skinnedEntities.Size() > 0)
            {
                if (useGraph)
                {
                    if (auto* graphMgr =
                            m_scene->GetSystem<animation::AnimationGraphComponentManager>())
                    {
                        foundation::RefPtr<animation::AnimationGraph> graph =
                            BuildClipCyclerGraph(*model);
                        animation::AnimationGraphComponent& gc = graphMgr->Add(modelRoot);
                        gc.skeleton = model->skeleton.Get();
                        gc.graph = graph.Get();
                        gc.meshEntities =
                            static_cast<foundation::Array<scene::EntityHandle>&&>(skinnedEntities);
                        m_graphs.PushBack(static_cast<foundation::RefPtr<animation::AnimationGraph>&&>(
                            graph));             // keep alive
                        m_graphChar = modelRoot; // G drives this one
                    }
                }
                else if (auto* skelMgr =
                             m_scene->GetSystem<animation::SkeletalAnimationComponentManager>())
                {
                    animation::SkeletalAnimationComponent& sa = skelMgr->Add(modelRoot);
                    sa.skeleton = model->skeleton.Get();
                    sa.clip = model->animations[0].Get();
                    sa.meshEntities =
                        static_cast<foundation::Array<scene::EntityHandle>&&>(skinnedEntities);
                }
            }
        }

        // Build a simple state-machine graph over a model's clips: one Clip state per animation, plus a
        // "Next" trigger that cross-fades each state to the following one (wrapping). Demonstrates the
        // AnimationGraph machinery (states, transitions, parameters, cross-fades) without authored data.
        foundation::RefPtr<animation::AnimationGraph> BuildClipCyclerGraph(model::ModelResource& model)
        {
            foundation::RefPtr<animation::AnimationGraph> graph =
                foundation::MakeRef<animation::AnimationGraph>(foundation::DefaultAllocator());
            const foundation::i32 nextParam =
                graph->AddParameter(u8"Next", animation::AnimationParameterType::Trigger);

            auto layer = foundation::MakeUnique<animation::AnimationLayer>(foundation::DefaultAllocator(),
                                                                     foundation::StringView(u8"Base"));
            const foundation::i32 clipCount = static_cast<foundation::i32>(model.animations.Size());
            for (foundation::i32 i = 0; i < clipCount; ++i)
            {
                animation::AnimationClip* clip =
                    model.animations[static_cast<foundation::usize>(i)].Get();
                auto state = foundation::MakeUnique<animation::AnimationGraphState>(
                    foundation::DefaultAllocator(),
                    clip != nullptr ? clip->Name().AsView() : foundation::StringView(u8"State"),
                    foundation::MakeUnique<animation::ClipStateNode>(foundation::DefaultAllocator(), clip));
                layer->AddState(
                    static_cast<foundation::UniquePtr<animation::AnimationGraphState>&&>(state));
            }
            // state[i] --Next--> state[(i+1) % N], cross-fading over 0.25s.
            for (foundation::i32 i = 0; i < clipCount; ++i)
            {
                auto t =
                    foundation::MakeUnique<animation::AnimationGraphTransition>(foundation::DefaultAllocator());
                t->sourceStateIndex = i;
                t->destStateIndex = (i + 1) % clipCount;
                t->duration = 0.25f;
                t->AddBoolCondition(nextParam, true);
                layer->AddTransition(
                    static_cast<foundation::UniquePtr<animation::AnimationGraphTransition>&&>(t));
            }
            graph->AddLayer(static_cast<foundation::UniquePtr<animation::AnimationLayer>&&>(layer));
            return graph;
        }

        // Fire the Character graph's "Next" trigger, advancing its state machine to the next clip. The
        // player is owned + created lazily by the AnimationGraphComponentManager, so reach it through the
        // component (it exists once the subsystem has ticked at least once).
        void FireGraphNext()
        {
            if (m_scene == nullptr || !m_graphChar.IsAssigned())
            {
                return;
            }
            auto* graphMgr = m_scene->GetSystem<animation::AnimationGraphComponentManager>();
            if (graphMgr == nullptr)
            {
                return;
            }
            if (animation::AnimationGraphComponent* gc = graphMgr->Get(m_graphChar))
            {
                if (gc->player.Get() != nullptr)
                {
                    gc->player->SetTrigger(foundation::StringView(u8"Next"));
                }
            }
        }

        // Aim the directional key light from pitch (downward tilt) + yaw (compass) angles. The light
        // shines along the entity's forward (-Z), so rotation = yaw(Y) * pitch(X); the sky sun tracks it.
        void ApplyKeyLightDir()
        {
            if (m_scene == nullptr || !m_keyLight.IsAssigned())
            {
                return;
            }
            foundation::Transform t = m_scene->GetLocalTransform(m_keyLight);
            t.rotation =
                foundation::Quaternion::FromAxisAngle(foundation::Float3{0.0f, 1.0f, 0.0f}, m_keyYaw) *
                foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, m_keyPitch);
            m_scene->SetLocalTransform(m_keyLight, t);
        }

        // Cycle the sky source (Procedural -> Analytic -> HDR Equirect -> Cubemap -> ...). Changing
        // skyMode re-runs the IBL precompute from the new source next frame.
        void CycleSkyMode()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                render::EnvironmentSettings& e = env->Environment();
                e.skyMode =
                    (e.skyMode == render::SkyMode::Procedural)    ? render::SkyMode::Analytic
                    : (e.skyMode == render::SkyMode::Analytic)    ? render::SkyMode::HDREquirect
                    : (e.skyMode == render::SkyMode::HDREquirect) ? render::SkyMode::Cubemap
                                                                  : render::SkyMode::Procedural;
            }
        }

        // Flip the reflection probe's box-parallax on/off (F6) to compare parallax-corrected vs infinite-env.
        void ToggleProbeParallax()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            if (auto* probes = m_scene->GetSystem<render::ReflectionProbeComponentManager>())
            {
                if (auto* rp = probes->Get(m_probeEntity))
                {
                    rp->parallax = !rp->parallax;
                }
            }
        }

        // Live debug UI (ImGui): scene environment tweakables wired straight to EnvironmentSettings -
        // editing these re-runs the IBL precompute next frame, so the ambient updates live.
        void BuildDebugUI(render::RenderSubsystem* render)
        {
            if (m_scene == nullptr)
            {
                return;
            }
            ImGui::Begin("Environment");
            if (render != nullptr)
            {
                float exposure = render->Exposure();
                if (ImGui::SliderFloat("Exposure", &exposure, 0.05f, 8.0f))
                {
                    render->SetExposure(exposure);
                }
                bool instShare =
                    render
                        ->InstanceSharing(); // prepass->forward instance-data sharing (A/B / regression)
                if (ImGui::Checkbox("Instance Sharing", &instShare))
                {
                    render->SetInstanceSharing(instShare);
                }
                {
                    // Ambient occlusion mode (Off/GTAO/SSAO, mutually exclusive) + shared knobs.
                    const char* aoItems[] = {"Off", "GTAO", "SSAO"};
                    int aoMode = static_cast<int>(render->GetAoMode());
                    if (ImGui::Combo("AO Mode", &aoMode, aoItems, 3))
                    {
                        render->SetAoMode(static_cast<render::AoMode>(aoMode));
                    }
                    if (render->GetAoMode() != render::AoMode::Off)
                    {
                        float s = render->AoStrength();
                        if (ImGui::SliderFloat("AO Strength", &s, 0.0f, 1.0f))
                        {
                            render->SetAoStrength(s);
                        }
                        float rad = render->AoRadius();
                        if (ImGui::SliderFloat("AO Radius", &rad, 0.1f, 3.0f))
                        {
                            render->SetAoRadius(rad);
                        }
                        float inten = render->AoIntensity();
                        if (ImGui::SliderFloat("AO Power", &inten, 0.5f, 4.0f))
                        {
                            render->SetAoIntensity(inten);
                        }
                    }
                    // Debug view (forces a generator on, shows the chosen buffer straight to screen): a good
                    // view-space normal ramps smoothly with orientation; View Z ramps with distance; AO
                    // should darken only in creases.
                    const char* dbgItems[] = {"Off",      "AO",     "Normal.x", "Normal.y",
                                              "Normal.z", "View Z", "Raw Depth"};
                    int dbg = render->AoDebug();
                    if (ImGui::Combo("AO Debug", &dbg, dbgItems, 7))
                    {
                        render->SetAoDebug(dbg);
                    }
                }
                ImGui::Separator();
                bool ssrOn = render->SsrEnabled();
                if (ImGui::Checkbox("SSR (screen-space reflections)", &ssrOn))
                {
                    render->SetSsrEnabled(ssrOn);
                }
                if (ssrOn)
                {
                    auto& p = render->SsrParams();
                    ImGui::Checkbox("SSR Temporal", &p.temporal);
                    ImGui::SliderFloat("SSR Intensity", &p.intensity, 0.0f, 1.0f);
                    ImGui::SliderFloat("SSR Glossy (0=sharp)", &p.glossy, 0.0f, 2.0f);
                    ImGui::SliderFloat("SSR Thickness", &p.thickness, 0.05f, 3.0f);
                    ImGui::SliderFloat("SSR Rough Cutoff", &p.roughnessCutoff, 0.0f, 1.0f);
                    ImGui::SliderInt("SSR Steps", &p.maxSteps, 16, 256);
                    if (p.temporal)
                    {
                        ImGui::SliderFloat("SSR History", &p.historyBlend, 0.0f, 0.98f);
                        ImGui::SliderFloat("SSR Ghost Reject", &p.ghostReject, 0.0f, 20.0f);
                    }
                    const char* ssrDbg[] = {"Off", "Raw Reflection", "Hit UV", "Weight",
                                            "Reflect Dir"};
                    ImGui::Combo("SSR Debug", &p.debug, ssrDbg, 5);
                }
                // The SSR eye test wants a tunable reflector: rebuild the floor material live
                // (roughness feeds the SSR cutoff/cone-gather; metallic drives reflectivity).
                bool floorChanged = false;
                floorChanged |= ImGui::SliderFloat("Floor Metallic", &m_floorMetallic, 0.0f, 1.0f);
                floorChanged |= ImGui::SliderFloat("Floor Roughness", &m_floorRoughness, 0.0f, 1.0f);
                if (floorChanged)
                {
                    ApplyFloorMaterial();
                }
                ImGui::Separator();
                bool taaOn = render->TaaEnabled();
                if (ImGui::Checkbox("TAA", &taaOn))
                {
                    render->SetTaaEnabled(taaOn);
                }
                if (!taaOn)
                { // FXAA is the TAA-off fallback (never stacked with TAA)
                    bool fxaaOn = render->FxaaEnabled();
                    if (ImGui::Checkbox("FXAA", &fxaaOn))
                    {
                        render->SetFxaaEnabled(fxaaOn);
                    }
                    if (fxaaOn)
                    {
                        float sq = render->FxaaSubpixel();
                        if (ImGui::SliderFloat("FXAA Subpixel", &sq, 0.0f, 1.0f))
                        {
                            render->SetFxaaSubpixel(sq);
                        }
                    }
                }
                if (taaOn)
                {
                    float b = render->TaaBlend();
                    if (ImGui::SliderFloat("TAA History", &b, 0.80f, 0.995f))
                    {
                        render->SetTaaBlend(b);
                    }
                    float g = render->TaaGamma();
                    if (ImGui::SliderFloat("TAA Variance", &g, 0.5f, 2.5f))
                    {
                        render->SetTaaGamma(g);
                    }
                    float m = render->TaaMotionScale();
                    if (ImGui::SliderFloat("TAA Motion", &m, 0.0f, 128.0f))
                    {
                        render->SetTaaMotionScale(m);
                    }
                }
                bool bloomOn = render->BloomEnabled();
                if (ImGui::Checkbox("Bloom", &bloomOn))
                {
                    render->SetBloomEnabled(bloomOn);
                }
                if (bloomOn)
                {
                    float bloom = render->BloomIntensity();
                    if (ImGui::SliderFloat("Bloom Intensity", &bloom, 0.0f, 0.5f))
                    {
                        render->SetBloomIntensity(bloom);
                    }
                    float bloomThresh = render->BloomThreshold();
                    if (ImGui::SliderFloat("Bloom Threshold", &bloomThresh, 0.0f, 4.0f))
                    {
                        render->SetBloomThreshold(bloomThresh);
                    }
                }
            }
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                render::EnvironmentSettings& e = env->Environment();
                // Sky source selector (also F5 to cycle). Picking a mode re-runs the IBL precompute.
                const char* modes[] = {"Procedural", "Analytic (Preetham)", "HDR Equirect",
                                       "Cubemap"};
                int modeIdx = (e.skyMode == render::SkyMode::Analytic)      ? 1
                              : (e.skyMode == render::SkyMode::HDREquirect) ? 2
                              : (e.skyMode == render::SkyMode::Cubemap)     ? 3
                                                                            : 0;
                if (ImGui::Combo("Sky Mode", &modeIdx, modes, 4))
                {
                    e.skyMode = (modeIdx == 1)   ? render::SkyMode::Analytic
                                : (modeIdx == 2) ? render::SkyMode::HDREquirect
                                : (modeIdx == 3) ? render::SkyMode::Cubemap
                                                 : render::SkyMode::Procedural;
                }
                ImGui::SliderFloat("Sky Intensity", &e.skyIntensity, 0.0f, 4.0f);

                const bool procedural = (e.skyMode == render::SkyMode::Procedural);
                const bool analytic = (e.skyMode == render::SkyMode::Analytic);
                const bool untextured = procedural || analytic; // has an analytic sun disc
                // Only show controls relevant to the selected mode.
                if (untextured)
                {
                    ImGui::SliderFloat("Sun Intensity", &e.sunIntensity, 0.0f, 8.0f);
                    ImGui::SliderFloat("Sun Size (deg)", &e.sunAngularSize, 0.1f, 10.0f);
                }
                if (analytic)
                {
                    ImGui::SliderFloat("Turbidity", &e.turbidity, 1.7f, 10.0f);
                }
                if (procedural)
                {
                    float h[3] = {e.skyHorizon.r, e.skyHorizon.g, e.skyHorizon.b};
                    if (ImGui::ColorEdit3("Horizon", h))
                    {
                        e.skyHorizon = foundation::Color{h[0], h[1], h[2], 1.0f};
                    }
                    float z[3] = {e.skyZenith.r, e.skyZenith.g, e.skyZenith.b};
                    if (ImGui::ColorEdit3("Zenith", z))
                    {
                        e.skyZenith = foundation::Color{z[0], z[1], z[2], 1.0f};
                    }
                    float gr[3] = {e.skyGround.r, e.skyGround.g, e.skyGround.b};
                    if (ImGui::ColorEdit3("Ground", gr))
                    {
                        e.skyGround = foundation::Color{gr[0], gr[1], gr[2], 1.0f};
                    }
                }
            }
            // Directional (key) light - the actual scene illumination + sky sun direction. Distinct from
            // "Sun Intensity" above (that's the sky's sun disc brightness, not the light that shades surfaces).
            if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
            {
                if (render::LightComponent* kl =
                        m_keyLight.IsAssigned() ? lights->Get(m_keyLight) : nullptr)
                {
                    ImGui::SeparatorText("Directional Light");
                    ImGui::SliderFloat("Light Intensity", &kl->intensity, 0.0f, 8.0f);
                    float lc[3] = {kl->color.r, kl->color.g, kl->color.b};
                    if (ImGui::ColorEdit3("Light Color", lc))
                    {
                        kl->color = foundation::Color{lc[0], lc[1], lc[2], 1.0f};
                    }
                    // Aim the light live (pitch = downward tilt, yaw = compass). SliderAngle shows degrees.
                    bool dirChanged = false;
                    dirChanged |= ImGui::SliderAngle("Pitch", &m_keyPitch, -89.0f, 0.0f);
                    dirChanged |= ImGui::SliderAngle("Yaw", &m_keyYaw, -180.0f, 180.0f);
                    if (dirChanged)
                    {
                        ApplyKeyLightDir();
                    }
                }
            }
            // Reflection probe: toggle box parallax (also F6) to compare parallax-corrected vs infinite-env.
            if (auto* probes = m_scene->GetSystem<render::ReflectionProbeComponentManager>())
            {
                if (render::ReflectionProbeComponent* rp =
                        m_probeEntity.IsAssigned() ? probes->Get(m_probeEntity) : nullptr)
                {
                    ImGui::SeparatorText("Reflection Probe");
                    ImGui::Checkbox("Box Parallax", &rp->parallax);
                    ImGui::SliderFloat("Probe Intensity", &rp->intensity, 0.0f, 4.0f);
                }
            }
            // Animation graph: fire the "Next" transition trigger (also the G hot-key).
            if (m_graphChar.IsAssigned())
            {
                ImGui::SeparatorText("Animation");
                if (ImGui::Button("Fire graph 'Next' (G)"))
                {
                    FireGraphNext();
                }
            }
            ImGui::SeparatorText("Debug");
            if (render != nullptr)
            {
                bool cull = render->ViewCulling();
                if (ImGui::Checkbox("View-frustum cull", &cull))
                {
                    render->SetViewCulling(cull);
                }
                if (cull)
                {
                    foundation::u32 culled = 0, total = 0;
                    render->ViewCullStats(culled, total);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u/%u)", culled, total);
                }
            }
            ImGui::Checkbox("Debug Draw (gizmos/grid/axes)", &m_showDebugDraw);
            ImGui::Checkbox("Show ImGui demo", &m_showImguiDemo);
            ImGui::End();
            if (m_showImguiDemo)
            {
                ImGui::ShowDemoWindow(&m_showImguiDemo);
            }
        }

        // Build (lazily) the input router + one surface per split half, then refit each surface's region
        // to the current window split. Each half's content-space equals its own pixel rect (region ==
        // contentSize, Stretch), so a viewport mouse reads [0..halfW]x[0..H] local to that view.
        void UpdateInputRouting(shell::IInputManager& input, shell::IWindow& win)
        {
            const foundation::f32 w = static_cast<foundation::f32>(win.Width());
            const foundation::f32 h = static_cast<foundation::f32>(win.Height());
            const foundation::f32 halfW = w * 0.5f;

            if (!m_inputRouter)
            {
                foundation::ContentFit fitL{.region = foundation::Rectangle{0.0f, 0.0f, halfW, h},
                                      .contentSize = foundation::Float2{halfW, h},
                                      .mode = foundation::FitMode::Stretch};
                foundation::ContentFit fitR{.region = foundation::Rectangle{halfW, 0.0f, w - halfW, h},
                                      .contentSize = foundation::Float2{w - halfW, h},
                                      .mode = foundation::FitMode::Stretch};
                m_surfaceL = foundation::MakeUnique<shell::InputSurface>(foundation::DefaultAllocator(), &input,
                                                                   win.Id(), fitL);
                m_surfaceR = foundation::MakeUnique<shell::InputSurface>(foundation::DefaultAllocator(), &input,
                                                                   win.Id(), fitR);
                m_inputRouter =
                    foundation::MakeUnique<shell::InputRouter>(foundation::DefaultAllocator(), &input);
                m_inputRouter->AddSurface(m_surfaceL.Get());
                m_inputRouter->AddSurface(m_surfaceR.Get());
                // Click-to-focus (router default): a click sets keyboard focus to that half, so WASD/QE
                // drive it; mouse-look (RMB drag) still follows the hovered half via pointer capture.
            }

            // Keep regions in sync with the live window size (resize/DPI).
            m_surfaceL->SetRegion(foundation::Rectangle{0.0f, 0.0f, halfW, h});
            m_surfaceL->SetContentSize(foundation::Float2{halfW, h});
            m_surfaceR->SetRegion(foundation::Rectangle{halfW, 0.0f, w - halfW, h});
            m_surfaceR->SetContentSize(foundation::Float2{w - halfW, h});

            // Let ImGui swallow input it's using this frame (e.g. scrolling the debug window) so the
            // viewport cameras don't also react. WantCapture* are valid after ImGui::NewFrame (above).
            if (ImGui::GetCurrentContext() != nullptr)
            {
                const ImGuiIO& io = ImGui::GetIO();
                m_inputRouter->SetExternalCapture(io.WantCaptureMouse, io.WantCaptureKeyboard);
            }
            m_inputRouter->Update();
        }

        // Split-screen rendered into an OFFSCREEN texture, then blitted to the backbuffer - the
        // editor-shaped path (a view renders to a sampleable/copyable target, not straight to the
        // swapchain). Exercises the full multi-view path + the configurable target final state.
        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>();
            auto* gfx = host.Graphics();
            if (m_scene == nullptr || render == nullptr || !render->IsReady() || gfx == nullptr ||
                gfx->Raw() == nullptr || frame.encoder == nullptr || frame.backbuffer == nullptr ||
                frame.window == nullptr)
            {
                return;
            }
            rhi::Device& device = *gfx->Raw();
            auto fmt = frame.window->Swap()->Format();
            // One offscreen per frame-in-flight: frame N+1 must not render into the target frame N's
            // blit still reads. (A single shared offscreen across in-flight frames races on resize.)
            const foundation::u32 slot = frame.frameIndex < kOffscreenSlots ? frame.frameIndex : 0u;
            if (!EnsureOffscreen(device, fmt, frame.width, frame.height, slot))
            {
                return;
            }
            rhi::Texture* offTex = m_offscreenTex[slot];
            rhi::TextureView* offView = m_offscreenView[slot];

            const foundation::u32 halfW = frame.width / 2;
            const foundation::f32 aspect =
                static_cast<foundation::f32>(halfW) / static_cast<foundation::f32>(frame.height);
            auto makeCam = [&](foundation::Float3 eye, foundation::Float3 target)
            {
                render::ViewCamera vc;
                vc.view = foundation::Float4x4::LookAtRH(eye, target, foundation::Float3{0.0f, 1.0f, 0.0f});
                vc.projection = foundation::Float4x4::PerspectiveFovRH(1.0472f, aspect, 0.1f, 1000.0f);
                vc.position = eye;
                vc.farZ = 1000.0f;
                return vc;
            };
            // Each half is driven by its own fly camera (hover a half to control it - see UpdateInputRouting).
            render::CameraOverride camL;
            camL.camera = makeCam(m_flyL.position, m_flyL.position + m_flyL.Forward());
            camL.clearColor = foundation::Color{0.02f, 0.02f, 0.03f, 1.0f};
            render::CameraOverride camR;
            camR.camera = makeCam(m_flyR.position, m_flyR.position + m_flyR.Forward());
            camR.clearColor = foundation::Color{0.02f, 0.02f, 0.03f, 1.0f};

            // Render both views into this slot's offscreen texture; the graph leaves it in CopySrc.
            render::TargetState ts{offTex, m_offscreenState[slot], rhi::ResourceState::CopySrc};
            render->BeginRendering(*frame.encoder, frame.frameIndex);
            render->RenderScene(*m_scene, offView, fmt, frame.width, frame.height,
                                render::ViewportRect{0, 0, halfW, frame.height}, &camL, ts);
            render->RenderScene(*m_scene, offView, fmt, frame.width, frame.height,
                                render::ViewportRect{static_cast<foundation::i32>(halfW), 0,
                                                     frame.width - halfW, frame.height},
                                &camR, ts);
            render->EndRendering();
            m_offscreenState[slot] = rhi::ResourceState::CopySrc;

            // Blit the offscreen result onto the backbuffer (the host then presents it).
            frame.encoder->TransitionTexture(frame.backbuffer, rhi::ResourceState::RenderTarget,
                                             rhi::ResourceState::CopyDst);
            frame.encoder->Blit(offTex, frame.backbuffer);
            frame.encoder->TransitionTexture(frame.backbuffer, rhi::ResourceState::CopyDst,
                                             rhi::ResourceState::RenderTarget);

            // Debug UI on top of the scene (backbuffer is RenderTarget again; ImGui loads + draws over it).
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

        // Create (or resize) one frame-slot's offscreen color target. Each slot tracks its own size,
        // so a resize lazily recreates each slot as it next renders.
        bool EnsureOffscreen(rhi::Device& device, rhi::TextureFormat fmt, foundation::u32 w, foundation::u32 h,
                             foundation::u32 slot)
        {
            if (m_offscreenTex[slot] != nullptr && m_offscreenW[slot] == w &&
                m_offscreenH[slot] == h)
            {
                return true;
            }
            device.WaitIdle();
            if (m_offscreenView[slot] != nullptr)
            {
                device.DestroyTextureView(m_offscreenView[slot]);
                m_offscreenView[slot] = nullptr;
            }
            if (m_offscreenTex[slot] != nullptr)
            {
                device.DestroyTexture(m_offscreenTex[slot]);
                m_offscreenTex[slot] = nullptr;
            }

            rhi::TextureDesc td{};
            td.format = fmt;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
                       rhi::TextureUsage::CopySrc;
            td.label = u8"sandbox.offscreen";
            if (!device.CreateTexture(td, m_offscreenTex[slot]).IsOk())
            {
                m_offscreenTex[slot] = nullptr;
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = fmt;
            if (!device.CreateTextureView(m_offscreenTex[slot], vd, m_offscreenView[slot]).IsOk())
            {
                m_offscreenView[slot] = nullptr;
                return false;
            }
            m_offscreenW[slot] = w;
            m_offscreenH[slot] = h;
            m_offscreenState[slot] = rhi::ResourceState::Undefined;
            return true;
        }

        void OnUpdate(runtime::IApplicationHost& host, foundation::f32 deltaTime) override
        {
            runtime::DefaultApplication::OnUpdate(host, deltaTime); // keep the P-key profiling dump

            // ImGui debug UI: open the frame (feed input + size) then build the tweakables. The draw
            // data is rendered over the scene in OnRenderWindow.
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildDebugUI(host.Ctx().GetSubsystem<render::RenderSubsystem>());
            }

            // Viewport-input routing. Build the router + one surface per split half once the shell
            // and window exist, then keep each surface's region in sync with the live split so a resize
            // (or DPI change) just re-fits. The router resolves the hovered surface and gates input.
            auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            shell::IWindow* win = host.Shell() != nullptr ? host.Shell()->MainWindow() : nullptr;
            if (input != nullptr && win != nullptr)
            {
                UpdateInputRouting(*input, *win);
            }

            // Per-view fly camera: each is driven by its surface's gated devices, so it only responds
            // while the pointer is over that half. WASD/QE move, RMB/Tab look, Shift fast.
            if (m_surfaceL)
            {
                m_flyL.Update(m_surfaceL->Keyboard(), m_surfaceL->Mouse(), deltaTime);
            }
            if (m_surfaceR)
            {
                m_flyR.Update(m_surfaceR->Keyboard(), m_surfaceR->Mouse(), deltaTime);
            }

            // Global (non-viewport) keys still read the raw keyboard.
            if (input != nullptr)
            {
                if (shell::IKeyboard* kb = input->Keyboard())
                {
                    if (kb->IsKeyPressed(shell::KeyCode::Escape))
                    {
                        host.RequestExit(0);
                        return;
                    }
                    // G fires the Character graph's "Next" trigger -> cross-fade to its next clip state.
                    if (kb->IsKeyPressed(shell::KeyCode::G))
                    {
                        FireGraphNext();
                    }
                    // F5 cycles the sky source (procedural <-> HDR equirectangular).
                    if (kb->IsKeyPressed(shell::KeyCode::F5))
                    {
                        CycleSkyMode();
                    }
                    // F6 toggles reflection-probe box parallax (compare parallax vs infinite-env reflection).
                    if (kb->IsKeyPressed(shell::KeyCode::F6))
                    {
                        ToggleProbeParallax();
                    }
                }
            }

            if (m_scene == nullptr)
            {
                return;
            }

            // Skinned models are advanced by the animation subsystem (SkeletalAnimation/AnimationGraph
            // components ticked on the scene's PostUpdate phase) - no per-frame driving here anymore.

            m_angle += deltaTime;
            const foundation::Quaternion spin =
                foundation::Quaternion::FromAxisAngle(foundation::Float3{0.3f, 1.0f, 0.0f}, m_angle);
            for (scene::EntityHandle cube : m_cubes)
            {
                foundation::Transform t = m_scene->GetLocalTransform(cube);
                t.rotation = spin;
                m_scene->SetLocalTransform(cube, t);
            }
            // Bob each point light in Z (depth) on its own phase, so the lights cross froxel depth
            // slices every frame - exercising the per-frame cluster rebuild, not a static binning.
            // Sweep the whole light field left/right (so the colored pools clearly slide as a
            // group - easy confirmation that pools exist and track the lights) + a gentle Z-bob.
            const foundation::f32 sweep = 5.0f * foundation::Sin(m_angle * 0.6f);
            for (foundation::usize k = 0; k < m_pointLights.Size(); ++k)
            {
                foundation::Float3 p = m_lightBases[k];
                p.x += sweep;
                p.z += 0.8f * foundation::Sin(m_angle * 1.3f + static_cast<foundation::f32>(k) * 0.5f);
                m_scene->SetLocalPosition(m_pointLights[k], p);
            }

            // Debug-draw demo: per-scene gizmos (wire boxes/spheres on the resting props, origin axes,
            // a ground grid, an arrow from the spot, 3D + screen text). Both split-screen views render
            // these, each projected through its OWN camera - and they're keyed to this scene, so a second
            // scene's gizmos would never bleed in.
            if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
            {
                // Debug gizmos toggle (ImGui "Debug Draw" checkbox). The FPS readout below stays on.
                if (m_showDebugDraw)
                {
                    auto& dbg = render->DebugScene(*m_scene);
                    for (int k = 0; k < 4; ++k)
                    {
                        const foundation::Float3 boxC{-7.5f + 5.0f * static_cast<foundation::f32>(k), 1.25f,
                                                10.0f};
                        dbg.DrawWireBoxCenter(boxC, foundation::Float3{1.3f, 1.3f, 1.3f},
                                              foundation::Color{1.0f, 1.0f, 0.0f, 1.0f});
                        const foundation::Float3 ballC{-7.5f + 5.0f * static_cast<foundation::f32>(k), 1.25f,
                                                 16.0f};
                        dbg.DrawWireSphere(foundation::BoundingSphere{ballC, 1.4f},
                                           foundation::Color{0.2f, 0.9f, 1.0f, 1.0f});
                    }
                    dbg.DrawAxis(foundation::Float4x4::Identity(), 3.0f, /*overlay*/ true);
                    dbg.DrawGrid(foundation::Float3{0.0f, 0.01f, 0.0f}, 60.0f, 30,
                                 foundation::Color{0.25f, 0.25f, 0.30f, 1.0f});
                    dbg.DrawArrow(foundation::Float3{0.0f, 14.0f, 13.0f}, foundation::Float3{0.0f, 0.5f, 13.0f},
                                  foundation::Color{1.0f, 0.5f, 0.0f, 1.0f});
                    dbg.DrawText3D(foundation::Float3{0.0f, 0.5f, 0.0f}, foundation::StringView(u8"origin"),
                                   foundation::Color{1.0f, 1.0f, 1.0f, 1.0f});
                    render->DebugScreen().DrawScreenText(12.0f, 12.0f,
                                                         foundation::StringView(u8"Draconic Debug Draw"),
                                                         foundation::Color{0.6f, 1.0f, 0.6f, 1.0f}, 2.0f);
                }

                // FPS / frame-time readout (smoothed). Format() has no float-precision spec, so round
                // to whole FPS and tenths-of-a-millisecond by hand.
                const foundation::f32 inst = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
                m_fpsSmoothed =
                    (m_fpsSmoothed > 0.0f) ? (m_fpsSmoothed * 0.9f + inst * 0.1f) : inst;
                const foundation::f32 ms = (m_fpsSmoothed > 0.0f) ? (1000.0f / m_fpsSmoothed) : 0.0f;
                const int fpsWhole = static_cast<int>(m_fpsSmoothed + 0.5f);
                const int msWhole = static_cast<int>(ms);
                const int msTenth =
                    static_cast<int>((ms - static_cast<foundation::f32>(msWhole)) * 10.0f + 0.5f);
                const foundation::String fpsText =
                    foundation::Format(u8"{} FPS  {}.{} ms", fpsWhole, msWhole, msTenth);
                render->DebugScreen().DrawScreenText(12.0f, 34.0f, fpsText.AsView(),
                                                     foundation::Color{1.0f, 1.0f, 0.4f, 1.0f}, 2.0f);
            }
        }

        void OnShutdown(runtime::IApplicationHost& host) override
        {
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                rhi::Device& device = *gfx->Raw();
                device.WaitIdle(); // GPU must finish before freeing the offscreen targets
                for (foundation::u32 i = 0; i < kOffscreenSlots; ++i)
                {
                    if (m_offscreenView[i] != nullptr)
                    {
                        device.DestroyTextureView(m_offscreenView[i]);
                        m_offscreenView[i] = nullptr;
                    }
                    if (m_offscreenTex[i] != nullptr)
                    {
                        device.DestroyTexture(m_offscreenTex[i]);
                        m_offscreenTex[i] = nullptr;
                    }
                }
                if (m_cutoutView != nullptr)
                {
                    device.DestroyTextureView(m_cutoutView);
                    m_cutoutView = nullptr;
                }
                if (m_cutoutTex != nullptr)
                {
                    device.DestroyTexture(m_cutoutTex);
                    m_cutoutTex = nullptr;
                }
            }
            foundation::ConsoleWrite(u8"Sandbox: shutting down.\n");
        }

        // Build a small procedural alpha-cutout texture (checkerboard: opaque cells + fully-transparent
        // holes) and upload it synchronously via an RHI transfer batch. Feeds the masked-material demo,
        // whose alpha-test discard cuts the holes. Stores the texture/view for shutdown cleanup.
        rhi::TextureView* CreateCutoutTexture(rhi::Device& device)
        {
            constexpr foundation::u32 N = 64, cell = 8;
            foundation::Array<foundation::u8> px;
            px.Resize(static_cast<foundation::usize>(N) * N * 4);
            for (foundation::u32 y = 0; y < N; ++y)
            {
                for (foundation::u32 x = 0; x < N; ++x)
                {
                    const bool solid = ((((x / cell) + (y / cell)) & 1u) == 0u);
                    foundation::u8* p = &px[(static_cast<foundation::usize>(y) * N + x) * 4];
                    p[0] = 255;
                    p[1] = 255;
                    p[2] = 255;
                    p[3] = solid ? 255 : 0;
                }
            }
            rhi::TextureDesc td{};
            td.dimension = rhi::TextureDimension::Texture2D;
            td.format = rhi::TextureFormat::RGBA8UnormSrgb;
            td.width = N;
            td.height = N;
            td.depth = 1;
            td.arrayLayerCount = 1;
            td.mipLevelCount = 1;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"masked.cutout";
            if (!device.CreateTexture(td, m_cutoutTex).IsOk())
            {
                m_cutoutTex = nullptr;
                return nullptr;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8UnormSrgb;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            vd.mipLevelCount = 1;
            vd.arrayLayerCount = 1;
            if (!device.CreateTextureView(m_cutoutTex, vd, m_cutoutView).IsOk())
            {
                device.DestroyTexture(m_cutoutTex);
                m_cutoutTex = nullptr;
                m_cutoutView = nullptr;
                return nullptr;
            }
            if (rhi::Queue* q = device.GetQueue(rhi::QueueType::Graphics, 0))
            {
                rhi::TransferBatch* batch = nullptr;
                if (q->CreateTransferBatch(batch).IsOk() && batch != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = N * 4;
                    layout.rowsPerImage = N;
                    batch->WriteTexture(m_cutoutTex,
                                        foundation::Span<const foundation::u8>(px.Data(), px.Size()), layout,
                                        rhi::Extent3D{N, N, 1});
                    (void)batch->Submit();
                    q->DestroyTransferBatch(batch);
                }
            }
            return m_cutoutView;
        }

        // Builds one kGrid×kGrid spinning cube grid centered at (originX, 0). When `instanced`,
        // every cube shares one material (-> a single instanced draw; the per-instance color
        // supplies each cube's hue). Otherwise each cube gets its own material with a per-cube PBR
        // matrix (roughness sweeps smooth→rough across X, top half metallic) -> many distinct
        // draws -> parallel command recording. Both grids run every frame.
        void BuildGrid(render::MeshComponentManager& meshes,
                       const foundation::RefPtr<geometry::StaticMesh>& cube, foundation::f32 originX,
                       bool instanced)
        {
            constexpr int kGrid =
                16; // 256 cubes -> the distinct grid trips the parallel-emit threshold
            constexpr foundation::f32 kSpacing = 0.8f;

            foundation::RefPtr<materials::Material> shared;
            if (instanced)
            {
                // White base + middling PBR; the per-instance color supplies each cube's hue.
                shared =
                    materials::CreatePBR(u8"lit", foundation::Float4{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.4f);
            }

            for (int y = 0; y < kGrid; ++y)
            {
                for (int x = 0; x < kGrid; ++x)
                {
                    scene::EntityHandle e = m_scene->CreateEntity(u8"cube");
                    const foundation::f32 fx = static_cast<foundation::f32>(x) - (kGrid - 1) * 0.5f;
                    const foundation::f32 fy = static_cast<foundation::f32>(y) - (kGrid - 1) * 0.5f;
                    m_scene->SetLocalPosition(
                        e, foundation::Float3{originX + fx * kSpacing, fy * kSpacing + 7.0f, 0.0f});

                    const foundation::Float4 baseColor{static_cast<foundation::f32>(x) / (kGrid - 1),
                                                 static_cast<foundation::f32>(y) / (kGrid - 1), 0.6f,
                                                 1.0f};
                    render::MeshComponent& mc = meshes.Add(e);
                    mc.mesh = cube;
                    if (instanced)
                    {
                        mc.SetMaterial(shared);
                        mc.color = foundation::Color{baseColor.x, baseColor.y, baseColor.z,
                                               1.0f}; // per-instance hue
                    }
                    else
                    {
                        const foundation::f32 rough =
                            0.05f + 0.95f * static_cast<foundation::f32>(x) / (kGrid - 1);
                        const foundation::f32 metal = (y >= kGrid / 2) ? 1.0f : 0.0f;
                        mc.SetMaterial(materials::CreatePBR(u8"lit", baseColor, metal, rough));
                        mc.color = foundation::Color{1.0f, 1.0f, 1.0f, 1.0f};
                    }
                    m_cubes.PushBack(e);
                }
            }
        }

    private:
        // Swap in a freshly built floor material for the current slider values. The mesh
        // renderer keys material instances by UID and prunes unreferenced ones, so replacing
        // the material is the clean live-tweak path (no instance-mutation API needed).
        void ApplyFloorMaterial()
        {
            if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
            {
                if (render::MeshComponent* fmc = meshes->Get(m_floorEntity))
                {
                    fmc->SetMaterial(materials::CreatePBR(
                        u8"lit", foundation::Float4{0.12f, 0.45f, 0.22f, 1.0f}, m_floorMetallic,
                        m_floorRoughness));
                }
            }
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_floorEntity{};
        foundation::f32 m_floorMetallic = 0.0f;  // floor material tweakables (SSR eye test)
        foundation::f32 m_floorRoughness = 0.45f;
        particles::ParticleEffect m_campfire; // the particle demo (billboards + trail sparks)
        scene::EntityHandle m_campfireEntity{};
        scene::EntityHandle m_camera{};
        // Offscreen render target, double-buffered per frame-in-flight (each slot tracks its own size).
        static constexpr foundation::u32 kOffscreenSlots = 3;
        rhi::Texture* m_offscreenTex[kOffscreenSlots] = {};
        rhi::TextureView* m_offscreenView[kOffscreenSlots] = {};
        rhi::ResourceState m_offscreenState[kOffscreenSlots] = {rhi::ResourceState::Undefined,
                                                                rhi::ResourceState::Undefined,
                                                                rhi::ResourceState::Undefined};
        foundation::u32 m_offscreenW[kOffscreenSlots] = {}, m_offscreenH[kOffscreenSlots] = {};
        rhi::Texture* m_cutoutTex = nullptr; // masked-demo alpha-cutout texture
        rhi::TextureView* m_cutoutView = nullptr;
        scene::EntityHandle
            m_keyLight; // directional key light (intensity/color tweakable in the debug UI)
        foundation::f32 m_keyPitch = -1.05f; // downward tilt (~-60 deg); Environment window slider
        foundation::f32 m_keyYaw = 0.35f;    // compass heading
        foundation::Array<scene::EntityHandle> m_pointLights;
        foundation::Array<foundation::Float3> m_lightBases;
        foundation::Array<scene::EntityHandle> m_cubes;
        foundation::f32 m_angle = 0.0f;
        foundation::f32 m_fpsSmoothed = 0.0f; // exponential moving average of 1/deltaTime
        // One fly camera per split view; hovering a view routes input to its camera (no V toggle).
        samples::FlyCamera m_flyL{.position = foundation::Float3{-6.0f, 21.0f, 30.0f}, .pitch = -0.45f};
        samples::FlyCamera m_flyR{.position = foundation::Float3{6.0f, 21.0f, 30.0f}, .pitch = -0.45f};

        // Viewport-input routing: each split half is an InputSurface (a ContentFit slice of the window);
        // the router picks the hovered surface and gates/transforms input into it. Created lazily once
        // the shell + window exist. focus-follows-hover so the pointer alone selects the active view.
        foundation::UniquePtr<shell::InputRouter> m_inputRouter;
        foundation::UniquePtr<shell::InputSurface> m_surfaceL;
        foundation::UniquePtr<shell::InputSurface> m_surfaceR;

        // Model-import pipeline state (must outlive the spawned entities - the resource manager owns
        // the cooked products' handles; the content DB + its filesystem mount back the manager).
        foundation::UniquePtr<vfs::NativeFileSystem> m_contentFs;
        foundation::UniquePtr<content::ContentDatabase> m_contentDb;
        foundation::UniquePtr<resource::ResourceManager> m_resources;
        geometry::StaticMeshFactory m_meshFactory;
        geometry::SkinnedMeshFactory m_skinnedMeshFactory;
        materials::MaterialFactory m_materialFactory;
        animation::SkeletonFactory m_skeletonFactory;
        animation::AnimationClipFactory m_clipFactory;
        foundation::UniquePtr<texture::TextureFactory> m_textureFactory; // needs the device
        resource::Proxy<texture::Texture>
            m_logoTex; // sprite-demo logo (kept alive for its GPU view)
        model::ModelFactory m_modelFactory;
        foundation::Array<resource::Proxy<model::ModelResource>>
            m_models; // keep cooked models + their resources alive

        // Animation graphs owned by the demo (the AnimationGraphComponents borrow them); the entity whose
        // graph the G key advances (the Character). Skinned models are otherwise driven by the subsystem.
        foundation::Array<foundation::RefPtr<animation::AnimationGraph>> m_graphs;
        scene::EntityHandle m_graphChar{};
        scene::EntityHandle m_probeEntity{}; // reflection probe (F6 toggles its parallax)
        bool m_showDebugDraw = true;         // debug gizmos (grid/axes/wire volumes/text)
        bool m_showImguiDemo = false;        // debug-UI toggle
    };
}

int main(int argc, char** argv)
{
    auto shell = shell::CreateShell();
    graphics::GraphicsDeviceDesc gpuDesc{};
    gpuDesc.backend = graphics::SelectBackendFromArguments(argc, argv);
    auto gpu = graphics::CreateGraphicsDevice(gpuDesc);
    graphics::GraphicsDevice* device = gpu.HasValue() ? gpu.Value().Get() : nullptr;

    SandboxApp app;
    return runtime::RunApplication(app, *shell, device);
}
