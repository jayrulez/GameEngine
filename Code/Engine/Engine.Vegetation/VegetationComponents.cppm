// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.vegetation:components - the TerrainVegetationComponent + its manager.
//
// ONE component on the terrain entity holds TWO layer lists (user, 2026-09-23): the PROCEDURAL
// layers (`Array<ProceduralVegetationLayer>`: grass from a splat layer, flowers from a painted
// mask plane, moss everywhere) that the scatter grows per chunk from the heightfield and their
// source, and the PROP layers (`Array<PropVegetationLayer>`: rocks, stumps) whose instances the
// Paint Props brush places by hand. Before the split one list carried both and a `placement`
// value decided what a layer was, so a procedural layer dragged an empty instance array and a
// prop layer a density that meant nothing. Both kinds share the base (`VegetationLayerBase`):
// mesh, material, scale, slope and height rules, fade, shadows, visibility, the per-chunk cap.
// The reflected inspector edits each layer in place (the generic list editor builds a per-slot
// expander of leaf rows through a ComponentPropertyPath, the pickers included), so a layer is a
// list slot, not an entity. The manager finds the terrain on the component's entity or the
// nearest ancestor with a TerrainComponent.
//
// The manager is the scene's IRenderDataProvider for vegetation: per (layer, chunk) it scatters
// on demand (Foundation::Vegetation, a pure function of the seed) or buckets the authored
// instances, keeps the set's terrain-local instances composed into world space, and emits ONE
// MultiMeshRenderData per set in range with the fade prefix as its count. Sets out of range are
// absent from the snapshot; the renderer evicts their GPU buffers after kMultiMeshEvictFrames.
// Invalidation: the heightfield uid + version, the splat uid + version, the entity world matrix
// and the layer's scatter hash; a region notice (InvalidateRegion, the editor brushes) regrows
// only the touched chunks.

module;
#include "Core/Prelude.h"

export module engine.vegetation:components;

import foundation.core;
import engine.domain;
import foundation.scene;
import foundation.resource;
import foundation.geometry;         // StaticMesh (bounds)
import foundation.materials;        // Material
import foundation.heightfield;      // Heightfield + HeightfieldRegion
import foundation.terrain;          // TerrainChunk + BuildChunks
import foundation.terrain.resource; // SplatWeights
import foundation.vegetation.resource; // VegetationMask (the painted density planes)
import foundation.vegetation;          // ScatterLayer (scatter params) + ScatterChunk + fade
import foundation.render;           // IRenderDataProvider, ExtractedScene, MultiMeshRenderData

using namespace foundation::core;

export namespace engine::vegetation
{
    namespace render = foundation::render;
    namespace scene = foundation::scene;
    namespace heightfield = foundation::heightfield;
    namespace tmodel = foundation::terrain;
    namespace veg = foundation::vegetation;

    using foundation::vegetation::VegetationPlacement;
    using foundation::vegetation::kSplatBaseLayer;

    // What every layer has, procedural or prop: the mesh, how each instance stands, how it
    // fades, what it costs. Mirrors foundation::vegetation::ScatterLayer FLAT (the inspector
    // edits leaf fields); FillScatterLayer() is the bridge to the pure scatter's rules.
    struct VegetationLayerBase
    {
        String name; // the inspector's slot label ("Grass")
        foundation::resource::Ref<foundation::geometry::StaticMesh> mesh;   // a card, a tuft, a rock
        // One per mesh slot, as a mesh component's list: a single entry covers the whole mesh, more
        // route each submesh to its slot's (a modelled tree's bark, needles and snow); none = the
        // default material.
        Array<foundation::resource::Ref<foundation::materials::Material>> materials;
        Float2 scaleRange{0.8f, 1.2f};
        f32 maxSlopeDegrees = 35.0f;
        Float2 heightRange{-1.0e6f, 1.0e6f}; // terrain-local Y window
        bool alignToNormal = false;
        f32 fadeStart = 40.0f; // metres: full density inside
        f32 fadeEnd = 80.0f;   // metres: nothing beyond
        bool castShadows = false;
        u32 maxInstancesPerChunk = 4096;
        bool visible = true;

