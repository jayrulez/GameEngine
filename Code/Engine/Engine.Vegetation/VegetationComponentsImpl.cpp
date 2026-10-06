// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.vegetation implementation: the manager's cache + extraction, reflection and the
// composition install (heavy bodies kept out of the interface; see gcc-module-interface-hygiene).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

module engine.vegetation;
import engine.domain;

import foundation.core;
import foundation.profiler;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.heightfield;
import foundation.terrain;
import foundation.terrain.resource;
import foundation.vegetation.resource;
import foundation.vegetation;
import foundation.render;
import engine.render;  // PackEntity, CategoryForMaterial (the instanced-mesh emitter's helpers)
import engine.terrain; // TerrainComponentManager (the terrain the layers grow on)

using namespace foundation::core;

namespace engine::vegetation
{
    namespace
    {
        // The terrain a vegetation entity grows on: its own TerrainComponent, else the nearest
        // ancestor's. Returns null (and leaves `terrainEntity` unassigned) when there is none.
        engine::terrain::TerrainComponent* FindTerrainFor(scene::Scene& scene,
                                                          scene::EntityHandle owner,
                                                          scene::EntityHandle& terrainEntity)
        {
            auto* terrains = scene.GetSystem<engine::terrain::TerrainComponentManager>();
            if (terrains == nullptr)
            {
                return nullptr;
            }
            scene::EntityHandle e = owner;
            for (u32 depth = 0; depth < 64 && e.IsAssigned(); ++depth)
            {
                if (engine::terrain::TerrainComponent* tc = terrains->Get(e))
                {
                    terrainEntity = e;
                    return tc;
                }
                e = scene.GetParent(e);
            }
            return nullptr;
        }

        // The largest axis scale a matrix applies (a bounds radius scales by at most this).
        f32 MaxAxisScale(const Float4x4& m) noexcept
        {
            const f32 sx = Length(Float3{m.m[0][0], m.m[0][1], m.m[0][2]});
            const f32 sy = Length(Float3{m.m[1][0], m.m[1][1], m.m[1][2]});
            const f32 sz = Length(Float3{m.m[2][0], m.m[2][1], m.m[2][2]});
            return Max(sx, Max(sy, sz));
        }
    }

    // ---- reflection --------------------------------------------------------------------------

    REFLECT_ENUM(VegetationPlacement, "rtti::foundation::vegetation")
    {
        builder.Value("Uniform", VegetationPlacement::Uniform);
        builder.Value("Splat", VegetationPlacement::Splat);
        builder.Value("Mask", VegetationPlacement::Mask);
        builder.Value("SplatTimesMask", VegetationPlacement::SplatTimesMask);
    }

    // The base fields, declared on each layer type (the inspector lists a type's own
    // properties; the two lists share the rows through this helper, not through the RTTI base).
    template <typename Layer>
    void ReflectLayerBase(TypeBuilder<Layer>& builder)
    {
        builder.template Property<&Layer::name>("name")
            .PropAttribute("description", String(u8"The layer's label in the list."))
            .template Property<&Layer::mesh>("mesh")
            .PropAttribute("description",
                           String(u8"The instanced mesh: a grass card, a tuft, a rock."))
            .template Property<&Layer::materials>("materials")
            .PropAttribute("description",
                           String(u8"One per mesh slot, as on a mesh: one covers the whole mesh."))
            .template Property<&Layer::scaleRange>("scaleRange")
            .PropAttribute("displayName", String(u8"Scale Range"))
            .template Property<&Layer::maxSlopeDegrees>("maxSlopeDegrees")
            .PropAttribute("displayName", String(u8"Max Slope"))
            .PropAttribute("description",
                           String(u8"Degrees from flat above which nothing grows or stands."))
            .template Property<&Layer::heightRange>("heightRange")
            .PropAttribute("displayName", String(u8"Height Range"))
            .PropAttribute("description", String(u8"Terrain-local Y window the layer lives in."))
            .template Property<&Layer::alignToNormal>("alignToNormal")
            .PropAttribute("displayName", String(u8"Align To Normal"))
            .template Property<&Layer::fadeStart>("fadeStart")
            .PropAttribute("displayName", String(u8"Fade Start"))
            .PropAttribute("description", String(u8"Metres from the camera: full density inside."))
            .template Property<&Layer::fadeEnd>("fadeEnd")
            .PropAttribute("displayName", String(u8"Fade End"))
            .PropAttribute("description", String(u8"Metres from the camera: nothing beyond."))
            .template Property<&Layer::castShadows>("castShadows")
            .PropAttribute("displayName", String(u8"Cast Shadows"))
            .PropAttribute("description",
                           String(u8"Off for grass (the single most expensive thing a grass layer "
                                  u8"can do); on for rocks and props."))
            .template Property<&Layer::maxInstancesPerChunk>("maxInstancesPerChunk")
            .PropAttribute("displayName", String(u8"Max Per Chunk"))
            .PropAttribute("description",
                           String(u8"Instances one 64 x 64 quad terrain chunk holds; a chunk that "
                                  u8"fills stops placing and logs once."))
            .template Property<&Layer::visible>("visible")
            .template Property<&Layer::collisionRadius>("collisionRadius")
            .PropAttribute("displayName", String(u8"Collision Radius"))
            .PropAttribute("description",
                           String(u8"A solid trunk this radius round (metres, scaled with each "
                                  u8"instance); 0 is scenery only."))
            .template Property<&Layer::collisionHeight>("collisionHeight")
            .PropAttribute("displayName", String(u8"Collision Height"))
            .PropAttribute("description",
                           String(u8"The trunk's height from its foot to its top (metres, scaled "
                                  u8"with each instance)."))
            .template Property<&Layer::collisionGroup>("collisionGroup")
            .PropAttribute("displayName", String(u8"Collision Group"))
            .PropAttribute("description",
                           String(u8"The physics collision group of the trunks (0 to 31): the scene's "
                                  u8"group matrix decides what they stop, and a query finds them by it."));
    }

