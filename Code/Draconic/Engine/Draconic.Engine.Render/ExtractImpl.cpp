/// Draconic::RenderSubsystem - the `:extract` partition.
///
/// Extraction: read a Scene's render components into a render::ExtractedScene (world-space
/// RenderData) + a render::ViewCamera, both pushed to the (scene-agnostic) renderer. This is
/// the one-way seam - this layer depends on both draconic.scene and draconic.render; the renderer
/// depends on neither. Run after the scene's transforms are current (the tick).
///
/// These are the providers in the design's terms (§5): a MeshComponent provider and the
/// camera reader. As more component types land (lights, probes), each gets its own provider
/// writing its own RenderData category into the snapshot.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.engine.render;

import draconic.foundation;
import draconic.scene;
import draconic.render;    // ExtractedScene / MeshRenderData / ViewCamera / categories
import draconic.materials; // BlendMode (category mapping)
import draconic.geometry;  // StaticMesh::bounds (world bounding sphere for shadow-caster culling)
import draconic.texture.resource; // texture::Texture (cooked product behind sprite/decal refs)
import :components;

using namespace draconic::foundation;

namespace draconic::render
{
    u64 PackEntity(scene::EntityHandle e) noexcept
    {
        return (static_cast<u64>(e.generation) << 32) | static_cast<u64>(e.index);
    }

    RenderCategory CategoryForMaterial(const materials::Material* m) noexcept
    {
        if (m == nullptr)
        {
            return RenderCategories::Opaque;
        }
        switch (m->pipeline.blendMode)
        {
        case materials::BlendMode::Opaque:
            return RenderCategories::Opaque;
        case materials::BlendMode::Masked:
            return RenderCategories::Masked;
        default:
            return RenderCategories::Transparent;
        }
    }

    f32 WorldBoundsRadius(const AABB& local, const Float4x4& world)
    {
        const f32 sx = Length(Float3{world.m[0][0], world.m[0][1], world.m[0][2]});
        const f32 sy = Length(Float3{world.m[1][0], world.m[1][1], world.m[1][2]});
        const f32 sz = Length(Float3{world.m[2][0], world.m[2][1], world.m[2][2]});
        return Length(local.Extents()) * Max(sx, Max(sy, sz));
    }

    void FillMeshRenderData(scene::Scene& scene, MeshComponent& mc, scene::EntityHandle e,
                            MeshRenderData& rd)
    {
        // Refresh the raw material cache from the ref proxies EVERY frame: a few pointer loads,
        // and late cooks / hot reloads heal live instead of pinning resolve-time nulls.
        mc.materialCache.Resize(mc.materials.Size());
        for (usize i = 0; i < mc.materials.Size(); ++i)
        {
            mc.materialCache[i] = RefPtr<materials::Material>(mc.materials[i].Get());
        }
        materials::Material* primary =
            mc.materialCache.IsEmpty() ? nullptr : mc.materialCache[0].Get();

        rd.world = scene.GetWorldMatrix(e);
        const AABB lb =
            (mc.mesh.Get() != nullptr) ? mc.mesh->bounds : AABB{Float3{0, 0, 0}, Float3{0, 0, 0}};
        rd.worldCenter = TransformPoint(lb.Center(), rd.world); // bounds center (cull + depth sort)
        rd.worldRadius = WorldBoundsRadius(lb, rd.world);
        rd.color = mc.color;
        rd.mesh = mc.mesh.Get();
        rd.material = primary;
        rd.entityId = PackEntity(e);
        rd.category = CategoryForMaterial(primary);
        // Batch-cluster key for the sort (opaque draws stay contiguous by mesh+material). rendererId keeps
        // its default 0 - the MeshRenderer is the first-registered renderer, so mesh data routes to it.
        rd.sortBatchKey = BatchKey(mc.mesh.Get(), primary);
        rd.boneMatrices = mc.boneMatrices; // borrowed for the frame (GPU skinning); null => static
        rd.prevBoneMatrices = mc.prevBoneMatrices; // borrowed; null => reuse current (no motion)
        rd.boneCount = mc.boneCount;
        // Submesh routing only when the mesh is genuinely multi-material; a single entry is the
        // whole-mesh path (slot 0 IS rd.material), preserving batching.
        rd.submeshMaterials = mc.materialCache.Size() > 1 ? mc.materialCache.Data() : nullptr;
        rd.submeshMaterialCount =
            mc.materialCache.Size() > 1 ? static_cast<u32>(mc.materialCache.Size()) : 0u;
    }

