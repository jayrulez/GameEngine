// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AnimStressTest - a skinned-animation benchmark. It cooks the Quaternius humanoid once and
// replicates it across a grid, each copy an entity with its own AnimationPlayer (all sharing the
// cooked mesh, skeleton, clips and materials), so the cost of driving N separate characters can be
// measured as batches are added. [Space] adds a batch, [Backspace] removes one, [P] the profiler.

#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (isolate animation-drive cost)
#include "imgui.h"             // Dear ImGui (HUD) - used directly; integration is extensions.imgui

import foundation.core;
import foundation.profiler;
import foundation.rhi; // offscreen render target (Texture / ResourceState / Blit)
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import engine.defaultapp; // DefaultApplication (scene + render subsystems)
import engine.composition; // FullComposition: the factory set every host composes from
import foundation.scene;
import engine.scene;
import engine.render; // MeshComponent / CameraComponent + their managers
import foundation.render;           // ViewCamera / ViewportRect (split-screen overrides)
import extensions.imgui;            // ImguiSubsystem (HUD)
import foundation.geometry;
import foundation.geometry.resource; // StaticMeshFactory + StaticMesh product
import foundation.materials;
import foundation.materials.resource;  // MaterialFactory (cooked materials)
import foundation.texture.resource;    // TextureFactory (cooked textures)
import foundation.animation.resource;  // Skeleton/AnimationClip factories
import foundation.vfs;                 // NativeFileSystem mount for the content DB
import foundation.content;             // ContentDatabase (cooked-resource output)
import foundation.resource;            // ResourceManager + Proxy
import foundation.model;               // ModelLoadResult
import modelimporter;       // LoadAndCook + ImportedModel manifest
import foundation.animation;           // AnimationClip / Skeleton
import engine.animation; // SkeletalAnimationComponent(Manager) - engine-driven skinning

#include "../Common/FlyCamera.h" // shared free-fly camera (uses the imported runtime/core types)

// The raw model files and the per-sample cooked-output database live under the data root
// (resolved by the app).

namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace shell = foundation::shell;
namespace scene = foundation::scene;
namespace imgui = extensions::imgui;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace texture = foundation::texture;
namespace vfs = foundation::vfs;
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace model = foundation::model;
namespace animation = foundation::animation;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}




namespace
{
    // How many characters each +/- press adds or removes (and the initial spawn).
    static constexpr core::u32 kBatchSize = 25;
    static constexpr core::f32 kCharacterSpacing = 8.0f; // grid spacing (world units)
    static constexpr core::f32 kCharacterSize = 6.0f; // auto-fit target height (matches CookModel)
    static constexpr core::f32 kFloorY = -7.0f;
    static constexpr core::f32 kFloorBaseSize =
        120.0f; // base floor-plane size (scaled to cover the grid)

    class AnimStressTestApp final : public engine::runtime::DefaultApplication
    {
    public:
        // Run uncapped (vsync off) so the frame time reflects real CPU+GPU skinning work, not the
        // display refresh - same as RenderStressTest. The image tears; fine for a benchmark.
        graphics::RenderWindowDesc MainRenderWindow() const override
        {
            graphics::RenderWindowDesc d;
            d.presentMode = rhi::PresentMode::Immediate;
            return d;
        }