        void FillScatterLayer(veg::ScatterLayer& layer) const noexcept
        {
            layer.scaleRange = scaleRange;
            layer.maxSlopeDegrees = maxSlopeDegrees;
            layer.heightRange = heightRange;
            layer.alignToNormal = alignToNormal;
            layer.fadeStart = fadeStart;
            layer.fadeEnd = fadeEnd;
            layer.castShadows = castShadows;
            layer.maxInstancesPerChunk = maxInstancesPerChunk;
        }
    };

    // A layer the scatter GROWS from a source: everywhere (Uniform), where a terrain splat layer
    // is painted (Splat), where a plane of the component's mask is painted (Mask), or both
    // (SplatTimesMask). Nothing per instance is stored.
    struct ProceduralVegetationLayer : VegetationLayerBase
    {
        // Mask by default: a new layer follows the plane you paint, and grows nothing until the
        // component has a mask with paint on that plane (Uniform would grow the moment a mesh is
        // assigned, which reads as an unasked-for scatter - the 2026-09-22 ruling, kept).
        VegetationPlacement placement = VegetationPlacement::Mask;
        u32 splatLayer = 0;         // Splat: palette index; kSplatBaseLayer = the unpainted base
        f32 splatThreshold = 0.25f; // Splat: share below which nothing grows
        u32 maskPlane = 0;          // Mask / SplatTimesMask: the plane of the component's mask
        f32 density = 2.0f;         // instances per square metre

        [[nodiscard]] veg::ScatterLayer ToScatterLayer() const noexcept
        {
            veg::ScatterLayer layer;
            FillScatterLayer(layer);
            layer.placement = placement;
            layer.splatLayer = splatLayer;
            layer.splatThreshold = splatThreshold;
            layer.maskPlane = maskPlane;
            layer.density = density;
            return layer;
        }
    };

    // A layer of PLACED instances (TERRAIN-LOCAL, like the procedural scatter's), painted by the
    // Paint Props brush with the base rules and its own spacing; the manager buckets them per
    // chunk. `instances` is not an inspector row (a list of matrices has no editor): the brush
    // is the editor.
    struct PropVegetationLayer : VegetationLayerBase
    {
        Array<Float4x4> instances;

        // The rules the stamp applies (slope, height, scale, alignment, the fade); no source.
        [[nodiscard]] veg::ScatterLayer ToScatterLayer() const noexcept
        {
            veg::ScatterLayer layer;
            FillScatterLayer(layer);
            layer.placement = VegetationPlacement::Uniform;
            layer.density = 0.0f;
            return layer;
        }
    };

    // A layer of data version 2 carried one optional material: it is the whole list's one entry.
    inline void ReadSingleMaterial(ISerializer& ar, VegetationLayerBase& l)
    {
        foundation::resource::Ref<foundation::materials::Material> material;
        foundation::core::Serialize(ar, "material", material);
        l.materials.Clear();
        if (!material.id.IsNil())
        {
            l.materials.PushBack(Move(material));
        }
    }

    inline void SerializeLayerBase(ISerializer& ar, VegetationLayerBase& l)
    {
        foundation::core::Serialize(ar, "name", l.name);
        foundation::core::Serialize(ar, "mesh", l.mesh);
        if (ar.Mode() == SerializeMode::Read && ar.Version() == 2) // the one-material layout
        {
            ReadSingleMaterial(ar, l);
        }
        else
        {
            foundation::core::Serialize(ar, "materials", l.materials);
        }
        foundation::core::Serialize(ar, "scaleRange", l.scaleRange);
        foundation::core::Serialize(ar, "maxSlopeDegrees", l.maxSlopeDegrees);
        foundation::core::Serialize(ar, "heightRange", l.heightRange);
        foundation::core::Serialize(ar, "alignToNormal", l.alignToNormal);
        foundation::core::Serialize(ar, "fadeStart", l.fadeStart);
        foundation::core::Serialize(ar, "fadeEnd", l.fadeEnd);
        foundation::core::Serialize(ar, "castShadows", l.castShadows);
        foundation::core::Serialize(ar, "maxInstancesPerChunk", l.maxInstancesPerChunk);
        foundation::core::Serialize(ar, "visible", l.visible);
    }

