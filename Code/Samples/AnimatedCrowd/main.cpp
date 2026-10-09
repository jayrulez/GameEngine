// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// AnimatedCrowd - a skinned crowd drawn as one instanced set: ONE InstancedMeshComponent (the
// character at a grid of transforms) and ONE InstancedSkinningComponent (M shared pose palettes),
// so the whole crowd is one draw per pass animated by M palette computes, not N. The HUD picks how
// each character takes its pose from the palettes (random, wave, columns, clusters). [Space] adds a
// batch, [Backspace] removes one, [P] the profiler.

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
namespace render = foundation::render;
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
    static constexpr core::u32 kBatchSize = 500;
    static constexpr core::f32 kCharacterSpacing = 8.0f; // grid spacing (world units)
    static constexpr core::f32 kCharacterSize = 6.0f; // auto-fit target height (matches CookModel)
    static constexpr core::f32 kFloorY = -7.0f;
    static constexpr core::f32 kFloorBaseSize =
        120.0f; // base floor-plane size (scaled to cover the grid)

    class AnimatedCrowdApp final : public engine::runtime::DefaultApplication
    {
    public:
        // How the crowd picks each character's pose out of the M shared palettes (HUD "Pose assignment").
        // Random/Wave map to renderer policies computed from the flat index; Columns/Clusters are layout-
        // aware, so the sample precomputes the pose index per instance and hands it over as Explicit.
        enum class PosePolicy : int
        {
            Random,
            Wave,
            Columns,
            Clusters,
            Custom
        };

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
                m_keyLight = m_scene->CreateEntity(u8"keyLight");
                core::Transform kt = m_scene->GetLocalTransform(m_keyLight);
                kt.rotation =
                    core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, -0.9f) *
                    core::Quaternion::FromAxisAngle(core::Float3{0.0f, 1.0f, 0.0f}, 0.5f);
                m_scene->SetLocalTransform(m_keyLight, kt);
                engine::render::LightComponent& kl = lights->Add(m_keyLight);
                kl.type = engine::render::LightType::Directional;
                kl.color = core::Color{1.0f, 0.97f, 0.92f, 1.0f};
                kl.intensity = 2.5f;
                kl.castsShadows = true; // directional CSM (K toggles)
            }

            LoadImportedModel(host); // cook the character + spawn the initial grid

            // Lower default exposure: the procedural-sky IBL + sun are bright, so AgX washes out at 1.0.
            if (auto* render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>())
            {
                render->SetExposure(0.5f);
            }

            core::ConsoleWrite(u8"AnimatedCrowd: [Space] add batch  [Backspace] remove batch  [P] "
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
                foundation::vfs::DataPath(DataRoot(), u8"Output/AnimatedCrowd");
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
                core::ConsoleWrite(core::Format(u8"AnimatedCrowd: model import failed ({})\n",
                                                static_cast<core::u32>(r)));
                return false;
            }
            m_model = m_resources->Bind<model::ModelResource>(modelGuid);
            if (!m_model)
            {
                core::ConsoleWrite(u8"AnimatedCrowd: model bind failed\n");
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

            // Clips available for the crowd (the shared pose pool plays one).
            for (auto& clip : m_model->animations)
            {
                if (clip)
                {
                    m_clips.PushBack(clip.Get());
                }
            }

            // Every SKINNED mesh + its material - the crowd renders one instanced set per part (a Quaternius
            // character is several skinned meshes sharing the one skeleton).
            for (core::usize i = 0; i < m_model->meshes.Size(); ++i)
            {
                geometry::StaticMesh* mesh = m_model->meshes[i].Get();
                if (mesh == nullptr || !mesh->IsSkinned())
                {
                    continue;
                }
                const core::i32 matIdx =
                    (i < m_model->meshMaterial.Size()) ? m_model->meshMaterial[i] : -1;
                core::RefPtr<materials::Material> mat =
                    (matIdx >= 0 && static_cast<core::usize>(matIdx) < m_modelMats.Size())
                        ? m_modelMats[static_cast<core::usize>(matIdx)]
                        : (m_modelMats.IsEmpty() ? core::RefPtr<materials::Material>{}
                                                 : m_modelMats[0]);
                Part part{core::RefPtr<geometry::StaticMesh>(mesh), mat, matIdx, {}};
                m_model->MeshMaterialIndices(i, part.slots); // what its submeshes index
                m_skinnedParts.PushBack(core::Move(part));
            }
            if (m_skinnedParts.IsEmpty())
            {
                core::ConsoleWrite(u8"AnimatedCrowd: no skinned mesh in model\n");
                return false;
            }
            return true;
        }

        // Merge the character's skinned mesh parts into ONE SkinnedMesh (one submesh per original part, so
        // materials survive). Skinned parts share the skeleton + render in skeleton-root space, so it's a
        // straight concat: append vertices + the parallel skinning stream, append indices offset by the
        // running vertex base. Lets the crowd draw the whole character as one set instead of N.
        [[nodiscard]] core::RefPtr<geometry::StaticMesh> MergeSkinnedParts()
        {
            core::RefPtr<geometry::SkinnedMesh> merged =
                core::MakeRef<geometry::SkinnedMesh>(AppRoot());
            core::u32 totalV = 0, totalI = 0;
            for (const Part& p : m_skinnedParts)
            {
                totalV += p.mesh->VertexCount();
                totalI += p.mesh->IndexCount();
            }
            merged->vertices.Reserve(totalV);
            merged->skinning.Reserve(totalV);
            merged->indices.Resize(
                totalI); // sets logical count + rewinds cursor: IndexBuffer::Add only fills up to m_count

            // Running write cursor into the merged index buffer. NOT merged->indices.Count() - Resize()
            // sets the logical count to totalI up front, so Count() reports the full size immediately;
            // the actual fill position is how many Add()s have happened (Add advances an internal cursor).
            core::u32 iwrite = 0;
            for (const Part& p : m_skinnedParts)
            {
                geometry::StaticMesh* sm = p.mesh.Get();
                const core::u32 vbase = merged->VertexCount();
                for (const geometry::StaticMeshVertex& v : sm->vertices)
                {
                    merged->vertices.PushBack(v);
                }
                const core::Span<const geometry::VertexSkinning> skin = sm->SkinningStream();
                for (core::usize k = 0; k < skin.Size(); ++k)
                {
                    merged->skinning.PushBack(skin[k]);
                }
                while (merged->skinning.Size() < merged->vertices.Size())
                {
                    merged->skinning.PushBack(geometry::VertexSkinning{});
                }

                // Emit a submesh per original submesh (preserving its material), offsetting index VALUES by
                // vbase; a part with no submeshes becomes one submesh with the part's material.
                if (sm->subMeshes.IsEmpty())
                {
                    geometry::SubMesh s;
                    s.startIndex = static_cast<core::i32>(iwrite);
                    s.indexCount = static_cast<core::i32>(sm->IndexCount());
                    s.materialIndex = (p.matIdx >= 0) ? p.matIdx : 0;
                    for (core::u32 k = 0; k < sm->IndexCount(); ++k)
                    {
                        merged->indices.Add(sm->indices.Get(k) + vbase);
                        ++iwrite;
                    }
                    merged->subMeshes.PushBack(s);
                }
                else
                {
                    for (const geometry::SubMesh& os : sm->subMeshes)
                    {
                        geometry::SubMesh s = os;
                        // The part's submeshes index its own slots; the merged mesh indexes the
                        // model's whole list.
                        s.materialIndex =
                            (os.materialIndex >= 0 && static_cast<core::usize>(os.materialIndex) < p.slots.Size())
                                ? p.slots[static_cast<core::usize>(os.materialIndex)]
                                : ((p.matIdx >= 0) ? p.matIdx : 0);
                        s.startIndex = static_cast<core::i32>(iwrite);
                        for (core::i32 k = 0; k < os.indexCount; ++k)
                        {
                            merged->indices.Add(
                                sm->indices.Get(static_cast<core::u32>(os.startIndex) +
                                                static_cast<core::u32>(k)) +
                                vbase);
                            ++iwrite;
                        }
                        merged->subMeshes.PushBack(s);
                    }
                }
            }
            if (!m_skinnedParts.IsEmpty())
            {
                merged->skeletonIndex =
                    static_cast<geometry::SkinnedMesh*>(m_skinnedParts[0].mesh.Get())
                        ->skeletonIndex;
            }
            merged->CalculateBounds();
            return core::RefPtr<geometry::StaticMesh>(merged.Get());
        }

        // Rebuild the crowd to `count` instances: ONE InstancedMeshComponent (the skinned mesh at a grid of
        // auto-fit-scaled transforms) + ONE InstancedSkinningComponent companion (M shared pose palettes). No per-
        // entity characters - the whole crowd is one draw per pass, animated by M palette computes, not N.
        void RebuildToCount(core::u32 count)
        {
            auto* imm = m_scene->GetSystem<engine::render::InstancedMeshComponentManager>();
            auto* anims = m_scene->GetSystem<engine::animation::InstancedSkinningComponentManager>();
            if (imm == nullptr || anims == nullptr || m_skinnedParts.IsEmpty())
            {
                return;
            }

            for (scene::EntityHandle e : m_crowdParts)
            {
                m_scene->DestroyEntity(e);
            } // tear down the old crowd
            m_crowdParts.Clear();

            const core::u32 side =
                (count == 0)
                    ? 1u
                    : static_cast<core::u32>(core::Ceil(core::Sqrt(static_cast<core::f32>(count))));
            const core::f32 half = (static_cast<core::f32>(side) - 1.0f) * 0.5f;

            // Bucket the crowd across the model's clips (round-robin), so it's a MIXED herd (walk/idle/run/...)
            // rather than one clip. Each clip is its own group: its subset of instances + per-instance tints +
            // its own InstancedSkinningComponent pose pool. Cost stays O(clips x M) palettes/frame, independent of count.
            // Single-clip collapses the herd to ONE clip/pose-pool so the spatial pose policies read cleanly
            // (with the mixed 6-clip herd, a column/wave spans different animations and looks muddled - the
            // phase pattern is there, but overlaid on 6 different clips). Off = the mixed-herd benchmark.
            const core::u32 numClips =
                m_singleClip ? 1u
                             : core::Min(static_cast<core::u32>(m_clips.Size()), kMaxClipGroups);
            if (numClips == 0 || !m_model->skeleton)
            {
                m_crowdCount = count;
                AutoFrame(side);
                return;
            }

            core::Array<core::Array<core::Float4x4>> clipXf;
            clipXf.Resize(numClips);
            core::Array<core::Array<core::Color>> clipTint;
            clipTint.Resize(numClips);
            core::Array<core::Array<core::u32>> clipPose;
            clipPose.Resize(numClips); // per-instance pose index (Explicit policies)
            const render::PoseAssignment assign = PoseAssignmentFor(m_posePolicy);
            for (core::u32 i = 0; i < count; ++i)
            {
                const core::u32 col = i % side;
                const core::u32 rowi = i / side;
                const core::f32 px = (static_cast<core::f32>(col) - half) * kCharacterSpacing;
                const core::f32 pz = (static_cast<core::f32>(rowi) - half) * kCharacterSpacing;
                core::Transform t;
                t.position = core::Float3{px, kFloorY, pz};
                t.scale = core::Float3{m_fit, m_fit, m_fit};
                const core::u32 g =
                    i % numClips; // round-robin -> clips spread evenly across the grid
                clipXf[g].PushBack(t.ToMatrix());
                clipTint[g].PushBack(m_tintEnabled
                                         ? ClipTint(g)
                                         : core::Color{1.0f, 1.0f, 1.0f, 1.0f}); // white == no tint
                // For the layout-aware policies the renderer can't compute from the flat index, precompute
                // this instance's pose index here (we have its grid col/row) and hand it over as Explicit.
                if (assign == render::PoseAssignment::Explicit)
                {
                    clipPose[g].PushBack(PoseIndexFor(col, rowi, side));
                }
            }

            // The meshes to instance per clip group: the merged single mesh, or the N skinned parts.
            core::Array<core::RefPtr<geometry::StaticMesh>> drawMeshes;
            core::Array<core::RefPtr<materials::Material>> drawMats;
            core::Array<core::Array<core::RefPtr<materials::Material>>> drawSubMats; // per draw-mesh
            if (m_mergeMeshes)
            {
                if (!m_mergedMesh)
                {
                    m_mergedMesh = MergeSkinnedParts();
                }
                drawMeshes.PushBack(m_mergedMesh);
                drawMats.PushBack(m_skinnedParts.IsEmpty() ? core::RefPtr<materials::Material>{}
                                                           : m_skinnedParts[0].mat);
                drawSubMats.PushBack(m_modelMats); // the merged mesh indexes the model's list
            }
            else
            {
                for (const Part& part : m_skinnedParts)
                {
                    drawMeshes.PushBack(part.mesh);
                    drawMats.PushBack(part.mat);
                    core::Array<core::RefPtr<materials::Material>> own; // the part's slots
                    for (const core::i32 slot : part.slots)
                    {
                        own.PushBack(static_cast<core::usize>(slot) < m_modelMats.Size()
                                         ? m_modelMats[static_cast<core::usize>(slot)]
                                         : core::RefPtr<materials::Material>{});
                    }
                    drawSubMats.PushBack(core::Move(own));
                }
            }

            // One group per clip: an InstancedMeshComponent per draw-mesh (its subset of transforms +
            // per-instance tints) + one InstancedSkinningComponent driving them all with that clip's shared pose pool.
            for (core::u32 g = 0; g < numClips; ++g)
            {
                if (clipXf[g].IsEmpty())
                {
                    continue;
                }
                core::Array<scene::EntityHandle> targets;
                for (core::usize p = 0; p < drawMeshes.Size(); ++p)
                {
                    scene::EntityHandle e = m_scene->CreateEntity(u8"crowd_part");
                    engine::render::InstancedMeshComponent& c = imm->Add(e);
                    c.mesh = drawMeshes[p];
                    c.material = drawMats[p];
                    c.submeshMaterials = drawSubMats[p];
                    c.tints =
                        clipTint[g]; // set BEFORE SetInstances (the version bump uploads them)
                    c.poseAssignment =
                        assign; // how each instance picks its pose (read each frame, not uploaded)
                    if (assign == render::PoseAssignment::Explicit)
                    {
                        c.poseIndices = clipPose[g];
                    }
                    c.SetInstances(
                        core::Span<const core::Float4x4>{clipXf[g].Data(), clipXf[g].Size()});
                    m_crowdParts.PushBack(e);
                    targets.PushBack(e);
                }
                scene::EntityHandle animE = m_scene->CreateEntity(u8"crowd_anim");
                engine::animation::InstancedSkinningComponent& s = anims->Add(animE);
                s.skeleton = m_model->skeleton.Get();
                s.clip = m_clips[g];
                s.poseCount = kPoseCount;
                s.targets = static_cast<core::Array<scene::EntityHandle>&&>(targets);
                m_crowdParts.PushBack(animE);
            }

            m_crowdCount = count;
            m_clipGroups = numClips;
            AutoFrame(side);
            m_frameTimeMs =
                16.6f; // reset the smoother so the rebuild hitch doesn't skew the reading
            core::ConsoleWrite(core::Format(
                u8"AnimatedCrowd: characters={}  sets/char={}  clips={}  poses={}\n", m_crowdCount,
                m_mergeMeshes ? 1u : static_cast<core::u32>(m_skinnedParts.Size()), numClips,
                kPoseCount));
        }

        // A distinct base colour per clip group (so the mixed-clip crowd is obvious at a glance) plus a
        // per-character brightness jitter (so a group isn't flat). Multiplied into the material albedo.
        [[nodiscard]] core::Color ClipTint(core::u32 group)
        {
            static constexpr core::Float3 kClipColors[kMaxClipGroups] = {
                {1.00f, 0.45f, 0.40f}, // red
                {0.45f, 0.90f, 0.50f}, // green
                {0.45f, 0.65f, 1.00f}, // blue
                {1.00f, 0.85f, 0.35f}, // yellow
                {0.90f, 0.50f, 1.00f}, // magenta
                {0.45f, 0.95f, 0.95f}, // cyan
            };
            const core::Float3 base = kClipColors[group % kMaxClipGroups];
            const core::f32 v =
                0.7f + m_rng.NextFloat() * 0.5f; // per-character brightness 0.7..1.2
            return core::Color{base.x * v, base.y * v, base.z * v, 1.0f};
        }

        // Map the HUD pose policy to a renderer PoseAssignment. Random is a function of the flat index the
        // renderer computes itself (Hashed, no array). Wave/Columns/Clusters are all layout-aware (derived
        // from the character's grid position, which the renderer can't see) -> Explicit + a per-instance
        // array. (Wave is Explicit, NOT the renderer's Sequential: Sequential uses the per-set LOCAL index,
        // and each clip set here holds every-Nth character, so it would be spatially scrambled, not a wave.)
        [[nodiscard]] static render::PoseAssignment PoseAssignmentFor(PosePolicy p)
        {
            return (p == PosePolicy::Random) ? render::PoseAssignment::Hashed
                                             : render::PoseAssignment::Explicit;
        }

        // Pose index for the layout-aware policies, from a character's grid column/row (pure render helpers,
        // unit-tested). Wave = diagonal phase gradient; Columns = whole column shares a phase (formation);
        // Clusters = 4×4-character cells share a phase, hashed so neighbours differ. Only called for Explicit.
        [[nodiscard]] core::u32 PoseIndexFor(core::u32 col, core::u32 row, core::u32 side) const
        {
            switch (m_posePolicy)
            {
            case PosePolicy::Wave:
                return render::WavePose(col, row, kPoseCount);
            case PosePolicy::Columns:
                return render::ColumnPose(col, kPoseCount);
            case PosePolicy::Clusters:
                return render::ClusterPose(col, row, 4, kPoseCount);
            case PosePolicy::Custom:
            {
                // A bespoke index scheme authored right here in the sample - NO render helper - to show
                // the Explicit contract accepts ANY per-instance index the caller computes itself.
                // Concentric rings: quantise each character's distance from the grid centre into a pose
                // bucket, so the animation phase ripples outward in rings.
                const core::f32 c = static_cast<core::f32>(side) * 0.5f;
                const core::f32 dx = static_cast<core::f32>(col) - c;
                const core::f32 dz = static_cast<core::f32>(row) - c;
                return static_cast<core::u32>(core::Sqrt(dx * dx + dz * dz)) % kPoseCount;
            }
            default:
                return 0;
            }
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

        void AddBatch() { RebuildToCount(static_cast<core::u32>(m_crowdCount) + kBatchSize); }
        void RemoveBatch()
        {
            const core::u32 n = static_cast<core::u32>(m_crowdCount);
            RebuildToCount(n > kBatchSize ? n - kBatchSize : 0u);
        }

        // Flip the directional light's CSM on/off (K, or the HUD checkbox) - isolate skinning throughput
        // from the shadow-cascade GPU cost.
        void ToggleShadows()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            if (auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>())
            {
                if (engine::render::LightComponent* kl =
                        m_keyLight.IsAssigned() ? lights->Get(m_keyLight) : nullptr)
                {
                    kl->castsShadows = !kl->castsShadows;
                    core::ConsoleWrite(kl->castsShadows ? u8"Directional shadows: ON\n"
                                                        : u8"Directional shadows: OFF\n");
                }
            }
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
                    if (kb->IsKeyPressed(shell::KeyCode::K))
                    {
                        ToggleShadows();
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
                        u8"=== AnimatedCrowd PROFILE: chars={}  fps={}  frame={} ms ===\n",
                        m_crowdCount, static_cast<core::u32>(fps + 0.5f), m_frameTimeMs));
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
                    const core::u32 n = static_cast<core::u32>(m_crowdCount);
                    if (fps <= 50.0f)
                    {
                        core::ConsoleWrite(core::Format(
                            u8"AnimatedCrowd: THRESHOLD chars={}  fps={}  frame={} ms\n", n,
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
            ImGui::Text("characters: %d   clips: %d x %d poses (%d palettes/frame)",
                        static_cast<int>(m_crowdCount), static_cast<int>(m_clipGroups),
                        static_cast<int>(kPoseCount), static_cast<int>(m_clipGroups * kPoseCount));
            bool tint = m_tintEnabled;
            if (ImGui::Checkbox("Per-instance tint (colour-code by clip)", &tint))
            {
                m_tintEnabled = tint;
                RebuildToCount(m_crowdCount);
            }
            bool merge = m_mergeMeshes;
            if (ImGui::Checkbox("Merge parts into one mesh", &merge))
            {
                m_mergeMeshes = merge;
                RebuildToCount(m_crowdCount);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d set%s/char)",
                                m_mergeMeshes ? 1 : static_cast<int>(m_skinnedParts.Size()),
                                m_mergeMeshes ? "" : "s");
            static const char* kPosePolicyNames[] = {"Random (hashed)", "Wave (diagonal)",
                                                     "Columns", "Clusters", "Custom (rings)"};
            int policy = static_cast<int>(m_posePolicy);
            if (ImGui::Combo("Pose assignment", &policy, kPosePolicyNames, 5))
            {
                m_posePolicy = static_cast<PosePolicy>(policy);
                RebuildToCount(m_crowdCount);
            }
            bool single = m_singleClip;
            if (ImGui::Checkbox("Single clip (isolate pose modes)", &single))
            {
                m_singleClip = single;
                RebuildToCount(m_crowdCount);
            }
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
                bool shadowsOn = false;
                if (m_scene != nullptr)
                {
                    if (auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>())
                    {
                        if (engine::render::LightComponent* kl =
                                m_keyLight.IsAssigned() ? lights->Get(m_keyLight) : nullptr)
                        {
                            shadowsOn = kl->castsShadows;
                            if (ImGui::Checkbox("Directional shadows (K)", &shadowsOn))
                            {
                                kl->castsShadows = shadowsOn;
                            }
                        }
                    }
                }
                if (!shadowsOn)
                {
                    ImGui::TextDisabled("(shadows off - isolates skinning throughput)");
                }
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
            core::ConsoleWrite(u8"AnimatedCrowd: shutting down.\n");
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

        // The crowd: ONE entity carrying an InstancedMeshComponent (the skinned mesh at N transforms) + an
        // InstancedSkinningComponent companion (M shared pose palettes). m_crowdCount tracks the instance count.
        // A skinned mesh part of the character + its material (+ global material index, for merged submeshes).
        struct Part
        {
            core::RefPtr<geometry::StaticMesh> mesh;
            core::RefPtr<materials::Material> mat;
            core::i32 matIdx = -1;
            core::Array<core::i32> slots; // the model-wide materials its submeshes index
        };
        scene::EntityHandle m_keyLight{}; // directional CSM light (K toggles its shadows)
        core::Array<Part>
            m_skinnedParts; // every skinned mesh of the model (shared across the crowd)
        core::Array<scene::EntityHandle>
            m_crowdParts; // all per-clip-group set + anim entities, torn down together
        core::u32 m_crowdCount = 0;
        core::u32 m_clipGroups = 1;
        bool m_tintEnabled = true; // per-instance/per-clip tint (HUD toggle)
        bool m_mergeMeshes =
            false; // merge the character's skinned parts into one mesh (HUD toggle)
        PosePolicy m_posePolicy =
            PosePolicy::Random;    // how each character picks its shared pose (HUD)
        bool m_singleClip = false; // collapse to 1 clip/pool so pose modes read cleanly (HUD)
        core::RefPtr<geometry::StaticMesh>
            m_mergedMesh; // cached merged mesh (one submesh per original part)
        core::Random m_rng{0x9e3779b97f4a7c15ull};
        static constexpr core::u32 kPoseCount = 32; // M unique phase buckets per shared pose pool
        static constexpr core::u32 kMaxClipGroups =
            6; // cap on distinct clips the crowd mixes across
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

    AnimatedCrowdApp app;
    app.SetDataRoot(foundation::vfs::DataRootFromArguments(argc, argv).AsView());
    app.OnCommandLine(argc, argv); // --screenshot and whatever else the base app reads
    return runtime::RunApplication(app, *shell, device);
}