    void ExtractSceneInto(scene::Scene& scene, ExtractedScene& out)
    {
        if (auto* meshes = scene.GetSystem<MeshComponentManager>())
        {
            meshes->ForEach(
                [&](MeshComponent& mc, scene::EntityHandle e)
                {
                    if (!mc.visible || mc.mesh.Get() == nullptr)
                    {
                        return;
                    }
                    if (MeshRenderData* rd = out.Add<MeshRenderData>())
                    {
                        FillMeshRenderData(scene, mc, e, *rd);
                    }
                });
        }
    }

    void ExtractSceneInto(scene::Scene& scene, ExtractedScene& out, RenderContext& ctx)
    {
        out.Reset();
        auto* meshes = scene.GetSystem<MeshComponentManager>();
        if (meshes == nullptr)
        {
            return;
        }
        const u32 count = meshes->Count();
        if (count == 0)
        {
            return;
        }

        ctx.ResetItems();
        const Span<MeshComponent> comps = meshes->Dense();
        const Span<const scene::EntityHandle> owners = meshes->Owners();

        const bool parallel = HasGlobalJobSystem() && count >= kParallelExtractThreshold;
        if (parallel)
        {
            JobSystem& jobs = GlobalJobs();
            jobs.ParallelFor(count,
                             [&](u32 i)
                             {
                                 // Non-const: the fill refreshes the component's per-frame material cache.
                                 // Safe under ParallelFor - each component is touched by exactly one job.
                                 MeshComponent& mc = const_cast<MeshComponent&>(comps[i]);
                                 if (!mc.visible || mc.mesh.Get() == nullptr)
                                 {
                                     return;
                                 }
                                 const u32 slot = jobs.CurrentSlot();
                                 MeshRenderData* rd = ctx.Arena(slot).New<MeshRenderData>();
                                 if (rd == nullptr)
                                 {
                                     return;
                                 }
                                 FillMeshRenderData(scene, mc, owners[i], *rd);
                                 ctx.Items(slot).PushBack(static_cast<RenderData*>(rd));
                             });
        }
        else
        {
            FrameArena& arena = ctx.Arena(0);
            Array<RenderData*>& items = ctx.Items(0);
            for (u32 i = 0; i < count; ++i)
            {
                MeshComponent& mc = const_cast<MeshComponent&>(comps[i]);
                if (!mc.visible || mc.mesh.Get() == nullptr)
                {
                    continue;
                }
                MeshRenderData* rd = arena.New<MeshRenderData>();
                if (rd == nullptr)
                {
                    continue;
                }
                FillMeshRenderData(scene, mc, owners[i], *rd);
                items.PushBack(static_cast<RenderData*>(rd));
            }
        }
        ctx.MergeInto(out);
    }