    REFLECT_VALUE(ProceduralVegetationLayer, "rtti::engine::vegetation")
    {
        builder.Attribute("displayName", String(u8"Procedural Layer"));
        ReflectLayerBase(builder);
        builder.Property<&ProceduralVegetationLayer::placement>("placement")
            .PropAttribute("description",
                           String(u8"Where it grows: everywhere (Uniform), where a terrain splat "
                                  u8"layer is painted (Splat), where a plane of the component's "
                                  u8"mask is painted (Mask - the default; paint it with Paint "
                                  u8"Vegetation), or both (SplatTimesMask)."))
            .Property<&ProceduralVegetationLayer::splatLayer>("splatLayer")
            .PropAttribute("displayName", String(u8"Splat Layer"))
            .PropAttribute("description",
                           String(u8"Splat placement: the terrain palette index to follow."))
            .Property<&ProceduralVegetationLayer::splatThreshold>("splatThreshold")
            .PropAttribute("displayName", String(u8"Splat Threshold"))
            .PropAttribute("description",
                           String(u8"Splat placement: the painted share (0..1) below which "
                                  u8"nothing grows."))
            .Property<&ProceduralVegetationLayer::maskPlane>("maskPlane")
            .PropAttribute("displayName", String(u8"Mask Plane"))
            .PropAttribute("description",
                           String(u8"Mask placement: the plane of the component's mask this "
                                  u8"layer follows (the Paint Vegetation brush paints it)."))
            .Property<&ProceduralVegetationLayer::density>("density")
            .PropAttribute("description", String(u8"Instances per square metre."));
    }

    REFLECT_VALUE(PropVegetationLayer, "rtti::engine::vegetation")
    {
        builder.Attribute("displayName", String(u8"Prop Layer"));
        ReflectLayerBase(builder);
        // `instances` is deliberately not a row: the Paint Props brush is its editor.
    }

    REFLECT_VALUE(TerrainVegetationComponent, "rtti::engine::vegetation")
    {
        builder.Attribute("displayName", String(u8"Terrain Vegetation"))
            .Attribute("category", String(u8"Terrain"))
            .DataVersion(4)          // 2026-10-06: a layer's collision (trunk radius, height, group);
                                     // 2026-10-05: a material per slot (was one material);
                                     // 2026-09-23: two layer lists (was one list + placement)
            .ReadsDataVersionsFrom(1) // the legacy reader splits a V1 list (remove after re-saves)
            .Property<&TerrainVegetationComponent::proceduralLayers>("proceduralLayers")
            .PropAttribute("displayName", String(u8"Procedural Layers"))
            .PropAttribute("description",
                           String(u8"Layers the scatter grows from a source: grass from a splat "
                                  u8"layer, flowers from a painted mask plane. Each slot edits "
                                  u8"below the list."))
            .Property<&TerrainVegetationComponent::propLayers>("propLayers")
            .PropAttribute("displayName", String(u8"Prop Layers"))
            .PropAttribute("description",
                           String(u8"Layers of placed props: rocks, stumps. Paint them with the "
                                  u8"Paint Props brush; each slot edits its rules below the list."))
            .Property<&TerrainVegetationComponent::mask>("mask")
            .PropAttribute("description",
                           String(u8"The painted vegetation mask (one density plane per "
                                  u8"procedural layer that uses Mask placement); paint it with "
                                  u8"the Paint Vegetation brush."))
            .Property<&TerrainVegetationComponent::visible>("visible");
    }