    inline void Serialize(ISerializer& ar, ProceduralVegetationLayer& l)
    {
        SerializeLayerBase(ar, l);
        foundation::core::Serialize(ar, "placement", l.placement);
        foundation::core::Serialize(ar, "splatLayer", l.splatLayer);
        foundation::core::Serialize(ar, "splatThreshold", l.splatThreshold);
        foundation::core::Serialize(ar, "maskPlane", l.maskPlane);
        foundation::core::Serialize(ar, "density", l.density);
    }

    inline void Serialize(ISerializer& ar, PropVegetationLayer& l)
    {
        SerializeLayerBase(ar, l);
        foundation::core::Serialize(ar, "instances", l.instances);
    }

    // The vegetation over one terrain: its layers, on the TerrainComponent's entity.
    struct TerrainVegetationComponent
    {
        Array<ProceduralVegetationLayer> proceduralLayers; // grown from a source
        Array<PropVegetationLayer> propLayers;             // placed by the Paint Props brush
        // The painted mask (density planes over the footprint); nil = no Mask placement grows.
        // A procedural layer with Mask / SplatTimesMask placement names its plane.
        foundation::resource::Ref<foundation::vegetation::VegetationMask> mask;
        bool visible = true;
    };

    // The one-list layout of data version 1 (until 2026-09-23): every layer carried every
    // field and `placement` 3 (Scattered) made it a prop layer. Read by the legacy reader
    // below, never written; remove it once every scene has been re-saved.
    namespace legacy
    {
        struct VegetationLayerV1
        {
            VegetationLayerBase base;
            u8 placement = 3; // the retired Scattered value: the V1 default
            u32 splatLayer = 0;
            f32 splatThreshold = 0.25f;
            u32 maskPlane = 0;
            f32 density = 2.0f;
            Array<Float4x4> instances;
        };
        constexpr u8 kPlacementScatteredV1 = 3;

        inline void Serialize(ISerializer& ar, VegetationLayerV1& l)
        {
            foundation::core::Serialize(ar, "name", l.base.name);
            foundation::core::Serialize(ar, "mesh", l.base.mesh);
            ReadSingleMaterial(ar, l.base);
            foundation::core::Serialize(ar, "placement", l.placement);
            foundation::core::Serialize(ar, "splatLayer", l.splatLayer);
            foundation::core::Serialize(ar, "splatThreshold", l.splatThreshold);
            foundation::core::Serialize(ar, "maskPlane", l.maskPlane);
            foundation::core::Serialize(ar, "density", l.density);
            foundation::core::Serialize(ar, "scaleRange", l.base.scaleRange);
            foundation::core::Serialize(ar, "maxSlopeDegrees", l.base.maxSlopeDegrees);
            foundation::core::Serialize(ar, "heightRange", l.base.heightRange);
            foundation::core::Serialize(ar, "alignToNormal", l.base.alignToNormal);
            foundation::core::Serialize(ar, "fadeStart", l.base.fadeStart);
            foundation::core::Serialize(ar, "fadeEnd", l.base.fadeEnd);
            foundation::core::Serialize(ar, "castShadows", l.base.castShadows);
            foundation::core::Serialize(ar, "maxInstancesPerChunk", l.base.maxInstancesPerChunk);
            foundation::core::Serialize(ar, "visible", l.base.visible);
            foundation::core::Serialize(ar, "instances", l.instances);
        }

        // Split a V1 list by its placement: Scattered -> a prop layer, anything else -> a
        // procedural one (the retired enumerator's neighbours keep their values).
        inline void SplitLayers(Array<VegetationLayerV1>& old, TerrainVegetationComponent& c)
        {
            c.proceduralLayers.Clear();
            c.propLayers.Clear();
            for (VegetationLayerV1& l : old)
            {
                if (l.placement == kPlacementScatteredV1)
                {
                    PropVegetationLayer prop;
                    static_cast<VegetationLayerBase&>(prop) = Move(l.base);
                    prop.instances = Move(l.instances);
                    c.propLayers.PushBack(Move(prop));
                }
                else
                {
                    ProceduralVegetationLayer grown;
                    static_cast<VegetationLayerBase&>(grown) = Move(l.base);
                    grown.placement = static_cast<VegetationPlacement>(l.placement);
                    grown.splatLayer = l.splatLayer;
                    grown.splatThreshold = l.splatThreshold;
                    grown.maskPlane = l.maskPlane;
                    grown.density = l.density;
                    c.proceduralLayers.PushBack(Move(grown));
                }
            }
        }
    }

