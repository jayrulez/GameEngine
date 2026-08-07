// Sandbox - the running dev harness. It extends DefaultApplication (which registers
// the SceneSubsystem + RenderSubsystem and renders active scenes each frame), creates a
// scene with two spinning cube grids (instanced + distinct), and lets the engine draw it.
// As the renderer grows, this is where we exercise it.

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Profiler/Profiler.h" // DRACONIC_PROFILE_SCOPE (isolate animation-drive cost)
#include "imgui.h"             // Dear ImGui (HUD) - used directly; integration is draconic.imgui

import draconic.foundation;
import draconic.profiler;
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
import draconic.render;           // ViewCamera / ViewportRect (split-screen overrides)
import draconic.imgui;            // ImguiSubsystem (HUD)
import draconic.geometry;
import draconic.geometry.resource; // StaticMeshFactory + StaticMesh product
import draconic.materials;
import draconic.materials.resource;  // MaterialFactory (cooked materials)
import draconic.texture.resource;    // TextureFactory (cooked textures)
import draconic.animation.resource;  // Skeleton/AnimationClip factories
import draconic.vfs;                 // NativeFileSystem mount for the content DB
import draconic.content;             // ContentDatabase (cooked-resource output)
import draconic.resource;            // ResourceManager + Proxy
import draconic.model;               // ModelLoadResult
import draconic.modelimporter;       // LoadAndCook + ImportedModel manifest
import draconic.animation;           // AnimationClip / Skeleton
import draconic.engine.animation; // SkeletalAnimationComponent(Manager) - engine-driven skinning

#include "../Common/FlyCamera.h" // shared free-fly camera (uses the imported runtime/foundation types)

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
namespace imgui = draconic::imgui;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace texture = draconic::texture;
namespace vfs = draconic::vfs;
namespace content = draconic::content;
namespace resource = draconic::resource;
namespace model = draconic::model;
namespace modelimporter = draconic::modelimporter;
namespace animation = draconic::animation;

namespace
{
    // How many characters each +/- press adds or removes (and the initial spawn).
    static constexpr foundation::u32 kBatchSize = 500;
    static constexpr foundation::f32 kCharacterSpacing = 8.0f; // grid spacing (world units)
    static constexpr foundation::f32 kCharacterSize = 6.0f; // auto-fit target height (matches CookModel)
    static constexpr foundation::f32 kFloorY = -7.0f;
    static constexpr foundation::f32 kFloorBaseSize =
        120.0f; // base floor-plane size (scaled to cover the grid)

    class AnimatedCrowdApp final : public runtime::DefaultApplication
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

            // CreateScene triggers the RenderSubsystem to inject the render managers.
            m_scene = PrimaryScenes().CreateScene(u8"sandbox");