    void AddVegetationSceneManagers(foundation::scene::Scene& scene)
    {
        scene.AddSystem<TerrainVegetationComponentManager>();
    }

    void RegisterVegetationComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_VegetationPlacement();
            RttiRegisterValue_ProceduralVegetationLayer();
            RttiRegisterValue_PropVegetationLayer();
            RegisterArrayType<ProceduralVegetationLayer>(); // the list containers (list editor + scripts)
            RegisterArrayType<PropVegetationLayer>();
            RttiRegisterValue_TerrainVegetationComponent();
            return true;
        }();
        (void)once;
    }

    // ---- the manager ---------------------------------------------------------------------------

    void TerrainVegetationComponentManager::InvalidateRegion(
        const heightfield::HeightfieldRegion& region)
    {
        if (!region.IsEmpty())
        {
            m_pendingRegions.PushBack(region);
        }
    }

    void TerrainVegetationComponentManager::InvalidateFootprint(f32 u0, f32 v0, f32 u1, f32 v1,
                                                                i32 gridSize)
    {
        if (gridSize <= 1 || u1 < u0 || v1 < v0)
        {
            return;
        }
        const f32 span = static_cast<f32>(gridSize - 1);
        heightfield::HeightfieldRegion region;
        region.minX = Clamp(static_cast<i32>(Floor(Clamp(u0, 0.0f, 1.0f) * span)), 0, gridSize - 1);
        region.maxX = Clamp(static_cast<i32>(Ceil(Clamp(u1, 0.0f, 1.0f) * span)), 0, gridSize - 1);
        region.minZ = Clamp(static_cast<i32>(Floor(Clamp(v0, 0.0f, 1.0f) * span)), 0, gridSize - 1);
        region.maxZ = Clamp(static_cast<i32>(Ceil(Clamp(v1, 0.0f, 1.0f) * span)), 0, gridSize - 1);
        InvalidateRegion(region);
    }

    usize TerrainVegetationComponentManager::BuiltSetCount() const noexcept
    {
        usize n = 0;
        for (const auto& entry : m_caches)
        {
            for (const ChunkSet& set : entry.value.sets)
            {
                n += (set.built && !set.world.IsEmpty()) ? 1 : 0;
            }
        }
        return n;
    }

    usize TerrainVegetationComponentManager::InstanceCount() const noexcept
    {
        usize n = 0;
        for (const auto& entry : m_caches)
        {
            for (const ChunkSet& set : entry.value.sets)
            {
                n += set.built ? set.world.Size() : 0;
            }
        }
        return n;
    }

    u64 TerrainVegetationComponentManager::CacheKey(scene::EntityHandle owner, u32 slot) noexcept
    {
        const u64 entity = engine::render::PackEntity(owner);
        return HashBytes(&slot, sizeof(slot), entity);
    }

    TerrainVegetationComponentManager::LayerCache& TerrainVegetationComponentManager::CacheFor(
        scene::EntityHandle owner, u32 slot)
    {
        const u64 key = CacheKey(owner, slot);
        if (LayerCache* found = m_caches.Find(key))
        {
            return *found;
        }
        LayerCache fresh;
        fresh.slot = slot;
        return m_caches.InsertOrAssign(key, Move(fresh));
    }

    // A slot's log name: "prop layer 1" / "procedural layer 0".
    static String SlotName(u32 slot)
    {
        const bool props = (slot & TerrainVegetationComponentManager::kPropSlotBit) != 0;
        return Format(u8"{} layer {}", props ? StringView(u8"prop") : StringView(u8"procedural"),
                      slot & ~TerrainVegetationComponentManager::kPropSlotBit);
    }

    void TerrainVegetationComponentManager::ResetCache(LayerCache& cache,
                                                       const heightfield::Heightfield& hf,
                                                       const Guid& ownerId, u32 slot)
    {
        cache.ownerId = ownerId;
        cache.slot = slot;
        cache.heightfieldUid = hf.uid;
        cache.heightfieldVersion = hf.Version();
        cache.splatUid = 0;
        cache.splatVersion = 0;
        cache.maskUid = 0;
        cache.maskVersion = 0;
        cache.composed = false;
        cache.chunks.Clear();
        tmodel::BuildChunks(hf, cache.chunks);
        cache.chunksPerSide = tmodel::ChunksPerSide(hf.Size());
        cache.sets.Clear();
        cache.sets.Resize(cache.chunks.Size());
        for (usize i = 0; i < cache.chunks.Size(); ++i)
        {
            cache.sets[i].key = veg::ChunkSeed(ownerId, slot, cache.chunks[i].chunkX,
                                               cache.chunks[i].chunkZ);
        }
    }

    void TerrainVegetationComponentManager::DirtyAll(LayerCache& cache)
    {
        for (ChunkSet& set : cache.sets)
        {
            set.dirty = true;
        }
    }

    void TerrainVegetationComponentManager::Compose(LayerCache& cache, ChunkSet& set)
    {
        set.world.Resize(set.local.Size());
        for (usize i = 0; i < set.local.Size(); ++i)
        {
            set.world[i] = set.local[i] * cache.entityWorld;
        }
        // World bounds: the local AABB's corners through the entity matrix.
        AABB world = AABB::Empty();
        const Float3 lo = set.localBounds.min;
        const Float3 hi = set.localBounds.max;
        for (u32 corner = 0; corner < 8; ++corner)
        {
            const Float3 p{(corner & 1u) ? hi.x : lo.x, (corner & 2u) ? hi.y : lo.y,
                           (corner & 4u) ? hi.z : lo.z};
            world.Expand(TransformPoint(p, cache.entityWorld));
        }
        set.worldCenter = world.Center();
        set.worldRadius = Length(world.Extents());
        ++set.version; // the renderer re-uploads the set's buffer on a version change
    }

    void TerrainVegetationComponentManager::BuildSet(LayerCache& cache, u32 chunkIndex,
                                                     const heightfield::Heightfield& hf,
                                                     const tmodel::SplatWeights* splat,
                                                     const veg::VegetationMask* mask,
                                                     const veg::ScatterLayer& layer,
                                                     const AABB& meshBounds, bool props,
                                                     Span<const Float4x4> authored)
    {
        ChunkSet& set = cache.sets[chunkIndex];
        veg::ScatterResult result;
        if (props)
        {
            // Authored props: the chunk's share of the layer's instances (each instance maps
            // to exactly one chunk by its terrain-local XZ), bounds grown like the scatter's.
            const tmodel::TerrainChunk& chunk = cache.chunks[chunkIndex];
            const tmodel::TerrainChunk& origin = cache.chunks[0];
            const f32 chunkWidth = Max(chunk.bounds.max.x - chunk.bounds.min.x, 1e-6f);
            const f32 chunkDepth = Max(chunk.bounds.max.z - chunk.bounds.min.z, 1e-6f);
            const i32 last = cache.chunksPerSide - 1;
            const bool holes = hf.HasHoles();
            for (const Float4x4& m : authored)
            {
                const i32 cx = Clamp(static_cast<i32>(Floor((m.m[3][0] - origin.bounds.min.x) / chunkWidth)), 0, last);
                const i32 cz = Clamp(static_cast<i32>(Floor((m.m[3][2] - origin.bounds.min.z) / chunkDepth)), 0, last);
                if (cx != chunk.chunkX || cz != chunk.chunkZ)
                {
                    continue;
                }
                // A prop standing over a cut cell has no surface under it: it stays in the
                // authored list (Fill brings it back, the eraser can reach it) but does not
                // draw - the one rule everywhere (Specs/terrain-holes.md), for props too.
                if (holes)
                {
                    i32 hx = 0;
                    i32 hz = 0;
                    hf.CellOfLocal(m.m[3][0], m.m[3][2], hx, hz);
                    if (hf.CellHasHole(hx, hz))
                    {
                        continue;
                    }
                }
                result.transforms.PushBack(m);
            }
            result.localBounds = chunk.bounds;
            if (!result.transforms.IsEmpty() && meshBounds.max.x >= meshBounds.min.x)
            {
                const f32 grow = (Length(meshBounds.Extents()) + Length(meshBounds.Center())) *
                                 Max(layer.scaleRange.x, layer.scaleRange.y);
                result.localBounds.min = result.localBounds.min - Float3{grow, grow, grow};
                result.localBounds.max = result.localBounds.max + Float3{grow, grow, grow};
            }
        }
        else
        {
            veg::ScatterChunk(set.key, cache.chunks[chunkIndex], hf, splat, mask, layer,
                              meshBounds, result);
        }
        if (result.densityClamped && !cache.warnedClamp)
        {
            cache.warnedClamp = true;
            LOG_WARNING(u8"Vegetation",
                        u8"{} over budget: {} instances/m2 wanted, {} used "
                        u8"(maxInstancesPerChunk = {})",
                        SlotName(cache.slot), layer.density, result.effectiveDensity,
                        layer.maxInstancesPerChunk);
        }
        // Build into a FRESH array and swap: the previous array may be borrowed by a snapshot
        // still being recorded (the instanced-mesh borrow rule).
        set.local = Move(result.transforms);
        set.localBounds = result.localBounds;
        set.built = true;
        set.dirty = false;
        Compose(cache, set);
        ++m_builds;
    }

    TerrainVegetationComponentManager::LayerCache* TerrainVegetationComponentManager::PrepareLayer(
        scene::EntityHandle owner, const Guid& ownerId, u32 slot, const VegetationLayerBase& base,
        const veg::ScatterLayer& layer, bool props, Span<const Float4x4> authored,
        const heightfield::Heightfield& hf, const tmodel::SplatWeights* splat,
        const veg::VegetationMask* mask, const Float4x4& entityWorld)
    {
        LayerCache& cache = CacheFor(owner, slot);
        foundation::geometry::StaticMesh* mesh = base.mesh.Get();
        if (mesh == nullptr)
        {
            if (!base.mesh.id.IsNil() && !cache.warnedNoMesh)
            {
                cache.warnedNoMesh = true;
                LOG_WARNING(u8"Vegetation",
                            u8"{} ('{}'): its mesh reference does not resolve (a deleted, "
                            u8"uncooked or stale asset) - nothing will draw",
                            SlotName(slot), base.name);
            }
            return nullptr;
        }
        cache.warnedNoMesh = false;
        const u64 layerHash = veg::LayerScatterHash(layer);

        // Identity changes rebuild the whole cache: another heightfield (or its size), the
        // owner's persistent id, the mesh, or any scatter parameter.
        if (cache.chunks.IsEmpty() || cache.heightfieldUid != hf.uid ||
            !(cache.ownerId == ownerId) || cache.meshUid != mesh->uid ||
            cache.layerHash != layerHash)
        {
            ResetCache(cache, hf, ownerId, slot);
            cache.layerHash = layerHash;
            cache.meshUid = mesh->uid;
            cache.warnedClamp = false;
        }
        // Content changes (a sculpt, a paint) regrow the touched chunks when the editor said
        // which (InvalidateRegion), else every chunk.
        const u64 splatUid = splat != nullptr ? splat->uid : 0;
        const u64 splatVersion = splat != nullptr ? splat->Version() : 0;
        const u64 maskUid = mask != nullptr ? mask->uid : 0;
        const u64 maskVersion = mask != nullptr ? mask->Version() : 0;
        // Props: the authored instances are the content; a brush stroke (or an undo) is a
        // hash change, and the whole layer re-buckets (props are few; a sculpt regrows too).
        u64 instancesHash = 0;
        if (props)
        {
            const usize count = authored.Size();
            instancesHash = HashBytes(&count, sizeof(count), 0x9E3779B97F4A7C15ull);
            if (count > 0)
            {
                instancesHash = HashBytes(authored.Data(), count * sizeof(Float4x4), instancesHash);
            }
        }
        if (cache.instancesHash != instancesHash)
        {
            DirtyAll(cache);
            cache.instancesHash = instancesHash;
        }
        if (cache.heightfieldVersion != hf.Version() || cache.splatUid != splatUid ||
            cache.splatVersion != splatVersion || cache.maskUid != maskUid ||
            cache.maskVersion != maskVersion)
        {
            if (m_pendingRegions.IsEmpty())
            {
                DirtyAll(cache);
            }
            else
            {
                Array<u32> touched;
                for (const heightfield::HeightfieldRegion& region : m_pendingRegions)
                {
                    veg::ChunksTouchedBy(region, cache.chunksPerSide, touched);
                }
                for (u32 index : touched)
                {
                    if (index < cache.sets.Size())
                    {
                        cache.sets[index].dirty = true;
                    }
                }
            }
            cache.heightfieldVersion = hf.Version();
            cache.splatUid = splatUid;
            cache.splatVersion = splatVersion;
            cache.maskUid = maskUid;
            cache.maskVersion = maskVersion;
        }
        // The terrain entity's world matrix places the terrain-local instances; a move
        // recomposes the built sets (no rescatter).
        if (!cache.composed || !(cache.entityWorld == entityWorld))
        {
            cache.entityWorld = entityWorld;
            cache.composed = true;
            for (ChunkSet& set : cache.sets)
            {
                if (set.built)
                {
                    Compose(cache, set);
                }
            }
        }

        return &cache;
    }

    void TerrainVegetationComponentManager::ExtractLayer(
        render::ExtractedScene& snapshot, scene::EntityHandle owner, const Guid& ownerId,
        u32 slot, const VegetationLayerBase& base, const veg::ScatterLayer& layer, bool props,
        Span<const Float4x4> authored, const heightfield::Heightfield& hf,
        const tmodel::SplatWeights* splat, const veg::VegetationMask* mask,
        const Float4x4& entityWorld, u32& budget)
    {
        LayerCache& seen = CacheFor(owner, slot);
        seen.seenThisFrame = true; // a hidden layer keeps its sets (unhide = no regrow)
        if (!base.visible)
        {
            return;
        }
        LayerCache* prepared = PrepareLayer(owner, ownerId, slot, base, layer, props, authored, hf, splat,
                                            mask, entityWorld);
        if (prepared == nullptr)
        {
            return;
        }
        LayerCache& cache = *prepared;
        foundation::geometry::StaticMesh* mesh = base.mesh.Get();

        const bool hasOrigin = snapshot.HasViewOrigin();
        const Float3 origin = snapshot.ViewOrigin();
        const f32 entityScale = MaxAxisScale(entityWorld);
        cache.materials.Resize(base.materials.Size());
        for (usize m = 0; m < base.materials.Size(); ++m)
        {
            cache.materials[m] = RefPtr<foundation::materials::Material>(base.materials[m].Get());
        }
        foundation::materials::Material* material =
            cache.materials.IsEmpty() ? nullptr : cache.materials[0].Get();
        // Submesh routing only for a genuinely multi-material list (a mesh component's rule): one
        // entry is the whole-mesh path, which keeps batching.
        const bool perSlot = cache.materials.Size() > 1;
        for (u32 i = 0; i < cache.sets.Size(); ++i)
        {
            ChunkSet& set = cache.sets[i];
            // Distance from the view to the chunk (its built bounds, else the terrain's).
            f32 distance = 0.0f;
            if (hasOrigin)
            {
                Float3 center = set.worldCenter;
                f32 radius = set.worldRadius;
                if (!set.built)
                {
                    const tmodel::TerrainChunk& chunk = cache.chunks[i];
                    center = TransformPoint(chunk.bounds.Center(), entityWorld);
                    radius = Length(chunk.bounds.Extents()) * entityScale;
                }
                distance = Max(0.0f, Length(origin - center) - radius);
                if (distance >= layer.fadeEnd)
                {
                    continue; // out of range: absent from the snapshot (renderer evicts)
                }
            }
            if (set.dirty)
            {
                // Authored props re-bucket in one copy - outside the budget, which exists for the
                // procedural scatter; a stroke lands whole. A procedural set past the budget
                // waits for a later frame, but a set that was ALREADY built keeps drawing its
                // previous instances meanwhile (a dropped frame under the brush is a flicker).
                if (props || budget > 0)
                {
                    BuildSet(cache, i, hf, splat, mask, layer, mesh->bounds, props, authored);
                    if (!props)
                    {
                        --budget;
                    }
                }
                else if (!set.built)
                {
                    continue; // never built: nothing stale to show until its turn
                }
            }
            if (set.world.IsEmpty())
            {
                continue;
            }
            const f32 density =
                hasOrigin ? veg::DensityAtDistance(distance, layer.fadeStart, layer.fadeEnd) : 1.0f;
            const u32 count = veg::FadePrefix(static_cast<u32>(set.world.Size()), density);
            if (count == 0)
            {
                continue;
            }

            render::MultiMeshRenderData* rd = snapshot.Add<render::MultiMeshRenderData>();
            if (rd == nullptr)
            {
                return;
            }
            rd->multiMesh = true;
            rd->key = set.key;
            rd->transforms = set.world.Data(); // borrowed for the frame (immutable snapshot)
            rd->instanceCount = count;         // the fade prefix (per frame)
            rd->uploadCount = static_cast<u32>(set.world.Size()); // the whole set, uploaded once
            rd->fadeStart = layer.fadeStart; // the per-instance dissolve in the vertex shaders
            rd->fadeEnd = layer.fadeEnd;
            rd->version = set.version;         // the scatter (re-upload only on change)
            rd->mesh = mesh;
            rd->material = material;
            rd->submeshMaterials = perSlot ? cache.materials.Data() : nullptr;
            rd->submeshMaterialCount = perSlot ? static_cast<u32>(cache.materials.Size()) : 0u;
            rd->worldCenter = set.worldCenter;
            rd->worldRadius = set.worldRadius;
            rd->entityId = engine::render::PackEntity(owner);
            rd->category = engine::render::CategoryForMaterial(material);
            rd->sortBatchKey = render::BatchKey(mesh, material);
            rd->castShadows = layer.castShadows;
        }
    }

    void TerrainVegetationComponentManager::ExtractRenderData(render::ExtractedScene& snapshot)
    {
        PROFILE_SCOPE("Vegetation.Extract");
        if (m_scene == nullptr)
        {
            return;
        }
        for (auto& entry : m_caches)
        {
            entry.value.seenThisFrame = false;
        }
        u32 budget = m_buildBudget;

        ForEach(
            [&](TerrainVegetationComponent& c, scene::EntityHandle owner)
            {
                // Hidden or inactive: the layers keep their caches (mark them seen), draw nothing.
                const bool active = c.visible && m_scene->IsEffectivelyActive(owner);
                scene::EntityHandle terrainEntity{};
                engine::terrain::TerrainComponent* tc =
                    active ? FindTerrainFor(*m_scene, owner, terrainEntity) : nullptr;
                foundation::terrain::TerrainResource* res =
                    tc != nullptr ? tc->terrain.Get() : nullptr;
                heightfield::Heightfield* hf = res != nullptr ? res->heightfield.Get() : nullptr;
                if (hf == nullptr || hf->IsEmpty())
                {
                    for (u32 li = 0; li < c.proceduralLayers.Size(); ++li)
                    {
                        CacheFor(owner, ProceduralSlot(li)).seenThisFrame = true;
                    }
                    for (u32 li = 0; li < c.propLayers.Size(); ++li)
                    {
                        CacheFor(owner, PropSlot(li)).seenThisFrame = true;
                    }
                    return;
                }
                const tmodel::SplatWeights* splat = res->weights.Get();
                const veg::VegetationMask* mask = c.mask.Get();
                const Guid ownerId = m_scene->GetEntityId(owner);
                const Float4x4 entityWorld = m_scene->GetWorldMatrix(terrainEntity);
                for (u32 li = 0; li < c.proceduralLayers.Size(); ++li)
                {
                    const ProceduralVegetationLayer& grown = c.proceduralLayers[li];
                    ExtractLayer(snapshot, owner, ownerId, ProceduralSlot(li), grown,
                                 grown.ToScatterLayer(), /*props*/ false, {}, *hf, splat, mask,
                                 entityWorld, budget);
                }
                for (u32 li = 0; li < c.propLayers.Size(); ++li)
                {
                    const PropVegetationLayer& placed = c.propLayers[li];
                    ExtractLayer(snapshot, owner, ownerId, PropSlot(li), placed,
                                 placed.ToScatterLayer(), /*props*/ true,
                                 Span<const Float4x4>{placed.instances.Data(),
                                                      placed.instances.Size()},
                                 *hf, splat, mask, entityWorld, budget);
                }
            });

        // Layers that no longer exist (removed slots, removed components, destroyed entities)
        // drop their caches; the renderer evicts their buffers.
        Array<u64> stale;
        for (const auto& entry : m_caches)
        {
            if (!entry.value.seenThisFrame)
            {
                stale.PushBack(entry.key);
            }
        }
        for (u64 key : stale)
        {
            (void)m_caches.Remove(key);
        }
        m_pendingRegions.Clear();
    }

    namespace
    {
        // A resource that is named but not resolved yet: still loading (or never will: a
        // deleted asset, which the drawing warns about on its own).
        template <typename T>
        [[nodiscard]] bool Pending(const foundation::resource::Ref<T>& ref) noexcept
        {
            return !ref.id.IsNil() && ref.Get() == nullptr;
        }
    }

    bool TerrainVegetationComponentManager::CollectStaticCapsules(scene::Scene& scene,
                                                                  Array<scene::StaticCapsule>& outCapsules)
    {
        if (m_scene == nullptr)
        {
            return true;
        }
        bool ready = true;
        ForEach(
            [&](TerrainVegetationComponent& c, scene::EntityHandle owner)
            {
                const auto solid = [](const VegetationLayerBase& layer) { return layer.collisionRadius > 0.0f; };
                bool any = false;
                for (const ProceduralVegetationLayer& layer : c.proceduralLayers)
                {
                    any = any || solid(layer);
                }
                for (const PropVegetationLayer& layer : c.propLayers)
                {
                    any = any || solid(layer);
                }
                // An inactive terrain has no trunks, as an inactive rigid body has no body. The view
                // toggles (the component's and a layer's `visible`) do not matter.
                if (!any || !scene.IsEffectivelyActive(owner))
                {
                    return;
                }
                scene::EntityHandle terrainEntity{};
                engine::terrain::TerrainComponent* tc = FindTerrainFor(scene, owner, terrainEntity);
                foundation::terrain::TerrainResource* res = tc != nullptr ? tc->terrain.Get() : nullptr;
                heightfield::Heightfield* hf = res != nullptr ? res->heightfield.Get() : nullptr;
                if (tc == nullptr)
                {
                    return; // no terrain to stand on: nothing grows, nothing is solid
                }
                if (hf == nullptr || hf->IsEmpty() || Pending(res->weights) || Pending(c.mask))
                {
                    ready = false;
                    return;
                }
                const tmodel::SplatWeights* splat = res->weights.Get();
                const veg::VegetationMask* mask = c.mask.Get();
                const Guid ownerId = scene.GetEntityId(owner);
                const Float4x4 entityWorld = scene.GetWorldMatrix(terrainEntity);
                const auto collect = [&](u32 slot, const VegetationLayerBase& base, const veg::ScatterLayer& layer,
                                         bool props, Span<const Float4x4> authored)
                {
                    if (!solid(base))
                    {
                        return;
                    }
                    if (Pending(base.mesh))
                    {
                        ready = false;
                        return;
                    }
                    LayerCache* cache = PrepareLayer(owner, ownerId, slot, base, layer, props, authored, *hf,
                                                     splat, mask, entityWorld);
                    if (cache == nullptr)
                    {
                        return;
                    }
                    const AABB meshBounds = base.mesh.Get()->bounds;
                    for (u32 i = 0; i < cache->sets.Size(); ++i)
                    {
                        if (cache->sets[i].dirty)
                        {
                            // Every chunk, whatever the camera sees: the same build the drawing does.
                            BuildSet(*cache, i, *hf, splat, mask, layer, meshBounds, props, authored);
                        }
                        for (const Float4x4& m : cache->sets[i].world)
                        {
                            // The instance's scale from its matrix (its X axis's length: the scatter's
                            // scale is uniform), its foot its translation.
                            const f32 scale = Length(Float3{m.m[0][0], m.m[0][1], m.m[0][2]});
                            scene::StaticCapsule capsule;
                            capsule.foot = Float3{m.m[3][0], m.m[3][1], m.m[3][2]};
                            capsule.radius = base.collisionRadius * scale;
                            capsule.height = base.collisionHeight * scale;
                            capsule.group = base.collisionGroup;
                            outCapsules.PushBack(capsule);
                        }
                    }
                };
                for (u32 li = 0; li < c.proceduralLayers.Size(); ++li)
                {
                    const ProceduralVegetationLayer& grown = c.proceduralLayers[li];
                    collect(ProceduralSlot(li), grown, grown.ToScatterLayer(), false, {});
                }
                for (u32 li = 0; li < c.propLayers.Size(); ++li)
                {
                    const PropVegetationLayer& placed = c.propLayers[li];
                    collect(PropSlot(li), placed, placed.ToScatterLayer(), true,
                            Span<const Float4x4>{placed.instances.Data(), placed.instances.Size()});
                }
            });
        return ready;
    }
}

namespace engine::vegetation
{
    const engine::DomainModule& VegetationDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::vegetation::kVegetationResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"vegetation",
            .installScene = &AddVegetationSceneManagers,
            .registerReflection = &RegisterVegetationComponentReflection,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