    void ExtractInstancedMeshesInto(scene::Scene& scene, ExtractedScene& out)
    {
        auto* mgr = scene.GetSystem<InstancedMeshComponentManager>();
        if (mgr == nullptr)
        {
            return;
        }
        mgr->ForEach(
            [&](InstancedMeshComponent& c, scene::EntityHandle e)
            {
                if (!c.visible || c.mesh.Get() == nullptr || c.Count() == 0)
                {
                    return;
                }

                // Compose entity-relative instances into world space, cached: rebuilt only when the
                // authored set changed (version) OR the entity moved (world matrix compare). The renderer
                // keys uploads on composedVersion, so both kinds of change re-upload the GPU buffer.
                const Float4x4 entityWorld = scene.GetWorldMatrix(e);
                if (c.composedFromVersion != c.version || !(c.composedEntityWorld == entityWorld))
                {
                    c.worldTransforms.Resize(c.instances.Size());
                    for (usize i = 0; i < c.instances.Size(); ++i)
                    {
                        c.worldTransforms[i] = c.instances[i] * entityWorld;
                    }
                    c.composedEntityWorld = entityWorld;
                    c.composedFromVersion = c.version;
                    ++c.composedVersion;
                }

                // Merged bounds: union of the mesh's local AABB transformed by every composed instance.
                // Cached on the component and only rebuilt when the composed set changed.
                if (c.boundsVersion != c.composedVersion)
                {
                    const AABB lb = c.mesh->bounds;
                    AABB acc = AABB::Empty();
                    for (const Float4x4& xf : c.worldTransforms)
                    {
                        const Float3 ctr = TransformPoint(lb.Center(), xf);
                        const f32 r = WorldBoundsRadius(lb, xf);
                        acc.Expand(ctr - Float3{r, r, r});
                        acc.Expand(ctr + Float3{r, r, r});
                    }
                    c.cachedCenter = acc.Center();
                    c.cachedRadius = Length(acc.Extents());
                    c.boundsVersion = c.composedVersion;
                }

                MultiMeshRenderData* rd = out.Add<MultiMeshRenderData>();
                if (rd == nullptr)
                {
                    return;
                }
                rd->multiMesh = true;
                rd->key = PackEntity(e);
                rd->transforms =
                    c.worldTransforms.Data(); // borrowed for the frame (immutable snapshot)
                rd->tints = (!c.tints.IsEmpty() && c.tints.Size() == c.instances.Size())
                                ? c.tints.Data()
                                : nullptr;
                rd->instanceCount = c.Count();
                rd->version = c.composedVersion;
                rd->mesh = c.mesh.Get();
                rd->material = c.material.Get();
                rd->submeshMaterials =
                    c.submeshMaterials.IsEmpty() ? nullptr : c.submeshMaterials.Data();
                rd->submeshMaterialCount = static_cast<u32>(c.submeshMaterials.Size());
                rd->color = c.color;
                rd->worldCenter = c.cachedCenter; // merged bounds -> single-AABB cull + depth sort
                rd->worldRadius = c.cachedRadius;
                rd->entityId = PackEntity(e);
                rd->category = CategoryForMaterial(c.material.Get());
                rd->sortBatchKey = BatchKey(c.mesh.Get(), c.material.Get());
                rd->posePool = c.posePool; // skinned crowds: shared pose pool (null => static)
                rd->prevPosePool = c.prevPosePool;
                rd->poseCount = c.poseCount;
                rd->boneCount = c.boneCount;
                rd->poseAssignment = c.poseAssignment;
                // Only borrow the explicit index array when the policy asks for it AND it's correctly sized;
                // otherwise leave it null so the renderer falls back to the Hashed default.
                rd->poseIndices = (c.poseAssignment == PoseAssignment::Explicit &&
                                   c.poseIndices.Size() == c.instances.Size())
                                      ? c.poseIndices.Data()
                                      : nullptr;
                // rendererId stays 0 (the MeshRenderer draws it); `world` stays identity (per-instance
                // transforms ride in `transforms`, uploaded to the renderer's persistent buffer).
            });
    }

    void ExtractSpritesInto(scene::Scene& scene, ExtractedScene& out, u16 spriteRendererId)
    {
        auto* sprites = scene.GetSystem<SpriteComponentManager>();
        if (sprites == nullptr)
        {
            return;
        }
        sprites->ForEach(
            [&](SpriteComponent& sc, scene::EntityHandle e)
            {
                if (!sc.visible)
                {
                    return;
                }
                // Runtime view override wins; otherwise the cooked texture product behind the ref.
                rhi::TextureView* view = sc.texture;
                if (view == nullptr)
                {
                    if (texture::Texture* t = sc.textureAsset.Get())
                    {
                        view = t->View();
                    }
                }
                if (view == nullptr)
                {
                    return;
                }
                SpriteRenderData* rd = out.Add<SpriteRenderData>();
                if (rd == nullptr)
                {
                    return;
                }
                rd->category =
                    sc.postTonemap ? RenderCategories::WorldUI : RenderCategories::Transparent;
                rd->rendererId = spriteRendererId;
                rd->worldCenter = TransformPoint(Float3{0, 0, 0}, scene.GetWorldMatrix(e));
                // Bounding-sphere radius for view-frustum culling: half the billboard's diagonal. size is in
                // world units (the sprite renderer sizes the quad directly), so entity scale isn't folded in.
                rd->worldRadius = 0.5f * Length(sc.size);
                rd->size = sc.size;
                rd->uvRect = sc.uvRect;
                rd->tint = sc.tint;
                rd->orientation = static_cast<u32>(sc.orientation);
                rd->additive = sc.additive;
                rd->postTonemap = sc.postTonemap;
                if (sc.orientation == SpriteOrientation::EntityOriented)
                {
                    // The entity's world right/up span the quad (normalized: `size` alone sets
                    // the extent, matching every other orientation's contract).
                    const Float4x4 world = scene.GetWorldMatrix(e);
                    rd->axisRight = Normalized(Float3{world.m[0][0], world.m[0][1], world.m[0][2]});
                    rd->axisUp = Normalized(Float3{world.m[1][0], world.m[1][1], world.m[1][2]});
                }
                rd->texture = view;
            });
    }