            // Per-scene environment ambient (a dim cool indirect term; IBL replaces it later).
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                env->Environment().ambientColor = foundation::Color{0.12f, 0.16f, 0.28f, 1.0f};
                env->Environment().ambientIntensity = 0.35f;
            }

            // camera, pulled back along +Z looking at the origin (down -Z by default)
            // Raised + pitched down so the horizontal floor (lights above it) is clearly in view,
            // with the cube grids standing on it. Pitch ~28 deg below horizontal (looks toward the
            // scene center). Default camera looks down -Z; rotating about +X by -pitch tilts it down.
            m_camera = m_scene->CreateEntity(u8"camera");
            m_scene->SetLocalPosition(m_camera, foundation::Float3{0.0f, 14.0f, 30.0f});
            foundation::Transform camT = m_scene->GetLocalTransform(m_camera);
            camT.rotation = foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -0.48f);
            m_scene->SetLocalTransform(m_camera, camT);
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                render::CameraComponent& cam = cameras->Add(m_camera); // default 60deg perspective
                cam.clearColor =
                    foundation::Color{0.02f, 0.02f, 0.03f, 1.0f}; // dark backdrop so the lit scene reads
            }

            // A large horizontal floor (Plane normal = +Y) under the scene - the animated models stand
            // on it and the lights cast their shadows onto it.
            if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
            {
                m_floor = m_scene->CreateEntity(u8"floor");
                m_scene->SetLocalPosition(m_floor, foundation::Float3{0.0f, -7.0f, 0.0f});
                render::MeshComponent& fmc = meshes->Add(m_floor);
                fmc.mesh = geometry::Primitives::Plane(kFloorBaseSize, kFloorBaseSize);
                fmc.SetMaterial(materials::CreatePBR(u8"lit", foundation::Float4{0.5f, 0.5f, 0.53f, 1.0f},
                                                     0.0f, 0.65f));
            }

            // One directional shadow-casting key light - the whole scene (skinning benchmark, kept light
            // to isolate skinning/animation cost, à la Flax's "5,000 basic characters" reference scene).
            if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
            {
                m_keyLight = m_scene->CreateEntity(u8"keyLight");
                foundation::Transform kt = m_scene->GetLocalTransform(m_keyLight);
                kt.rotation =
                    foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -0.9f) *
                    foundation::Quaternion::FromAxisAngle(foundation::Float3{0.0f, 1.0f, 0.0f}, 0.5f);
                m_scene->SetLocalTransform(m_keyLight, kt);
                render::LightComponent& kl = lights->Add(m_keyLight);
                kl.type = render::LightType::Directional;
                kl.color = foundation::Color{1.0f, 0.97f, 0.92f, 1.0f};
                kl.intensity = 2.5f;
                kl.castsShadows = true; // directional CSM (K toggles)
            }

            LoadImportedModel(host); // cook the character + spawn the initial grid

            // Lower default exposure: the procedural-sky IBL + sun are bright, so AgX washes out at 1.0.
            if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
            {
                render->SetExposure(0.5f);
            }

            foundation::ConsoleWrite(u8"AnimatedCrowd: [Space] add batch  [Backspace] remove batch  [P] "
                               u8"profiler  [Esc] exit\n");
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

            // Cook the Quaternius humanoid once, then replicate it across a grid (each instance gets its
            // own AnimationPlayer; all share the cooked mesh/skeleton/clips/materials).
            if (!CookModel(u8"Char",
                           foundation::Format(u8"{}/QuaterniusCharacter/glTF/Character.gltf", modelDir)
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
        bool CookModel(foundation::StringView prefix, foundation::StringView path)
        {
            if (m_contentDb.Get() == nullptr)
            {
                return false;
            }
            foundation::Guid modelGuid;
            const model::ModelLoadResult r =
                modelimporter::LoadAndCook(path, *m_contentDb, prefix, modelGuid);
            if (r != model::ModelLoadResult::Ok)
            {
                foundation::ConsoleWrite(foundation::Format(u8"AnimatedCrowd: model import failed ({})\n",
                                                static_cast<foundation::u32>(r)));
                return false;
            }
            m_model = m_resources->Bind<model::ModelResource>(modelGuid);
            if (!m_model)
            {
                foundation::ConsoleWrite(u8"AnimatedCrowd: model bind failed\n");
                return false;
            }

            // Auto-fit: scale the model's largest extent to a target size.
            constexpr foundation::f32 kTargetSize = 6.0f;
            const foundation::Float3 extent = m_model->boundsMax - m_model->boundsMin;
            const foundation::f32 maxExtent = foundation::Max(extent.x, foundation::Max(extent.y, extent.z));
            m_fit = (maxExtent > 0.0001f) ? (kTargetSize / maxExtent) : 1.0f;

            // All materials, indexed by SubMesh::materialIndex (multi-material).
            m_modelMats.Reserve(m_model->materials.Size());
            for (auto& mp : m_model->materials)
            {
                m_modelMats.PushBack(foundation::RefPtr<materials::Material>(mp.Get()));
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
            for (foundation::usize i = 0; i < m_model->meshes.Size(); ++i)
            {
                geometry::StaticMesh* mesh = m_model->meshes[i].Get();
                if (mesh == nullptr || !mesh->IsSkinned())
                {
                    continue;
                }
                const foundation::i32 matIdx =
                    (i < m_model->meshMaterial.Size()) ? m_model->meshMaterial[i] : -1;
                foundation::RefPtr<materials::Material> mat =
                    (matIdx >= 0 && static_cast<foundation::usize>(matIdx) < m_modelMats.Size())
                        ? m_modelMats[static_cast<foundation::usize>(matIdx)]
                        : (m_modelMats.IsEmpty() ? foundation::RefPtr<materials::Material>{}
                                                 : m_modelMats[0]);
                m_skinnedParts.PushBack(
                    Part{foundation::RefPtr<geometry::StaticMesh>(mesh), mat, matIdx});
            }
            if (m_skinnedParts.IsEmpty())
            {
                foundation::ConsoleWrite(u8"AnimatedCrowd: no skinned mesh in model\n");
                return false;
            }
            return true;
        }

        // Merge the character's skinned mesh parts into ONE SkinnedMesh (one submesh per original part, so
        // materials survive). Skinned parts share the skeleton + render in skeleton-root space, so it's a
        // straight concat: append vertices + the parallel skinning stream, append indices offset by the
        // running vertex base. Lets the crowd draw the whole character as one set instead of N.
        [[nodiscard]] foundation::RefPtr<geometry::StaticMesh> MergeSkinnedParts()
        {
            foundation::RefPtr<geometry::SkinnedMesh> merged =
                foundation::MakeRef<geometry::SkinnedMesh>(foundation::DefaultAllocator());
            foundation::u32 totalV = 0, totalI = 0;
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
            foundation::u32 iwrite = 0;
            for (const Part& p : m_skinnedParts)
            {
                geometry::StaticMesh* sm = p.mesh.Get();
                const foundation::u32 vbase = merged->VertexCount();
                for (const geometry::StaticMeshVertex& v : sm->vertices)
                {
                    merged->vertices.PushBack(v);
                }
                const foundation::Span<const geometry::VertexSkinning> skin = sm->SkinningStream();
                for (foundation::usize k = 0; k < skin.Size(); ++k)
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
                    s.startIndex = static_cast<foundation::i32>(iwrite);
                    s.indexCount = static_cast<foundation::i32>(sm->IndexCount());
                    s.materialIndex = (p.matIdx >= 0) ? p.matIdx : 0;
                    for (foundation::u32 k = 0; k < sm->IndexCount(); ++k)
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
                        s.startIndex = static_cast<foundation::i32>(iwrite);
                        for (foundation::i32 k = 0; k < os.indexCount; ++k)
                        {
                            merged->indices.Add(
                                sm->indices.Get(static_cast<foundation::u32>(os.startIndex) +
                                                static_cast<foundation::u32>(k)) +
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
            return foundation::RefPtr<geometry::StaticMesh>(merged.Get());
        }

        // Rebuild the crowd to `count` instances: ONE InstancedMeshComponent (the skinned mesh at a grid of
        // auto-fit-scaled transforms) + ONE InstancedSkinningComponent companion (M shared pose palettes). No per-
        // entity characters - the whole crowd is one draw per pass, animated by M palette computes, not N.
        void RebuildToCount(foundation::u32 count)
        {
            auto* imm = m_scene->GetSystem<render::InstancedMeshComponentManager>();
            auto* anims = m_scene->GetSystem<animation::InstancedSkinningComponentManager>();
            if (imm == nullptr || anims == nullptr || m_skinnedParts.IsEmpty())
            {
                return;
            }

            for (scene::EntityHandle e : m_crowdParts)
            {
                m_scene->DestroyEntity(e);
            } // tear down the old crowd
            m_crowdParts.Clear();

            const foundation::u32 side =
                (count == 0)
                    ? 1u
                    : static_cast<foundation::u32>(foundation::Ceil(foundation::Sqrt(static_cast<foundation::f32>(count))));
            const foundation::f32 half = (static_cast<foundation::f32>(side) - 1.0f) * 0.5f;

            // Bucket the crowd across the model's clips (round-robin), so it's a MIXED herd (walk/idle/run/...)
            // rather than one clip. Each clip is its own group: its subset of instances + per-instance tints +
            // its own InstancedSkinningComponent pose pool. Cost stays O(clips x M) palettes/frame, independent of count.
            // Single-clip collapses the herd to ONE clip/pose-pool so the spatial pose policies read cleanly
            // (with the mixed 6-clip herd, a column/wave spans different animations and looks muddled - the
            // phase pattern is there, but overlaid on 6 different clips). Off = the mixed-herd benchmark.
            const foundation::u32 numClips =
                m_singleClip ? 1u
                             : foundation::Min(static_cast<foundation::u32>(m_clips.Size()), kMaxClipGroups);
            if (numClips == 0 || !m_model->skeleton)
            {
                m_crowdCount = count;
                AutoFrame(side);
                return;
            }

            foundation::Array<foundation::Array<foundation::Float4x4>> clipXf;
            clipXf.Resize(numClips);
            foundation::Array<foundation::Array<foundation::Color>> clipTint;
            clipTint.Resize(numClips);
            foundation::Array<foundation::Array<foundation::u32>> clipPose;
            clipPose.Resize(numClips); // per-instance pose index (Explicit policies)
            const render::PoseAssignment assign = PoseAssignmentFor(m_posePolicy);
            for (foundation::u32 i = 0; i < count; ++i)
            {
                const foundation::u32 col = i % side;
                const foundation::u32 rowi = i / side;
                const foundation::f32 px = (static_cast<foundation::f32>(col) - half) * kCharacterSpacing;
                const foundation::f32 pz = (static_cast<foundation::f32>(rowi) - half) * kCharacterSpacing;
                foundation::Transform t;
                t.position = foundation::Float3{px, kFloorY, pz};
                t.scale = foundation::Float3{m_fit, m_fit, m_fit};
                const foundation::u32 g =
                    i % numClips; // round-robin -> clips spread evenly across the grid
                clipXf[g].PushBack(t.ToMatrix());
                clipTint[g].PushBack(m_tintEnabled
                                         ? ClipTint(g)
                                         : foundation::Color{1.0f, 1.0f, 1.0f, 1.0f}); // white == no tint
                // For the layout-aware policies the renderer can't compute from the flat index, precompute
                // this instance's pose index here (we have its grid col/row) and hand it over as Explicit.
                if (assign == render::PoseAssignment::Explicit)
                {
                    clipPose[g].PushBack(PoseIndexFor(col, rowi, side));
                }
            }

            // The meshes to instance per clip group: the merged single mesh, or the N skinned parts.
            foundation::Array<foundation::RefPtr<geometry::StaticMesh>> drawMeshes;
            foundation::Array<foundation::RefPtr<materials::Material>> drawMats;
            if (m_mergeMeshes)
            {
                if (!m_mergedMesh)
                {
                    m_mergedMesh = MergeSkinnedParts();
                }
                drawMeshes.PushBack(m_mergedMesh);
                drawMats.PushBack(m_skinnedParts.IsEmpty() ? foundation::RefPtr<materials::Material>{}
                                                           : m_skinnedParts[0].mat);
            }
            else
            {
                for (const Part& part : m_skinnedParts)
                {
                    drawMeshes.PushBack(part.mesh);
                    drawMats.PushBack(part.mat);
                }
            }

            // One group per clip: an InstancedMeshComponent per draw-mesh (its subset of transforms +
            // per-instance tints) + one InstancedSkinningComponent driving them all with that clip's shared pose pool.
            for (foundation::u32 g = 0; g < numClips; ++g)
            {
                if (clipXf[g].IsEmpty())
                {
                    continue;
                }
                foundation::Array<scene::EntityHandle> targets;
                for (foundation::usize p = 0; p < drawMeshes.Size(); ++p)
                {
                    scene::EntityHandle e = m_scene->CreateEntity(u8"crowd_part");
                    render::InstancedMeshComponent& c = imm->Add(e);
                    c.mesh = drawMeshes[p];
                    c.material = drawMats[p];
                    c.submeshMaterials = m_modelMats;
                    c.tints =
                        clipTint[g]; // set BEFORE SetInstances (the version bump uploads them)
                    c.poseAssignment =
                        assign; // how each instance picks its pose (read each frame, not uploaded)
                    if (assign == render::PoseAssignment::Explicit)
                    {
                        c.poseIndices = clipPose[g];
                    }
                    c.SetInstances(
                        foundation::Span<const foundation::Float4x4>{clipXf[g].Data(), clipXf[g].Size()});
                    m_crowdParts.PushBack(e);
                    targets.PushBack(e);
                }
                scene::EntityHandle animE = m_scene->CreateEntity(u8"crowd_anim");
                animation::InstancedSkinningComponent& s = anims->Add(animE);
                s.skeleton = m_model->skeleton.Get();
                s.clip = m_clips[g];
                s.poseCount = kPoseCount;
                s.targets = static_cast<foundation::Array<scene::EntityHandle>&&>(targets);
                m_crowdParts.PushBack(animE);
            }

            m_crowdCount = count;
            m_clipGroups = numClips;
            AutoFrame(side);
            m_frameTimeMs =
                16.6f; // reset the smoother so the rebuild hitch doesn't skew the reading
            foundation::ConsoleWrite(foundation::Format(
                u8"AnimatedCrowd: characters={}  sets/char={}  clips={}  poses={}\n", m_crowdCount,
                m_mergeMeshes ? 1u : static_cast<foundation::u32>(m_skinnedParts.Size()), numClips,
                kPoseCount));
        }

        // A distinct base colour per clip group (so the mixed-clip crowd is obvious at a glance) plus a
        // per-character brightness jitter (so a group isn't flat). Multiplied into the material albedo.
        [[nodiscard]] foundation::Color ClipTint(foundation::u32 group)
        {
            static constexpr foundation::Float3 kClipColors[kMaxClipGroups] = {
                {1.00f, 0.45f, 0.40f}, // red
                {0.45f, 0.90f, 0.50f}, // green
                {0.45f, 0.65f, 1.00f}, // blue
                {1.00f, 0.85f, 0.35f}, // yellow
                {0.90f, 0.50f, 1.00f}, // magenta
                {0.45f, 0.95f, 0.95f}, // cyan
            };
            const foundation::Float3 base = kClipColors[group % kMaxClipGroups];
            const foundation::f32 v =
                0.7f + m_rng.NextFloat() * 0.5f; // per-character brightness 0.7..1.2
            return foundation::Color{base.x * v, base.y * v, base.z * v, 1.0f};
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
        [[nodiscard]] foundation::u32 PoseIndexFor(foundation::u32 col, foundation::u32 row, foundation::u32 side) const
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
                const foundation::f32 c = static_cast<foundation::f32>(side) * 0.5f;
                const foundation::f32 dx = static_cast<foundation::f32>(col) - c;
                const foundation::f32 dz = static_cast<foundation::f32>(row) - c;
                return static_cast<foundation::u32>(foundation::Sqrt(dx * dx + dz * dz)) % kPoseCount;
            }
            default:
                return 0;
            }
        }

        // Position the fly camera so the whole side×side grid is in frame + grow the floor under it (called
        // on every batch change).
        void AutoFrame(foundation::u32 side)
        {
            const foundation::f32 extent =
                (static_cast<foundation::f32>(side) - 1.0f) * kCharacterSpacing * 0.5f + kCharacterSize;
            // Floor: scale the base plane so it covers the whole grid + margin (uniform XZ; Y stays flat).
            const foundation::f32 fscale = foundation::Max(1.0f, (extent * 2.0f + 40.0f) / kFloorBaseSize);
            foundation::Transform ft = m_scene->GetLocalTransform(m_floor);
            ft.scale = foundation::Float3{fscale, 1.0f, fscale};
            m_scene->SetLocalTransform(m_floor, ft);
            const foundation::Float3 target{0.0f, kFloorY + kCharacterSize * 0.5f, 0.0f}; // grid center
            const foundation::f32 dist = extent / foundation::Tan(0.5236f) +
                                   kCharacterSize * 2.0f; // fit 60° FOV horizontally + margin
            const foundation::f32 camY = extent * 0.55f + kCharacterSize;
            m_fly.position = foundation::Float3{target.x, target.y + camY, target.z + dist};
            m_fly.yaw = 0.0f;
            m_fly.pitch = -foundation::Atan2(camY, dist); // look down onto the grid center
            // Extend the far plane to cover the whole grid from this distance, so no characters get
            // frustum-far-culled (which would make the throughput measurement cheaper than it is).
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                if (render::CameraComponent* cam = cameras->Get(m_camera))
                {
                    cam->nearZ = 0.5f;
                    cam->farZ = dist + extent * 2.0f + 100.0f;
                }
            }
        }

        void AddBatch() { RebuildToCount(static_cast<foundation::u32>(m_crowdCount) + kBatchSize); }
        void RemoveBatch()
        {
            const foundation::u32 n = static_cast<foundation::u32>(m_crowdCount);
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
            if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
            {
                if (render::LightComponent* kl =
                        m_keyLight.IsAssigned() ? lights->Get(m_keyLight) : nullptr)
                {
                    kl->castsShadows = !kl->castsShadows;
                    foundation::ConsoleWrite(kl->castsShadows ? u8"Directional shadows: ON\n"
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

            // HUD over the scene (backbuffer is RenderTarget after the default render path).
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

        void OnUpdate(runtime::IApplicationHost& host, foundation::f32 deltaTime) override
        {
            runtime::DefaultApplication::OnUpdate(host, deltaTime); // keep the P-key profiling dump

            // ImGui HUD: open the frame + build the stats window (drawn in OnRenderWindow).
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildHud(host.Ctx().GetSubsystem<render::RenderSubsystem>());
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
                        if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
                        {
                            const bool on = !render->InstanceSharing();
                            render->SetInstanceSharing(on);
                            foundation::ConsoleWrite(on ? u8"Instance sharing: ON\n"
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
                foundation::Transform camT = m_scene->GetLocalTransform(m_camera);
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
                    const foundation::f32 fps =
                        (m_frameTimeMs > 0.001f) ? (1000.0f / m_frameTimeMs) : 0.0f;
                    foundation::ConsoleWrite(foundation::Format(
                        u8"=== AnimatedCrowd PROFILE: chars={}  fps={}  frame={} ms ===\n",
                        m_crowdCount, static_cast<foundation::u32>(fps + 0.5f), m_frameTimeMs));
                    foundation::ConsoleWrite(draconic::profiler::Profiler::Get().BuildReport().AsView());
                    if (auto* renderer = host.Ctx().GetSubsystem<render::RenderSubsystem>())
                    {
                        foundation::String gpu;
                        renderer->BuildGpuProfileReport(gpu);
                        foundation::ConsoleWrite(gpu.AsView());
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
                    const foundation::f32 fps =
                        (m_frameTimeMs > 0.001f) ? (1000.0f / m_frameTimeMs) : 0.0f;
                    const foundation::u32 n = static_cast<foundation::u32>(m_crowdCount);
                    if (fps <= 50.0f)
                    {
                        foundation::ConsoleWrite(foundation::Format(
                            u8"AnimatedCrowd: THRESHOLD chars={}  fps={}  frame={} ms\n", n,
                            static_cast<foundation::u32>(fps + 0.5f), m_frameTimeMs));
                        host.RequestExit(0);
                        return;
                    }
                    const foundation::u32 next =
                        (fps > 60.0f) ? foundation::Max(n + 50u, n + n / 4u) : (n + kBatchSize);
                    RebuildToCount(next);
                }
            }
        }

        // ImGui HUD: character count + frame stats + exposure/bloom controls (H toggles it).
        void BuildHud(render::RenderSubsystem* render)
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
                    foundation::u32 culled = 0, total = 0;
                    render->ViewCullStats(culled, total);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u/%u culled)", culled, total);
                }
                ImGui::Separator();
                bool shadowsOn = false;
                if (m_scene != nullptr)
                {
                    if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
                    {
                        if (render::LightComponent* kl =
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
                float shadowDist = render->ShadowDistance();
                if (ImGui::SliderFloat("Distance", &shadowDist, 50.0f, 1000.0f, "%.0f"))
                {
                    render->SetShadowDistance(shadowDist);
                }
                float shadowFade = render->ShadowFarFade();
                if (ImGui::SliderFloat("Far fade", &shadowFade, 2.0f, 150.0f, "%.0f"))
                {
                    render->SetShadowFarFade(shadowFade);
                }
                ImGui::TextDisabled("shadows fade out over the last %.0f units",
                                    static_cast<double>(shadowFade));
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
            foundation::ConsoleWrite(u8"AnimatedCrowd: shutting down.\n");
        }

    private:
        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_floor{};
        samples::FlyCamera m_fly{.position = foundation::Float3{0.0f, 10.0f, 26.0f}, .pitch = -0.25f};

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
        model::ModelFactory m_modelFactory;
        resource::Proxy<model::ModelResource>
            m_model; // the one cooked model, shared by every instance
        foundation::Array<foundation::RefPtr<materials::Material>>
            m_modelMats; // its materials (indexed by submesh material index)
        foundation::Array<animation::AnimationClip*> m_clips; // clips for random per-instance selection
        foundation::f32 m_fit = 1.0f;                         // auto-fit scale

        // The crowd: ONE entity carrying an InstancedMeshComponent (the skinned mesh at N transforms) + an
        // InstancedSkinningComponent companion (M shared pose palettes). m_crowdCount tracks the instance count.
        // A skinned mesh part of the character + its material (+ global material index, for merged submeshes).
        struct Part
        {
            foundation::RefPtr<geometry::StaticMesh> mesh;
            foundation::RefPtr<materials::Material> mat;
            foundation::i32 matIdx = -1;
        };
        scene::EntityHandle m_keyLight{}; // directional CSM light (K toggles its shadows)
        foundation::Array<Part>
            m_skinnedParts; // every skinned mesh of the model (shared across the crowd)
        foundation::Array<scene::EntityHandle>
            m_crowdParts; // all per-clip-group set + anim entities, torn down together
        foundation::u32 m_crowdCount = 0;
        foundation::u32 m_clipGroups = 1;
        bool m_tintEnabled = true; // per-instance/per-clip tint (HUD toggle)
        bool m_mergeMeshes =
            false; // merge the character's skinned parts into one mesh (HUD toggle)
        PosePolicy m_posePolicy =
            PosePolicy::Random;    // how each character picks its shared pose (HUD)
        bool m_singleClip = false; // collapse to 1 clip/pool so pose modes read cleanly (HUD)
        foundation::RefPtr<geometry::StaticMesh>
            m_mergedMesh; // cached merged mesh (one submesh per original part)
        foundation::Random m_rng{0x9e3779b97f4a7c15ull};
        static constexpr foundation::u32 kPoseCount = 32; // M unique phase buckets per shared pose pool
        static constexpr foundation::u32 kMaxClipGroups =
            6; // cap on distinct clips the crowd mixes across
        foundation::f32 m_frameTimeMs = 16.6f;
        bool m_showHud = true; // HUD visibility (H)

        // Measurement aid (off by default): auto-ramp the character count until FPS <= 50, then
        // report + exit. Flip to true for a headless throughput baseline; normal use is interactive.
        static constexpr bool kAutoRamp = false;
        foundation::f32 m_rampSettle = 0.0f;
        // Measurement aid (off by default): hold a fixed count, then dump the CPU+GPU profiler
        // breakdown and exit. Flip true to re-capture the baseline frame breakdown headless.
        static constexpr bool kAutoProfile = false;
        static constexpr foundation::u32 kProfileCount = 1000;
        foundation::f32 m_profileElapsed = 0.0f;
    };
}

int main(int, char**)
{
    auto shell = shell::CreateShell();
    graphics::GraphicsDeviceDesc gpuDesc{};
    auto gpu = graphics::CreateGraphicsDevice(gpuDesc);
    graphics::GraphicsDevice* device = gpu.HasValue() ? gpu.Value().Get() : nullptr;

    AnimatedCrowdApp app;
    return runtime::RunApplication(app, *shell, device);
}