    inline void Serialize(ISerializer& ar, TerrainVegetationComponent& c)
    {
        if (ar.Mode() == SerializeMode::Read && ar.Version() == 1)
        {
            // The legacy reader (data version 1, the one-list layout): TypeBuilder's
            // ReadsDataVersionsFrom(1) lets the payload in; the scene re-saves as version 2.
            Array<legacy::VegetationLayerV1> old;
            foundation::core::Serialize(ar, "layers", old);
            legacy::SplitLayers(old, c);
            foundation::core::Serialize(ar, "mask", c.mask);
            foundation::core::Serialize(ar, "visible", c.visible);
            return;
        }
        foundation::core::Serialize(ar, "proceduralLayers", c.proceduralLayers);
        foundation::core::Serialize(ar, "propLayers", c.propLayers);
        foundation::core::Serialize(ar, "mask", c.mask);
        foundation::core::Serialize(ar, "visible", c.visible);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 TerrainVegetationComponent& c)
    {
        const auto bind = [&](VegetationLayerBase& layer)
        {
            layer.mesh.Bind(manager);
            for (foundation::resource::Ref<foundation::materials::Material>& material : layer.materials)
            {
                material.Bind(manager);
            }
        };
        for (ProceduralVegetationLayer& layer : c.proceduralLayers)
        {
            bind(layer);
        }
        for (PropVegetationLayer& layer : c.propLayers)
        {
            bind(layer);
        }
        c.mask.Bind(manager);
    }

    class TerrainVegetationComponentManager final
        : public foundation::scene::SerializableComponentManager<TerrainVegetationComponent>,
          public render::IRenderDataProvider
    {
    public:
        // Chunks scattered per extraction (a cold start spreads over frames).
        static constexpr u32 kDefaultBuildBudget = 4;

        // A layer's SLOT: its index in its list, the prop list flagged in the top bit. The
        // cache key, the chunk seed (the renderer's persistent-buffer key) and the log lines
        // all speak in slots, so the two lists never collide.
        static constexpr u32 kPropSlotBit = 0x80000000u;
        [[nodiscard]] static constexpr u32 ProceduralSlot(u32 index) noexcept { return index; }
        [[nodiscard]] static constexpr u32 PropSlot(u32 index) noexcept { return index | kPropSlotBit; }

        TerrainVegetationComponentManager()
            : SerializableComponentManager<TerrainVegetationComponent>(u8"terrainVegetation")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return false; }

        void SetBuildBudget(u32 chunksPerFrame) noexcept { m_buildBudget = chunksPerFrame; }
        [[nodiscard]] u32 BuildBudget() const noexcept { return m_buildBudget; }

        /// A sculpt or paint over `region` (sample-grid coordinates of the heightfield the layers
        /// grow on) - only the chunks it touches regrow on the next extraction. Without a notice,
        /// a heightfield, splat or mask version bump regrows every chunk (the conservative
        /// fallback). A mask or splat brush maps its texel rect through InvalidateFootprint.
        void InvalidateRegion(const heightfield::HeightfieldRegion& region);

        /// The same notice from a 0..1 FOOTPRINT rect (a mask or splat texel rect over the
        /// terrain footprint, `u0..u1 x v0..v1`), mapped to the heightfield's sample grid of
        /// `gridSize` samples per side.
        void InvalidateFootprint(f32 u0, f32 v0, f32 u1, f32 v1, i32 gridSize);

        /// render::IRenderDataProvider: one MultiMeshRenderData per (layer, chunk) in range of
        /// the snapshot's view origin (every chunk when the snapshot has none - headless).
        void ExtractRenderData(render::ExtractedScene& snapshot) override;