    void ExtractDecalsInto(scene::Scene& scene, ExtractedScene& out)
    {
        auto* decals = scene.GetSystem<DecalComponentManager>();
        if (decals == nullptr)
        {
            return;
        }
        decals->ForEach(
            [&](DecalComponent& dc, scene::EntityHandle e)
            {
                if (!dc.visible)
                {
                    return;
                }
                rhi::TextureView* view = dc.texture;
                if (view == nullptr)
                {
                    if (texture::Texture* t = dc.textureAsset.Get())
                    {
                        view = t->View();
                    }
                }
                if (view == nullptr)
                {
                    return;
                }
                DecalInstance di;
                di.world = Float4x4::Scale(dc.size) * scene.GetWorldMatrix(e);
                di.color = dc.color;
                di.fadeStart = dc.fadeStart;
                di.fadeEnd = dc.fadeEnd;
                di.texture = view;
                out.AddDecal(di);
            });
    }

    bool ExtractPrimaryCamera(scene::Scene& scene, ViewCamera& out, Color* outClear)
    {
        bool found = false;
        if (auto* cameras = scene.GetSystem<CameraComponentManager>())
        {
            cameras->ForEach(
                [&](CameraComponent& cam, scene::EntityHandle e)
                {
                    if (found || !cam.primary)
                    {
                        return;
                    }
                    found = true;
                    const Float4x4 world = scene.GetWorldMatrix(e);
                    out.view = Inverse(world);
                    out.projection = Float4x4::PerspectiveFovRH(cam.fovYRadians, cam.aspect,
                                                                cam.nearZ, cam.farZ);
                    out.position = TransformPoint(Float3{0, 0, 0}, world);
                    out.farZ = cam.farZ;
                    if (outClear != nullptr)
                    {
                        *outClear = cam.clearColor;
                    }
                });
        }
        return found;
    }