        // Register the ImGui subsystem so the benchmark HUD can draw over the scene.
        void Configure(runtime::IApplicationHost& host) override
        {
            engine::runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight(),
                                                                    DataFileSystem());
            }
        }

        void OnStartup(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }

            // CreateScene triggers the RenderSubsystem to inject the render managers.
            m_scene = PrimaryScenes().CreateScene(u8"sandbox");

            // Per-scene environment ambient (a dim cool indirect term; IBL replaces it later).
            if (auto* env = m_scene->GetSystem<engine::render::EnvironmentSystem>())
            {
                env->Environment().ambientColor = core::Color{0.12f, 0.16f, 0.28f, 1.0f};
                env->Environment().ambientIntensity = 0.35f;
            }

            // camera, pulled back along +Z looking at the origin (down -Z by default)
            // Raised + pitched down so the horizontal floor (lights above it) is clearly in view,
            // with the cube grids standing on it. Pitch ~28 deg below horizontal (looks toward the
            // scene center). Default camera looks down -Z; rotating about +X by -pitch tilts it down.
            m_camera = m_scene->CreateEntity(u8"camera");
            m_scene->SetLocalPosition(m_camera, core::Float3{0.0f, 14.0f, 30.0f});
            core::Transform camT = m_scene->GetLocalTransform(m_camera);
            camT.rotation = core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, -0.48f);
            m_scene->SetLocalTransform(m_camera, camT);
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                engine::render::CameraComponent& cam = cameras->Add(m_camera); // default 60deg perspective
                cam.clearColor =
                    core::Color{0.02f, 0.02f, 0.03f, 1.0f}; // dark backdrop so the lit scene reads
            }

            // A large horizontal floor (Plane normal = +Y) under the scene - the animated models stand
            // on it and the lights cast their shadows onto it.
            if (auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>())
            {
                m_floor = m_scene->CreateEntity(u8"floor");
                m_scene->SetLocalPosition(m_floor, core::Float3{0.0f, -7.0f, 0.0f});
                engine::render::MeshComponent& fmc = meshes->Add(m_floor);
                fmc.mesh = geometry::Primitives::Plane(AppRoot(), kFloorBaseSize, kFloorBaseSize);
                fmc.SetMaterial(materials::CreatePBR(u8"lit", core::Float4{0.5f, 0.5f, 0.53f, 1.0f},
                                                     0.0f, 0.65f));
            }

            // One directional shadow-casting key light - the whole scene (skinning benchmark, kept light
            // to isolate skinning/animation cost, à la Flax's "5,000 basic characters" reference scene).
            if (auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>())
            {
                scene::EntityHandle key = m_scene->CreateEntity(u8"keyLight");
                core::Transform kt = m_scene->GetLocalTransform(key);
                kt.rotation =
                    core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, -0.9f) *
                    core::Quaternion::FromAxisAngle(core::Float3{0.0f, 1.0f, 0.0f}, 0.5f);
                m_scene->SetLocalTransform(key, kt);
                engine::render::LightComponent& kl = lights->Add(key);
                kl.type = engine::render::LightType::Directional;
                kl.color = core::Color{1.0f, 0.97f, 0.92f, 1.0f};
                kl.intensity = 2.5f;
                kl.castsShadows = true; // directional CSM
            }

            LoadImportedModel(host); // cook the character + spawn the initial grid

            // Lower default exposure: the procedural-sky IBL + sun are bright, so AgX washes out at 1.0.
            if (auto* render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>())
            {
                render->SetExposure(0.5f);
            }

            core::ConsoleWrite(u8"AnimStressTest: [Space] add batch  [Backspace] remove batch  [P] "
                               u8"profiler  [Esc] exit\n");
        }

        // The model-import seam: open the cooked-resource output DB, register the geometry factory,
        // load+cook a glTF file through the importer, then spawn its node hierarchy as entities whose
        // MeshComponents reference the cooked StaticMesh resources. This is the clean runtime cook seam
        // the design calls for - an editor would cook offline and the runtime would only Bind, but the
        // wiring (factory -> Bind -> render) is identical.
        void LoadImportedModel(runtime::IApplicationHost& host)
        {
            const core::String outputDirStorage =
                foundation::vfs::DataPath(DataRoot(), u8"Output/AnimStressTest");
            const core::String modelDirStorage =
                foundation::vfs::DataPath(DataRoot(), u8"Assets/models");
            const core::StringView outputDir = outputDirStorage.AsView();
            const core::StringView modelDir = modelDirStorage.AsView();
            if (outputDir.IsEmpty() || modelDir.IsEmpty())
            {
                return;
            }

            // Output DB (cooked resources) + resource manager + the factories. ModelFactory builds the
            // manifest into a ModelResource, resolving its meshes/materials/textures (dependency edges).
            m_contentFs =
                core::MakeUnique<vfs::NativeFileSystem>(AppRoot(), outputDir, AppRoot());
            m_contentDb = core::MakeUnique<content::ContentDatabase>(AppRoot(), 
                AppRoot(), *m_contentFs, core::BinarySerializerFactory(),
                u8".rasset");
            m_resources =
                core::MakeUnique<resource::ResourceManager>(AppRoot(), AppRoot(), *m_contentDb);
            // Every factory the engine composition describes, created with what this host offers
            // (the device gates the texture factory), registered into the manager.
            resource::ResourceServiceTable services;
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                services.Add<rhi::Device>(gfx->Raw());
            }
            engine::FullComposition().CreateFactories(m_factories, AppRoot(), services);
            m_factories.Register(*m_resources);
            engine::RegisterAllResourceTypes(); // make the cooked types deserializable

            // Cook the Quaternius humanoid once, then replicate it across a grid (each instance gets its
            // own AnimationPlayer; all share the cooked mesh/skeleton/clips/materials).
            if (!CookModel(u8"Char",
                           core::Format(u8"{}/QuaterniusCharacter/glTF/Character.gltf", modelDir)
                               .AsView()))
            {
                return;
            }
            RebuildToCount(kAutoProfile ? kProfileCount : (kAutoRamp ? 100u : kBatchSize));
        }

        // Cook + bind + spawn one model, placed at `position` and auto-fit to a target size. Each model
        // spawns its node hierarchy (local TRS + parent links) under a scaled model-root entity; mesh
        // nodes get a MeshComponent referencing the cooked StaticMesh + material.
        // Cook + bind the model ONCE. Every spawned instance shares these resources (mesh/skeleton/
        // clips/materials); only the per-entity transform + AnimationPlayer differ. Returns true on success.
        bool CookModel(core::StringView prefix, core::StringView path)
        {
            if (m_contentDb.Get() == nullptr)
            {
                return false;
            }
            core::Guid modelGuid;
            const model::ModelLoadResult r =
                pipeline::LoadAndCook(path, *m_contentDb, prefix, modelGuid);
            if (r != model::ModelLoadResult::Ok)
            {
                core::ConsoleWrite(core::Format(u8"AnimStressTest: model import failed ({})\n",
                                                static_cast<core::u32>(r)));
                return false;
            }
            m_model = m_resources->Bind<model::ModelResource>(modelGuid);
            if (!m_model)
            {
                core::ConsoleWrite(u8"AnimStressTest: model bind failed\n");
                return false;
            }

            // Auto-fit: scale the model's largest extent to a target size.
            constexpr core::f32 kTargetSize = 6.0f;
            const core::Float3 extent = m_model->boundsMax - m_model->boundsMin;
            const core::f32 maxExtent = core::Max(extent.x, core::Max(extent.y, extent.z));
            m_fit = (maxExtent > 0.0001f) ? (kTargetSize / maxExtent) : 1.0f;

            // All materials, indexed by SubMesh::materialIndex (multi-material).
            m_modelMats.Reserve(m_model->materials.Size());
            for (auto& mp : m_model->materials)
            {
                m_modelMats.PushBack(core::RefPtr<materials::Material>(mp.Get()));
            }

            // Clips available for random per-instance selection (variety so the herd never lockstep).
            for (auto& clip : m_model->animations)
            {
                if (clip)
                {
                    m_clips.PushBack(clip.Get());
                }
            }
            return true;
        }

        // Spawn one instance of the cooked model at `position`: its own node hierarchy under a scaled
        // root (mesh nodes get MeshComponents referencing the SHARED cooked meshes/materials), plus its
        // own AnimationPlayer over the shared skeleton.
        void SpawnInstance(core::Float3 position)
        {
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr || !m_model)
            {
                return;
            }

            scene::EntityHandle modelRoot = m_scene->CreateEntity(u8"char");
            core::Transform rootT;
            rootT.position = position;
            rootT.scale = core::Float3{m_fit, m_fit, m_fit};
            m_scene->SetLocalTransform(modelRoot, rootT);
            Instance inst;
            inst.root = modelRoot;

            core::Array<scene::EntityHandle> entities;
            core::Array<scene::EntityHandle> skinnedEntities;
            entities.Reserve(m_model->nodes.Size());
            for (const pipeline::ModelNode& node : m_model->nodes)
            {
                scene::EntityHandle e = m_scene->CreateEntity(node.name.AsView());
                m_scene->SetLocalTransform(e, node.localTransform);
                entities.PushBack(e);
            }
            for (core::usize i = 0; i < m_model->nodes.Size(); ++i)
            {
                const pipeline::ModelNode& node = m_model->nodes[i];
                if (node.parentIndex >= 0 &&
                    static_cast<core::usize>(node.parentIndex) < entities.Size())
                {
                    m_scene->SetParent(entities[i],
                                       entities[static_cast<core::usize>(node.parentIndex)]);
                }
                else
                {
                    m_scene->SetParent(entities[i],
                                       modelRoot); // top-level node -> the scaled model root
                }
                if (node.meshIndex < 0 ||
                    static_cast<core::usize>(node.meshIndex) >= m_model->meshes.Size())
                {
                    continue;
                }
                geometry::StaticMesh* mesh =
                    m_model->meshes[static_cast<core::usize>(node.meshIndex)].Get();
                if (mesh == nullptr)
                {
                    continue;
                }
                engine::render::MeshComponent& mc = meshes->Add(entities[i]);
                mc.mesh = core::RefPtr<geometry::StaticMesh>(mesh);
                mc.color = core::Color{1.0f, 1.0f, 1.0f, 1.0f};
                mc.SetMaterials(m_modelMats); // unified list; slot 0 covers out-of-range
                if (mesh->IsSkinned())
                {
                    skinnedEntities.PushBack(entities[i]);
                }
            }

            // Attach a SkeletalAnimationComponent on the root: the AnimationSubsystem ticks its player
            // each frame (PostUpdate) and feeds the skinning matrices to the skinned mesh entities. Random
            // clip + speed jitter + randomized start so the herd desyncs (à la Sedulous EngineAnimationSandbox).
            if (m_model->skeleton && !m_clips.IsEmpty() && skinnedEntities.Size() > 0)
            {
                if (auto* anims =
                        m_scene->GetSystem<engine::animation::SkeletalAnimationComponentManager>())
                {
                    animation::AnimationClip* clip = m_clips[static_cast<core::usize>(
                        m_rng.NextInt(0, static_cast<core::i32>(m_clips.Size()) - 1))];
                    engine::animation::SkeletalAnimationComponent& a = anims->Add(modelRoot);
                    a.skeleton = m_model->skeleton.Get();
                    a.clip = clip;
                    for (scene::EntityHandle e : skinnedEntities)
                    {
                        a.meshEntities.PushBack(m_scene->GetEntityId(e));
                    }
                    a.speed = 0.85f + m_rng.NextFloat() * 0.3f;
                    a.startTime = (clip != nullptr && clip->duration > 0.0f)
                                      ? m_rng.NextFloat() * clip->duration
                                      : 0.0f;
                }
            }
            m_instances.PushBack(static_cast<Instance&&>(inst));
        }

        // Rebuild the whole grid to `count` characters: destroy the current instances, then spawn a fresh
        // square grid (side = ceil(sqrt(count))) centered on the origin, and re-frame the camera on it.
        void RebuildToCount(core::u32 count)
        {
            for (Instance& inst : m_instances)
            {
                m_scene->DestroyEntity(inst.root);
            } // recurses -> frees comps
            m_instances.Clear(); // frees the per-instance players

            const core::u32 side =
                (count == 0)
                    ? 1u
                    : static_cast<core::u32>(core::Ceil(core::Sqrt(static_cast<core::f32>(count))));
            const core::f32 half = (static_cast<core::f32>(side) - 1.0f) * 0.5f;
            for (core::u32 i = 0; i < count; ++i)
            {
                const core::f32 px = (static_cast<core::f32>(i % side) - half) * kCharacterSpacing;
                const core::f32 pz = (static_cast<core::f32>(i / side) - half) * kCharacterSpacing;
                SpawnInstance(core::Float3{px, kFloorY, pz});
            }
            AutoFrame(side);
            m_frameTimeMs =
                16.6f; // reset the smoother so the rebuild hitch doesn't skew the reading
            core::ConsoleWrite(
                core::Format(u8"AnimStressTest: characters={}\n", m_instances.Size()));
        }

        // Position the fly camera so the whole side×side grid is in frame + grow the floor under it (called
        // on every batch change).
        void AutoFrame(core::u32 side)
        {
            const core::f32 extent =
                (static_cast<core::f32>(side) - 1.0f) * kCharacterSpacing * 0.5f + kCharacterSize;
            // Floor: scale the base plane so it covers the whole grid + margin (uniform XZ; Y stays flat).
            const core::f32 fscale = core::Max(1.0f, (extent * 2.0f + 40.0f) / kFloorBaseSize);
            core::Transform ft = m_scene->GetLocalTransform(m_floor);
            ft.scale = core::Float3{fscale, 1.0f, fscale};
            m_scene->SetLocalTransform(m_floor, ft);
            const core::Float3 target{0.0f, kFloorY + kCharacterSize * 0.5f, 0.0f}; // grid center
            const core::f32 dist = extent / core::Tan(0.5236f) +
                                   kCharacterSize * 2.0f; // fit 60° FOV horizontally + margin
            const core::f32 camY = extent * 0.55f + kCharacterSize;
            m_fly.position = core::Float3{target.x, target.y + camY, target.z + dist};
            m_fly.yaw = 0.0f;
            m_fly.pitch = -core::Atan2(camY, dist); // look down onto the grid center
            // Extend the far plane to cover the whole grid from this distance, so no characters get
            // frustum-far-culled (which would make the throughput measurement cheaper than it is).
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                if (engine::render::CameraComponent* cam = cameras->Get(m_camera))
                {
                    cam->nearZ = 0.5f;
                    cam->farZ = dist + extent * 2.0f + 100.0f;
                }
            }
        }

        void AddBatch() { RebuildToCount(static_cast<core::u32>(m_instances.Size()) + kBatchSize); }
        void RemoveBatch()
        {
            const core::u32 n = static_cast<core::u32>(m_instances.Size());
            RebuildToCount(n > kBatchSize ? n - kBatchSize : 0u);
        }

        // Single full-screen view via the default render path (reads the scene's primary camera).
        // Keep the camera's aspect synced to the backbuffer before delegating.
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
            RenderFrame(host, frame); // the scenes + window overlays; FinishFrame below closes the shot

            // HUD over the scene (backbuffer is RenderTarget after the default render path).
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
            FinishFrame(host, frame); // the screenshot copy, with the overlay in it
        }

        void OnUpdate(runtime::IApplicationHost& host, core::f32 deltaTime) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, deltaTime); // keep the P-key profiling dump

            // ImGui HUD: open the frame + build the stats window (drawn in OnRenderWindow).
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildHud(host.Ctx().GetSubsystem<engine::render::RenderSubsystem>());
            }

            // Fly camera (WASD/QE move, RMB/Tab look, Shift fast). Drives the scene camera entity; Esc exits.
            m_fly.Update(host, deltaTime);
            if (auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr)
            {
                if (shell::IKeyboard* kb = input->Keyboard())
                {
                    if (kb->IsKeyPressed(shell::KeyCode::Space))
                    {
                        AddBatch();
                    }
                    if (kb->IsKeyPressed(shell::KeyCode::Backspace))
                    {
                        RemoveBatch();
                    }
                    if (kb->IsKeyPressed(shell::KeyCode::H))
                    {
                        m_showHud = !m_showHud;
                    }
                    if (kb->IsKeyPressed(shell::KeyCode::I))
                    { // toggle prepass->forward instance-data sharing (A/B)
                        if (auto* render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>())
                        {
                            const bool on = !render->InstanceSharing();
                            render->SetInstanceSharing(on);
                            core::ConsoleWrite(on ? u8"Instance sharing: ON\n"
                                                  : u8"Instance sharing: OFF (forward re-fills)\n");
                        }
                    }
                    if (kb->IsKeyPressed(shell::KeyCode::Escape))
                    {
                        host.RequestExit(0);
                        return;
                    }
                }
            }
            if (m_scene != nullptr)
            {
                core::Transform camT = m_scene->GetLocalTransform(m_camera);
                camT.position = m_fly.position;
                camT.rotation = m_fly.Rotation();
                m_scene->SetLocalTransform(m_camera, camT);
            }

            if (m_scene == nullptr)
            {
                return;
            }

            // Animation is now driven by the engine's AnimationSubsystem (it ticks each entity's
            // SkeletalAnimationComponent in the scene's PostUpdate phase and feeds the bone matrices);
            // the sample no longer drives players by hand.

            // Smoothed FPS/frame-ms (vsync off -> real frame cost); shown in the ImGui HUD.
            m_frameTimeMs = m_frameTimeMs * 0.9f + (deltaTime * 1000.0f) * 0.1f;

            // TEMP headless auto-profile: hold a fixed count, warm up, then dump CPU + GPU profiler
            // reports (same as the P key) and exit - for capturing the baseline frame breakdown.
            if (kAutoProfile)
            {
                m_profileElapsed += deltaTime;
                if (m_profileElapsed >= 5.0f)
                {
                    const core::f32 fps =
                        (m_frameTimeMs > 0.001f) ? (1000.0f / m_frameTimeMs) : 0.0f;
                    core::ConsoleWrite(core::Format(
                        u8"=== AnimStressTest PROFILE: chars={}  fps={}  frame={} ms ===\n",
                        m_instances.Size(), static_cast<core::u32>(fps + 0.5f), m_frameTimeMs));
                    core::ConsoleWrite(foundation::profiler::Profiler::Get().BuildReport().AsView());
                    if (auto* renderer = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>())
                    {
                        core::String gpu;
                        renderer->BuildGpuProfileReport(gpu);
                        core::ConsoleWrite(gpu.AsView());
                    }
                    host.RequestExit(0);
                    return;
                }
            }

            // TEMP headless auto-ramp: grow the count until FPS settles at/below 50, then report + exit.
            // Coarse (+25%) while well above 50, fine (+kBatchSize) near the knee, for a precise threshold.
            if (kAutoRamp)
            {
                m_rampSettle += deltaTime;
                if (m_rampSettle >= 1.3f)
                {
                    m_rampSettle = 0.0f;
                    const core::f32 fps =
                        (m_frameTimeMs > 0.001f) ? (1000.0f / m_frameTimeMs) : 0.0f;
                    const core::u32 n = static_cast<core::u32>(m_instances.Size());
                    if (fps <= 50.0f)
                    {
                        core::ConsoleWrite(core::Format(
                            u8"AnimStressTest: THRESHOLD chars={}  fps={}  frame={} ms\n", n,
                            static_cast<core::u32>(fps + 0.5f), m_frameTimeMs));
                        host.RequestExit(0);
                        return;
                    }
                    const core::u32 next =
                        (fps > 60.0f) ? core::Max(n + 50u, n + n / 4u) : (n + kBatchSize);
                    RebuildToCount(next);
                }
            }
        }

        // ImGui HUD: character count + frame stats + exposure/bloom controls (H toggles it).
        void BuildHud(engine::render::RenderSubsystem* render)
        {
            if (!m_showHud)
            {
                return;
            }
            ImGui::Begin("Anim Stress Test");
            const float fps = m_frameTimeMs > 0.001f ? 1000.0f / m_frameTimeMs : 0.0f;
            ImGui::Text("%.0f fps   %.2f ms", static_cast<double>(fps),
                        static_cast<double>(m_frameTimeMs));
            ImGui::Text("characters: %d", static_cast<int>(m_instances.Size()));
            if (render != nullptr)
            {
                ImGui::Separator();
                float exposure = render->Exposure();
                if (ImGui::SliderFloat("Exposure", &exposure, 0.05f, 4.0f))
                {
                    render->SetExposure(exposure);
                }
                bool bloomOn = render->BloomEnabled();
                if (ImGui::Checkbox("Bloom", &bloomOn))
                {
                    render->SetBloomEnabled(bloomOn);
                }
                bool inst = render->InstanceSharing();
                if (ImGui::Checkbox("Instance sharing (I)", &inst))
                {
                    render->SetInstanceSharing(inst);
                }
                bool cull = render->ViewCulling();
                if (ImGui::Checkbox("View-frustum cull", &cull))
                {
                    render->SetViewCulling(cull);
                }
                if (cull)
                {
                    core::u32 culled = 0, total = 0;
                    render->ViewCullStats(culled, total);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u/%u culled)", culled, total);
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Directional shadows");
                // The sun's shadow reach is the scene's (its environment settings).
                if (auto* env = m_scene->GetSystem<engine::render::EnvironmentSystem>())
                {
                    ImGui::SliderFloat("Distance", &env->Environment().shadowDistance, 50.0f, 1000.0f, "%.0f");
                    ImGui::SliderFloat("Far fade", &env->Environment().shadowFadeDistance, 2.0f, 150.0f, "%.0f");
                    ImGui::TextDisabled("shadows fade out over the last %.0f units",
                                        static_cast<double>(env->Environment().shadowFadeDistance));
                }
            }
            ImGui::Separator();
            if (ImGui::Button("+ batch (Space)"))
            {
                AddBatch();
            }
            ImGui::SameLine();
            if (ImGui::Button("- batch (Backspace)"))
            {
                RemoveBatch();
            }
            ImGui::TextUnformatted("H hide HUD   P profiler   Esc exit");
            ImGui::TextUnformatted("WASD/QE move   RMB look   Shift fast");
            ImGui::End();
        }

        void OnShutdown(runtime::IApplicationHost& host) override
        {
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                gfx->Raw()->WaitIdle();
            }
            core::ConsoleWrite(u8"AnimStressTest: shutting down.\n");
        }

    private:
        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_floor{};
        samples::FlyCamera m_fly{.position = core::Float3{0.0f, 10.0f, 26.0f}, .pitch = -0.25f};

        // Model-import pipeline state (must outlive the spawned entities - the resource manager owns
        // the cooked products' handles; the content DB + its filesystem mount back the manager).
        core::UniquePtr<vfs::NativeFileSystem> m_contentFs;
        core::UniquePtr<content::ContentDatabase> m_contentDb;
        core::UniquePtr<resource::ResourceManager> m_resources;
        resource::ResourceFactorySet m_factories; // the engine composition's set, this host's services
        resource::Proxy<model::ModelResource>
            m_model; // the one cooked model, shared by every instance
        core::Array<core::RefPtr<materials::Material>>
            m_modelMats; // its materials (indexed by submesh material index)
        core::Array<animation::AnimationClip*> m_clips; // clips for random per-instance selection
        core::f32 m_fit = 1.0f;                         // auto-fit scale

        // One spawned character: just its root entity (DestroyEntity recurses to free the hierarchy +
        // its SkeletalAnimationComponent). The component (engine-driven) owns the player + targets.
        struct Instance
        {
            scene::EntityHandle root{};
        };
        core::Array<Instance> m_instances;
        core::Random m_rng{0x9e3779b97f4a7c15ull};
        core::f32 m_frameTimeMs = 16.6f;
        bool m_showHud = true; // HUD visibility (H)

        // Measurement aid (off by default): auto-ramp the character count until FPS <= 50, then
        // report + exit. Flip to true for a headless throughput baseline; normal use is interactive.
        static constexpr bool kAutoRamp = false;
        core::f32 m_rampSettle = 0.0f;
        // Measurement aid (off by default): hold a fixed count, then dump the CPU+GPU profiler
        // breakdown and exit. Flip true to re-capture the baseline frame breakdown headless.
        static constexpr bool kAutoProfile = false;
        static constexpr core::u32 kProfileCount = 1000;
        core::f32 m_profileElapsed = 0.0f;
    };
}

// A custom entry (not APP_MAIN) that still honours the shared flags: --vulkan/--webgpu/--dx12
// pick the backend, --data-root <dir> overrides the data-root walk.
int main(int argc, char** argv)
{
    auto shell = shell::CreateShell(AppRoot());
    graphics::GraphicsDeviceDesc gpuDesc = graphics::DeviceDescFromArguments(argc, argv);
    auto gpu = graphics::CreateGraphicsDevice(gpuDesc);
    graphics::GraphicsDevice* device = gpu.HasValue() ? gpu.Value().Get() : nullptr;

    AnimStressTestApp app;
    app.SetDataRoot(foundation::vfs::DataRootFromArguments(argc, argv).AsView());
    app.OnCommandLine(argc, argv); // --screenshot and whatever else the base app reads
    return runtime::RunApplication(app, *shell, device);
}