        // ---- introspection (tests + the HUD) ----
        [[nodiscard]] usize BuiltSetCount() const noexcept; // sets holding instances
        [[nodiscard]] usize InstanceCount() const noexcept; // instances across built sets
        [[nodiscard]] u64 BuildCount() const noexcept { return m_builds; } // scatters run, ever

    private:
        struct ChunkSet
        {
            u64 key = 0;           // ChunkSeed - the renderer's persistent-buffer key
            u32 version = 0;       // bumps per rebuild (the renderer re-uploads on change)
            bool built = false;
            bool dirty = true;     // needs a (re)scatter before it can draw
            Array<Float4x4> world; // composed instances (terrain-local x entity world)
            Array<Float4x4> local; // the scatter (kept: a moved entity recomposes, no rescatter)
            AABB localBounds = AABB::Empty();
            Float3 worldCenter = Float3{0.0f, 0.0f, 0.0f};
            f32 worldRadius = 0.0f;
        };

        // The cached sets of one (component entity, layer slot).
        struct LayerCache
        {
            Guid ownerId;
            u32 slot = 0; // ProceduralSlot / PropSlot
            u64 heightfieldUid = 0;
            u64 heightfieldVersion = 0;
            u64 splatUid = 0;
            u64 splatVersion = 0;
            u64 maskUid = 0;
            u64 maskVersion = 0;
            u64 instancesHash = 0; // props: the authored instances' content
            u64 layerHash = 0;
            u64 meshUid = 0;
            Float4x4 entityWorld = Float4x4::Identity();
            bool composed = false;
            i32 chunksPerSide = 0;
            Array<tmodel::TerrainChunk> chunks;
            Array<ChunkSet> sets; // one per chunk, row-major
            bool seenThisFrame = false;
            bool warnedClamp = false;  // the over-budget warning fires once per layer
            bool warnedNoMesh = false; // the unresolved-mesh warning fires once per layer
            // The layer's materials as the renderer borrows them, refreshed from the refs each
            // extraction (a late cook or a hot reload heals live, as a mesh component's do).
            Array<RefPtr<foundation::materials::Material>> materials;
        };

        [[nodiscard]] static u64 CacheKey(scene::EntityHandle owner, u32 slot) noexcept;
        [[nodiscard]] LayerCache& CacheFor(scene::EntityHandle owner, u32 slot);
        void ResetCache(LayerCache& cache, const heightfield::Heightfield& hf, const Guid& ownerId,
                        u32 slot);
        void DirtyAll(LayerCache& cache);
        void BuildSet(LayerCache& cache, u32 chunkIndex, const heightfield::Heightfield& hf,
                      const tmodel::SplatWeights* splat, const veg::VegetationMask* mask,
                      const veg::ScatterLayer& layer, const AABB& meshBounds, bool props,
                      Span<const Float4x4> authored);
        void Compose(LayerCache& cache, ChunkSet& set);
        // One layer of either list: `layer` carries the rules (and the source for a procedural
        // one), `authored` the placed instances of a prop layer (`props`), else empty.
        void ExtractLayer(render::ExtractedScene& snapshot, scene::EntityHandle owner,
                          const Guid& ownerId, u32 slot, const VegetationLayerBase& base,
                          const veg::ScatterLayer& layer, bool props,
                          Span<const Float4x4> authored, const heightfield::Heightfield& hf,
                          const tmodel::SplatWeights* splat, const veg::VegetationMask* mask,
                          const Float4x4& entityWorld, u32& budget);

        scene::Scene* m_scene = nullptr;
        u32 m_buildBudget = kDefaultBuildBudget;
        u64 m_builds = 0;
        HashMap<u64, LayerCache> m_caches; // key = CacheKey(entity, slot)
        Array<heightfield::HeightfieldRegion> m_pendingRegions;
    };

    // Scene-composition hooks (the SceneModule pair, mirroring every other domain).
    void AddVegetationSceneManagers(foundation::scene::Scene& scene);
    void RegisterVegetationComponentReflection();
}

export namespace engine::vegetation
{
    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& VegetationDomain() noexcept;
}