    void ExtractLightsInto(scene::Scene& scene, ExtractedScene& out)
    {
        auto* lights = scene.GetSystem<LightComponentManager>();
        if (lights == nullptr)
        {
            return;
        }
        bool haveShadow = false;
        u32 rtTiles = 0,
            stTiles = 0;     // tiles used per atlas layer (realtime / static); capped separately
        u32 flatEntries = 0; // running GpuLocalShadow entry index = the next caster's shadowIndex
        lights->ForEach(
            [&](LightComponent& lc, scene::EntityHandle e)
            {
                if (!lc.enabled)
                {
                    return;
                }
                const Float4x4 world = scene.GetWorldMatrix(e);
                GpuLight g;
                g.positionWS = TransformPoint(Float3{0, 0, 0}, world);
                // Forward is -Z (row 2 negated) in world space (row-major, row-vector convention).
                g.directionWS = Normalized(Float3{-world.m[2][0], -world.m[2][1], -world.m[2][2]});
                g.range = lc.range;
                g.color = Float3{lc.color.r, lc.color.g, lc.color.b};
                g.intensity = lc.intensity;
                g.type = static_cast<f32>(static_cast<u32>(lc.type));
                g.innerCos = Cos(lc.innerAngle);
                g.outerCos = Cos(lc.outerAngle);
                if (!haveShadow && lc.castsShadows && lc.type == LightType::Directional)
                {
                    haveShadow = true;
                    g.shadowIndex = 0.0f; // marks this light as shadowed in the forward shader
                    DirectionalShadow ds;
                    ds.direction = g.directionWS;
                    ds.valid = true;
                    out.SetDirectionalShadow(ds);
                }
                // Local (spot/point) shadow casters (5.3): assign shadowIndex = the caster's BASE atlas tile
                // here, then register it; the ShadowSystem builds the perspective matrix/matrices at frame
                // time. A spot uses 1 tile, a point 6 (cube faces). Capped at the atlas tile budget.
                const bool localCaster =
                    lc.castsShadows && (lc.type == LightType::Spot || lc.type == LightType::Point);
                const u32 tilesNeeded = (lc.type == LightType::Point) ? 6u : 1u;
                const bool isStatic = (lc.shadowUpdate == ShadowUpdateMode::Static);
                u32& layerTiles =
                    isStatic ? stTiles : rtTiles; // each atlas layer has its own budget
                if (localCaster && layerTiles + tilesNeeded <= kMaxLocalShadowTiles)
                {
                    g.shadowIndex =
                        static_cast<f32>(flatEntries); // base entry into the GpuLocalShadow buffer
                    LocalShadowCaster c;
                    c.type = static_cast<u32>(lc.type);
                    c.positionWS = g.positionWS;
                    c.directionWS = g.directionWS;
                    c.range = lc.range;
                    c.outerAngle = lc.outerAngle;
                    c.isStatic = isStatic;
                    out.AddLocalShadowCaster(c);
                    layerTiles += tilesNeeded;
                    flatEntries += tilesNeeded;
                }
                out.AddLight(g);
            });
    }

    void ExtractEnvironmentInto(scene::Scene& scene, ExtractedScene& out)
    {
        if (auto* env = scene.GetSystem<EnvironmentSystem>())
        {
            const EnvironmentSettings& e = env->Environment();
            out.SetAmbient(Float3{e.ambientColor.r, e.ambientColor.g, e.ambientColor.b} *
                           e.ambientIntensity);
            SkySnapshot s{};
            s.mode = e.skyMode;
            s.intensity = e.skyIntensity;
            s.backgroundIntensity = e.skyBackgroundIntensity;
            s.rotation = e.skyRotation;
            s.horizon = Float3{e.skyHorizon.r, e.skyHorizon.g, e.skyHorizon.b};
            s.zenith = Float3{e.skyZenith.r, e.skyZenith.g, e.skyZenith.b};
            s.ground = Float3{e.skyGround.r, e.skyGround.g, e.skyGround.b};
            s.sunIntensity = e.sunIntensity;
            s.sunAngularSize = e.sunAngularSize;
            s.turbidity = e.turbidity;
            // Resolved sky-texture product (textured modes). Uid = change detection; the IBL
            // rebuilds its env products when it swaps (pick, hot-reload).
            if (texture::Texture* skyTex = e.skyTexture.Get())
            {
                s.texture = skyTex->View();
                s.textureUid = skyTex->Uid();
                s.textureIsCube = skyTex->IsCube();
            }
            out.SetSky(s);
        }
    }

    void ExtractReflectionProbesInto(scene::Scene& scene, ExtractedScene& out)
    {
        auto* probes = scene.GetSystem<ReflectionProbeComponentManager>();
        if (probes == nullptr)
        {
            return;
        }
        u32 count = 0;
        probes->ForEach(
            [&](ReflectionProbeComponent& pc, scene::EntityHandle e)
            {
                if (!pc.enabled || count >= kMaxReflectionProbes)
                {
                    return;
                }
                const Float4x4 world = scene.GetWorldMatrix(e);
                ReflectionProbe p;
                p.key = PackEntity(e);
                p.center = TransformPoint(Float3{0, 0, 0}, world);
                p.halfExtents = pc.halfExtents;
                p.blendDistance = pc.blendDistance;
                p.intensity = pc.intensity;
                p.resolution = pc.resolution;
                p.priority = pc.priority;
                p.update = pc.update;
                p.parallax = pc.parallax;
                out.AddReflectionProbe(p);
                ++count;
            });
    }
}
