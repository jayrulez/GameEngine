// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:data` partition.
///
/// The render-data contract - and the boundary that keeps the renderer scene-agnostic.
/// Render data is *extracted and pushed to* the renderer; the renderer never reaches back
/// into a scene (one-way: the scene-integration layer in engine.render depends
/// on this, not the reverse).
///
/// A `RenderData` is a unit of renderable work: a `RenderCategory` tag plus the data a
/// draw needs (e.g. `MeshRenderData` = world matrix + mesh + material). It is allocated
/// from a per-frame `FrameArena` (bump allocator), is trivially destructible, and is valid
/// for exactly one frame. An `ExtractedScene` is the per-scene, once-per-frame, immutable
/// snapshot of all a scene's render data; every view of that scene shares it read-only.
///
/// A `RenderData` carries no view-dependent state: the sort key (which depends on the
/// camera) lives on a per-view `DrawItem`, computed during the view's cull+sort against the
/// shared snapshot.

module;
#include "Core/Prelude.h"
#include <new>
#include <type_traits>

export module foundation.render:data;

import foundation.core;
import foundation.rhi;
import foundation.geometry;
import foundation.materials;

using namespace foundation::core;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;

export namespace foundation::render
{

    namespace rhi = foundation::rhi;

    // Forward MRT G-buffer aux target formats, shared by the PSO config (:mesh_renderer) and the pass /
    // transient declarations (:pipeline): target 1 = view-space normal (octahedral XY), target 2 =
    // screen-space motion vector (UV-delta). RG16Float - enough range/precision for both.
    inline constexpr rhi::TextureFormat kGNormalFormat = rhi::TextureFormat::RG16Float;
    inline constexpr rhi::TextureFormat kGVelocityFormat = rhi::TextureFormat::RG16Float;
    // Target 3 = material params (R=roughness, G=metallic), both in [0,1] → RG8Unorm. Consumed by the
    // screen-space reflection pass (roughness gates/fades SSR; metallic tints it). Written only by the
    // opaque/masked GBUFFER permutation, same as normal/velocity.
    inline constexpr rhi::TextureFormat kGMaterialFormat = rhi::TextureFormat::RG8Unorm;
    // G-buffer diffuse albedo (rgb; a unused): what screen-space GI tints the bounce light it
    // gathers by, since what leaves a surface is the light arriving times its albedo. Written by
    // the same GBUFFER permutation. (Five targets total 22 bytes a sample, under WebGPU's 32.)
    inline constexpr rhi::TextureFormat kGAlbedoFormat = rhi::TextureFormat::RGBA8Unorm;

    // A renderable's category - the dispatch key that routes it to a `Renderer`. A plain u16
    // (not an enum class) so external subsystems (particles, world-space UI) can claim ids
    // beyond the built-ins without touching this enum. Values >= kBuiltinCategoryCount are
    // available to extensions; the `Renderer` registry sizes its table to kMaxCategories.
    using RenderCategory = u16;

    namespace RenderCategories
    {
        inline constexpr RenderCategory Opaque = 0;      // depth-sorted front-to-back
        inline constexpr RenderCategory Masked = 1;      // alpha-tested, opaque-ish
        inline constexpr RenderCategory Transparent = 2; // depth-sorted back-to-front, blended
        inline constexpr RenderCategory Sky = 3;
        inline constexpr RenderCategory Decal = 4;
        inline constexpr RenderCategory Light = 5;
        inline constexpr RenderCategory ReflectionProbe = 6;
        inline constexpr RenderCategory GUI = 7;
        inline constexpr RenderCategory Particle = 8;
        inline constexpr RenderCategory WorldUI = 9; // POST-TONEMAP, depth-tested (world panels)
    }
    inline constexpr u16 kBuiltinCategoryCount = 10;
    inline constexpr u16 kMaxCategories = 64; // registry table size (room for extensions)

    // How a category's draws are depth-ordered (packed into the sort key). FrontToBack for opaque
    // (early-Z + state clustering); BackToFront for blended (correct alpha over-compositing).
    enum class SortMode : u8
    {
        FrontToBack,
        BackToFront
    };

    // Which forward pass emits a category.
    // Opaque = the MRT opaque pass; Blended = the color-only pass after TAA; None = not emitted by the
    // forward passes at all (Sky/Decal/Light have their own dedicated passes or are shading-only inputs).
    enum class PassAffinity : u8
    {
        Opaque,
        Blended,
        PostTonemap,
        None
    };

    // A dynamic registry of render categories (ezEngine-style): categories carry a name + sort/pass
    // metadata and are assigned ids at RegisterCategory time, so extensions (sprites, particles, custom
    // passes) add categories WITHOUT editing the built-in enum. The core pre-registers its built-ins at
    // their well-known ids (so the RenderCategories:: constants stay valid); Register is idempotent by
    // name. One process-wide instance (Categories()); registration happens at init (single-threaded),
    // reads (Sort/Affinity, during draw-list build) are lock-free afterward.
    class CategoryRegistry
    {
    public:
        CategoryRegistry()
        {
            // Built-ins, in id order (0..8) so the ids match the RenderCategories:: constants.
            Register(u8"Opaque", SortMode::FrontToBack, PassAffinity::Opaque);
            Register(u8"Masked", SortMode::FrontToBack, PassAffinity::Opaque);
            Register(u8"Transparent", SortMode::BackToFront, PassAffinity::Blended);
            Register(u8"Sky", SortMode::FrontToBack, PassAffinity::None);   // dedicated sky pass
            Register(u8"Decal", SortMode::FrontToBack, PassAffinity::None); // dedicated decal pass
            Register(u8"Light", SortMode::FrontToBack,
                     PassAffinity::None); // shading input, not drawn
            Register(u8"ReflectionProbe", SortMode::FrontToBack, PassAffinity::None);
            Register(u8"GUI", SortMode::BackToFront, PassAffinity::Blended);
            Register(u8"Particle", SortMode::BackToFront, PassAffinity::Blended);
            // World-space UI: drawn AFTER tonemap, depth-tested against the scene - the
            // panel keeps its authored colors (matching the screen tier) yet still occludes.
            Register(u8"WorldUI", SortMode::BackToFront, PassAffinity::PostTonemap);
        }

        // Register a category by name (idempotent - returns the existing id if the name is taken).
        // Names are borrowed string literals (must outlive the registry). Returns kMaxCategories on overflow.
        RenderCategory Register(StringView name, SortMode sort, PassAffinity affinity);

        [[nodiscard]] SortMode Sort(RenderCategory c) const noexcept;
        [[nodiscard]] PassAffinity Affinity(RenderCategory c) const noexcept;
        [[nodiscard]] StringView Name(RenderCategory c) const noexcept;
        [[nodiscard]] u16 Count() const noexcept { return m_count; }

    private:
        struct Info
        {
            StringView name;
            SortMode sort = SortMode::FrontToBack;
            PassAffinity affinity = PassAffinity::None;
        };
        Info m_info[kMaxCategories];
        u16 m_count = 0;
    };

    // The one process-wide category registry (built-ins pre-registered on first use).
    // NON-inline (RenderDataImpl.cpp): producers (Engine.Terrain, Engine.Particles)
    // register categories and the sorter reads them from Foundation.Render - per-library
    // copies would diverge the ids and draw items would sort into the wrong pass
    // (shared-libraries.md rendezvous rule).
    [[nodiscard]] CategoryRegistry& Categories() noexcept;

    // Base for a unit of renderable work. Arena-allocated, trivially destructible, valid one
    // frame. Dispatch is by `category` (not virtual) - the registered `Renderer` knows the
    // concrete subclass and static_casts, so there is no vtable.
    /// What a RenderData IS, for the few frame-level passes that must read past the base fields
    /// (the shadow caster list reads skinned-caster bones). A one-byte answer with no RTTI: the
    /// subtype's constructor stamps it, so a subclass of MeshRenderData inherits Mesh. This is
    /// the ONLY sanctioned way to downcast a RenderData outside its own renderer - never the
    /// renderer id, which is a registration-order value an external renderer can hold too.
    enum class RenderDataKind : u8
    {
        Generic = 0, // base fields only (terrain, sprites, particles, any external renderer)
        Mesh,        // MeshRenderData or a subclass (MultiMeshRenderData)
    };

    struct RenderData
    {
        RenderCategory category = RenderCategories::Opaque;
        // Which renderer draws this item - the per-item dispatch key (ezEngine-style), so several
        // renderers can share a category (e.g. sprites + transparent meshes both blended) and still be
        // routed correctly. Its value is the renderer's registration id (RendererRegistry assigns them in
        // order); the DEFAULT 0 is the first-registered renderer (the MeshRenderer in the standard
        // subsystem, so mesh data needs no change) - a DISPATCH default, not a type: a frame that
        // registers another renderer first routes id 0 there. Non-mesh producers set their renderer's
        // id. Never infer the concrete type from this id; read `kind`.
        u16 rendererId = 0;
        RenderDataKind kind = RenderDataKind::Generic;
        // View-space depth sort center (world-space) + a batch-clustering key, read GENERICALLY by the
        // draw-list builder (it no longer downcasts to a concrete type). worldCenter drives the depth sort;
        // sortBatchKey folds (mesh,material)-like identity into the sort so same-state draws stay contiguous
        // (opaque only - blended zeroes it so depth dominates). Producers set both at extraction.
        Float3 worldCenter = Float3{0, 0, 0};
        // World-space bounding-sphere radius about worldCenter. Read generically by the draw-list builder
        // for view-frustum culling (every producer sets it: meshes from local bounds, sprites from size).
        f32 worldRadius = 0.0f;
        u32 sortBatchKey = 0;
        // Whether this item is a shadow caster (the sun cascades and the local-light tiles).
        // Producers that draw dense fillers (vegetation) opt out; everything else casts.
        bool castShadows = true;
        // The producer's entity tag (EntityTag layout: index low, generation high) - what the
        // GPU pick pass writes and its readback decodes. 0 = untagged. Set by every producer
        // whose draws should be pickable (meshes, instanced sets, terrain).
        u64 entityId = 0;
    };

    // The one layout of the `entityId` tag a producer stamps on RenderData for picking: the scene
    // entity's slot index in the low word, its generation in the high word. Producers pack (the
    // engine extract), the pick readback unpacks - nothing in between interprets it.
    struct EntityTag
    {
        [[nodiscard]] static constexpr u64 Pack(u32 index, u32 generation) noexcept
        {
            return (static_cast<u64>(generation) << 32) | static_cast<u64>(index);
        }
        [[nodiscard]] static constexpr u32 Index(u64 tag) noexcept
        {
            return static_cast<u32>(tag & 0xFFFFFFFFull);
        }
        [[nodiscard]] static constexpr u32 Generation(u64 tag) noexcept
        {
            return static_cast<u32>(tag >> 32);
        }
    };

    // One mesh draw: a mesh + material at a world transform. Pointers are borrowed for the
    // frame (the producer keeps the resources alive). `worldCenter` is the world-space bounds
    // center, used for view-depth sorting (and, later, culling). `entityId` is an opaque tag
    // the producer may set (e.g. a packed entity handle) for picking - meaningless to the core.
    struct MeshRenderData : RenderData
    {
        MeshRenderData() noexcept { kind = RenderDataKind::Mesh; } // subclasses inherit the stamp
        Float4x4 world = Float4x4::Identity();
        // worldCenter + worldRadius live on the RenderData base now (generic depth sort + cull); see them there.
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f}; // per-instance tint
        // Screen-door fade, 0 (solid) to 1 (gone). A faded mesh draws in the Masked category (out of
        // the depth prepass, whose depth would hide what shows through it) with the DITHER variant,
        // and always through the instanced path, its fade riding DataOffsets.w. Shadows ignore it.
        f32 fade = 0.0f;
        geometry::StaticMesh* mesh = nullptr;
        materials::Material* material = nullptr;
        // Optional per-submesh materials (borrowed array, indexed by SubMesh::materialIndex). When present,
        // the renderer draws each submesh with submeshMaterials[matIdx]; else `material` covers the mesh.
        const RefPtr<materials::Material>* submeshMaterials = nullptr;
        u32 submeshMaterialCount = 0;
        // entityId lives on the RenderData base (generic pick tag); see it there.
        // GPU skinning: per-bone skinning matrices for a skinned mesh (borrowed for the frame, from an
        // AnimationPlayer). When non-null + the mesh IsSkinned(), the renderer uploads them to its bone
        // pool and draws the SKINNED permutation; otherwise the mesh draws static (bind pose).
        const Float4x4* boneMatrices = nullptr;
        const Float4x4* prevBoneMatrices =
            nullptr; // previous-frame skinning matrices (motion vectors); null => reuse current
        u32 boneCount = 0;
        // LOD knobs, copied from the component at extraction. Selection is
        // PER VIEW in the renderer (extraction is one snapshot shared by every view), from
        // projected-sphere coverage of worldCenter/worldRadius vs the mesh's chain.
        f32 lodBias = 0.0f;
        i32 forceLod = -1; // -1 = automatic
        // Discriminator: when true this is actually a `MultiMeshRenderData` (an instanced SET drawn as one
        // item). The Resolve loop only sees `MeshRenderData*`, so it branches on this flag and downcasts.
        // Regular meshes leave it false and are unaffected.
        bool multiMesh = false;
    };
    static_assert(std::is_trivially_destructible_v<MeshRenderData>);

    // An instanced-mesh SET (a "MultiMesh"): ONE shared mesh+material drawn `instanceCount` times, whose
    // per-instance transforms live in a PERSISTENT GPU buffer owned by the renderer (keyed by `key`),
    // uploaded only when `version` changes. Extraction emits ONE of these per InstancedMeshComponent (not
    // one per instance), so per-frame CPU is O(1) in the instance count. The base `MeshRenderData` carries
    // the shared mesh/material/color and the MERGED bounds (worldCenter/worldRadius) so the set culls as a
    // single AABB; `world` is unused (each instance has its own transform in `transforms`).
    // How a skinned MultiMesh instance picks its pose out of the M shared palettes. Hashed decorrelates
    // from any spatial layout (splitmix-scattered) - the natural default for an independent-agent crowd;
    // Sequential (i % M) makes a phase gradient/wave; Explicit lets the CALLER supply a per-instance pose
    // index (needed for layout-aware looks the renderer can't compute from the flat index i - columns,
    // spatial clusters, gameplay state). Explicit with a null/mismatched index array falls back to Hashed.
    enum class PoseAssignment : u8
    {
        Hashed,
        Sequential,
        Explicit
    };

    struct MultiMeshRenderData : MeshRenderData
    {
        u64 key = 0; // stable per-component id -> the renderer's persistent buffer slot
        const Float4x4* transforms =
            nullptr; // borrowed per-instance world transforms (instanceCount entries), valid this frame
        const Color* tints =
            nullptr; // optional borrowed per-instance tint (instanceCount entries); null => use `color`
        u32 instanceCount = 0;
        // Instances to UPLOAD when more than instanceCount are borrowed (0 = instanceCount): a
        // distance-faded set uploads its whole list once and draws a prefix that moves with the
        // camera without re-uploading (`transforms`/`tints` then hold uploadCount entries).
        u32 uploadCount = 0;
        // Per-instance distance fade (metres; fadeEnd <= 0 = none): full density inside fadeStart,
        // a smoothstep to nothing at fadeEnd, evaluated PER INSTANCE in the vertex shaders against
        // its own camera distance and its rank (its position in the set's random order, which the
        // renderer writes into its tint alpha at upload). The draw-count prefix stays the coarse
        // bound; this is what removes the seam between chunks (a vegetation layer's window).
        f32 fadeStart = 0.0f;
        f32 fadeEnd = 0.0f;
        u32 version = 0; // bumps when `transforms` change; the renderer re-uploads only on a change
        // GPU-skinned crowds (SS7): a shared pose pool of `poseCount` palettes (each `boneCount` matrices),
        // borrowed for the frame. When non-null the set draws SKINNED with instance i using pose (i % poseCount).
        const Float4x4* posePool = nullptr;
        const Float4x4* prevPosePool =
            nullptr;       // last frame's palettes (per-bone motion vectors); null => reuse current
        u32 poseCount = 0; // M unique phase buckets
        u32 boneCount = 0; // bones per palette
        // Pose-selection policy + the optional per-instance index array it consumes when Explicit (borrowed,
        // instanceCount entries). poseIndices is null unless poseAssignment==Explicit AND the caller's array
        // was present and correctly sized; the renderer then falls back to Hashed.
        PoseAssignment poseAssignment = PoseAssignment::Hashed;
        const u32* poseIndices = nullptr;
    };
    static_assert(std::is_trivially_destructible_v<MultiMeshRenderData>);

    // The renderer's per-instance pose pick out of the M shared palettes (used in the skinned-MultiMesh
    // offsets fill), factored out pure so it can be unit-tested without a GPU. `i` is the per-SET instance
    // index. Explicit consumes the caller's array (null => the array was absent/mismatched, degrade to
    // Hashed). Sequential = i % M (a phase gradient, but only meaningful when a set's instances ARE laid
    // out in the order you want the gradient). Hashed = splitmix-scattered, decorrelated from any layout.
    // poseCount == 0 returns 0. All branches index within [0, poseCount).
    [[nodiscard]] inline u32 SelectPose(PoseAssignment policy, u32 i, u32 poseCount,
                                        const u32* explicitIndices) noexcept
    {
        if (poseCount == 0)
        {
            return 0;
        }
        if (policy == PoseAssignment::Explicit && explicitIndices != nullptr)
        {
            return explicitIndices[i] % poseCount;
        }
        if (policy == PoseAssignment::Sequential)
        {
            return i % poseCount;
        }
        return static_cast<u32>(HashInteger(i) % poseCount);
    }

    // Crowd-authoring helpers for building an Explicit per-instance pose-index array from a character's GRID
    // position. The renderer sees only the flat index, so layout-aware looks are precomputed with these and
    // handed over as Explicit. All return an index in [0, poseCount); poseCount must be > 0 (cellSize >= 1).
    [[nodiscard]] inline u32 ColumnPose(u32 col, u32 poseCount) noexcept
    {
        return col % poseCount;
    } // whole column shares a phase (formation)
    [[nodiscard]] inline u32 WavePose(u32 col, u32 row, u32 poseCount) noexcept
    {
        return (col + row) % poseCount;
    } // diagonal phase gradient (a wave)
    [[nodiscard]] inline u32 ClusterPose(u32 col, u32 row, u32 cellSize, u32 poseCount) noexcept
    { // cellSize×cellSize cells share a phase; cells hashed
        const u32 cx = col / cellSize, cz = row / cellSize;
        return static_cast<u32>(HashInteger((static_cast<u64>(cx) << 32) ^ static_cast<u64>(cz)) %
                                poseCount);
    }

    // One textured billboard quad. A camera-facing (or world-aligned) sprite drawn by the SpriteRenderer,
    // which shares the blended forward pass with transparent meshes (interleaved by depth). Its world
    // position is the RenderData base `worldCenter`; extraction sets category = Transparent and stamps
    // rendererId with the sprite renderer's id. `texture` is borrowed (the app/resource keeps it alive).
    struct SpriteRenderData : RenderData
    {
        Float2 size = Float2{1.0f, 1.0f};               // world-unit width/height
        Float4 uvRect = Float4{0.0f, 0.0f, 1.0f, 1.0f}; // atlas sub-rect (u, v, w, h)
        Color tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
        u32 orientation =
            0; // 0 = camera-facing, 1 = about world-Y, 2 = world XY, 3 = entity-oriented
        bool additive = false;    // blend: false = alpha over, true = additive
        bool postTonemap = false; // WorldUI category: after tonemap, authored colors intact
        Float3 axisRight = Float3{1.0f, 0.0f, 0.0f}; // EntityOriented: world right axis
        Float3 axisUp = Float3{0.0f, 1.0f, 0.0f};    // EntityOriented: world up axis
        rhi::TextureView* texture = nullptr;
    };
    static_assert(std::is_trivially_destructible_v<SpriteRenderData>);

    // One screen-space projected decal - NOT a `RenderData`/drawable (it isn't dispatched through the
    // Renderer path); it's a snapshot list consumed by the standalone DecalPass, like the light list.
    // `world` is the decal's oriented unit box (scale = box size); it projects along its local +Z axis.
    // `texture` is borrowed (the app/resource keeps it alive).
    struct DecalInstance
    {
        Float4x4 world = Float4x4::Identity();
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f};
        f32 fadeStart =
            0.0f; // angle-fade start (radians): full opacity until the surface tilts past this
        f32 fadeEnd =
            1.30f; // angle-fade end (radians ~75deg): fully faded once the surface tilts past this
        rhi::TextureView* texture = nullptr;
    };

    // One light, packed for a GPU storage buffer (std430, 64 bytes = 4x float4). A shading input,
    // not a drawable - extracted into the ExtractedScene's light list, uploaded to a storage buffer,
    // and consumed by the forward shading loop. Directional: dir is the light direction; Point/Spot:
    // position + range (+ spot cone cosines). type: 0 = Directional, 1 = Point, 2 = Spot.
    struct GpuLight
    {
        Float3 positionWS = Float3{0, 0, 0};
        f32 range = 0.0f; // xyz pos, w range
        Float3 color = Float3{1, 1, 1};
        f32 intensity = 1.0f; // rgb color, a intensity
        Float3 directionWS = Float3{0, -1, 0};
        f32 type = 0.0f; // xyz dir, w type
        f32 innerCos = 1.0f;
        f32 outerCos = 1.0f; // spot cone cosines
        // shadowIndex: -1 = this light casts no shadow; else an index into the shadow data (a single
        // directional shadow map, so any >= 0 selects it).
        f32 shadowIndex = -1.0f;
        // How dark the light's shadow gets: 1 = full, 0 = none (the shader lerps toward lit).
        f32 shadowStrength = 1.0f;
    };
    static_assert(sizeof(GpuLight) == 64);

    // The active directional shadow caster for a scene (the extraction OUTPUT): just the light direction
    // + whether one exists. The cascade matrices are derived later (in RenderFrame, where the camera
    // frustum is available) since CSM fitting needs the camera. valid == false => no shadow this frame.
    // The shadow biases a light starts with, in one place (the light component and every renderer
    // read these). The normal offset is in shadow TEXELS: the shader pushes a receiver along its
    // normal by this many texels of its cascade (or atlas tile), more as the surface turns from the
    // light. It stays small: the casters' slope-scaled hardware bias carries acne (a wall the sun
    // grazes at N.L 0.17 reads clean at 0.02, Render.Backend.Tests' shadow control probe), and a
    // large offset parts a shadow from its caster. The depth bias is the receiver's compare bias in
    // NDC depth (toward the light, per depth.hlsli).
    struct ShadowBiasDefaults
    {
        static constexpr f32 kNormalBias = 0.02f;
        static constexpr f32 kDepthBias = 0.0009f;      // directional (cascades)
        static constexpr f32 kLocalDepthBias = 0.0015f; // spot / point (atlas)
    };

    // A scene's directional-shadow reach (its environment settings): how far from the camera the
    // cascades cover (clamped to the camera's far plane: a reach independent of how far the camera
    // sees, so a near cascade's texels stay small), how the splits blend (0 = uniform, 1 =
    // logarithmic) and the width of the soft edge the shadow fades over at the reach.
    struct SceneShadowSettings
    {
        f32 distance = 300.0f;
        f32 cascadeSplit = 0.5f;
        f32 fadeDistance = 40.0f;
    };

    struct DirectionalShadow
    {
        Float3 direction = Float3{0, -1, 0};
        bool valid = false;
        f32 normalBias = ShadowBiasDefaults::kNormalBias; // texels
        f32 depthBias = ShadowBiasDefaults::kDepthBias;
        f32 strength = 1.0f;
    };

    // Cascaded shadow map data for the directional caster, computed per-frame from the
    // primary view's frustum + the light direction. kCount cascades, each a world->light-clip matrix +
    // the view-space depth where it ends (cascade selection) + the world size of one shadow texel (for
    // normal-offset bias). Shared by all views (fit to the primary camera).
    struct ShadowCascades
    {
        static constexpr u32 kCount = 4;
        Float4x4 viewProj[kCount] = {Float4x4::Identity(), Float4x4::Identity(),
                                     Float4x4::Identity(), Float4x4::Identity()};
        f32 splitFar[kCount] = {0.0f, 0.0f, 0.0f, 0.0f}; // view-space far depth of each cascade
        f32 texelWorldSize[kCount] = {0.0f, 0.0f, 0.0f,
                                      0.0f}; // world units per texel (normal-offset bias)
        // The caster light's own values (DirectionalShadow), carried to every renderer that samples,
        // and the scene's fade width at the reach (SceneShadowSettings::fadeDistance).
        f32 farFade = 40.0f;
        f32 normalBias = ShadowBiasDefaults::kNormalBias; // texels
        f32 depthBias = ShadowBiasDefaults::kDepthBias;
        f32 strength = 1.0f;
        bool valid = false;
    };

    // One local-light (spot, or a single point-light cube face) shadow entry, packed for a
    // StructuredBuffer (96 bytes). `GpuLight.shadowIndex` selects the entry (point lights use 6
    // consecutive face entries). `atlasScaleBias` maps the light-clip NDC into the light's tile in the
    // shared shadow atlas: uv_atlas = uv_ndc * scale + offset. Built per-frame by the ShadowSystem once
    // the atlas layout is known (so it carries the assigned tile), unlike the directional cascades.
    struct GpuLocalShadow
    {
        Float4x4 viewProj = Float4x4::Identity();   // world -> light clip (perspective)
        Float4 atlasScaleBias = Float4{1, 1, 0, 0}; // xy = uv scale, zw = uv offset (tile in atlas)
        f32 depthBias = ShadowBiasDefaults::kLocalDepthBias; // depth-compare bias (NDC units; the
                                                             // shader applies it toward the light)
        f32 atlasSelect = 0.0f; // atlas array layer: 0 = realtime, 1 = static (5.4)
        // The light's normal offset as world units per unit of distance from the light: its texels
        // times the tile's texel size at distance 1 (the shader multiplies by the distance).
        f32 normalBiasPerDistance = 0.0f;
        f32 pad2 = 0.0f;
    };
    static_assert(sizeof(GpuLocalShadow) == 96);

    // A spot/point light that should cast a shadow - the extraction OUTPUT. The ShadowSystem assigns it
    // atlas tile(s) and builds its perspective view-projection(s) at frame time (when the atlas layout is
    // known), then patches the source light's shadowIndex to point at the built entry. type mirrors
    // GpuLight (1 = point, 2 = spot); point lights expand to 6 cube faces in 5.3b.
    struct LocalShadowCaster
    {
        u32 type = 2; // 1 = point, 2 = spot
        Float3 positionWS = Float3{0, 0, 0};
        Float3 directionWS = Float3{0, -1, 0};
        f32 range = 10.0f;     // perspective far plane
        f32 outerAngle = 0.6f; // spot cone half-angle (radians); fov = 2 * outerAngle
        bool isStatic = false; // Static update mode -> cached static atlas layer (5.4)
        f32 normalBias = ShadowBiasDefaults::kNormalBias; // texels of its atlas tile
        f32 depthBias = ShadowBiasDefaults::kLocalDepthBias;
    };

    // Atlas tile budget for local (spot/point) shadows per frame. A spot consumes 1 tile, a point 6
    // (cube faces). Extraction assigns each caster's shadowIndex = its BASE tile (0-based, into the
    // GpuLocalShadow buffer) and caps total tiles here; the ShadowSystem's atlas must hold this many.
    inline constexpr u32 kMaxLocalShadowTiles = 16;
    // Frame-global cap on GpuLocalShadow ENTRIES (all scenes concatenated) - the renderer's
    // local-shadow ring is sized to this; the pipeline degenerate-fills instead of exceeding it.
    inline constexpr u32 kMaxLocalShadowEntries = 64;

    // How a reflection probe's captured cubemap refreshes (defined in the snapshot layer, reused by the
    // ReflectionProbeComponent). Static = capture once + full prefilter, cache until the probe moves/
    // invalidates. Realtime = re-capture on a round-robin cadence (cheap prefilter). Manual = only on request.
    enum class ProbeUpdateMode : u32
    {
        Static = 0,
        Realtime = 1,
        Manual = 2
    };

    // A reflection probe (extraction OUTPUT). The ReflectionProbeSystem captures the scene into a cubemap
    // from `center`, prefilters it, and the forward samples it with parallax correction against the box
    // [center - halfExtents, center + halfExtents] (world-axis-aligned box, not an OBB). `blendDistance`
    // softens the influence toward the box edge so overlapping probes blend without a seam. `key` is a
    // stable per-entity tag (PackEntity) so the system maps a probe to a persistent GPU slot across frames.
    struct ReflectionProbe
    {
        u64 key = 0;
        Float3 center = Float3{0, 0, 0}; // capture center (world)
        Float3 halfExtents =
            Float3{5, 5, 5};      // box half-extents (world; influence + parallax proxy)
        f32 blendDistance = 1.0f; // soft falloff width inward from the box edge
        f32 intensity = 1.0f;     // reflection multiplier
        u32 resolution = 128;     // captured cube face size
        u32 priority = 0;         // tie-break when volumes overlap (higher wins)
        ProbeUpdateMode update = ProbeUpdateMode::Static;
        bool parallax = true; // box-project the reflection ray (vs infinite env)
    };

    // Reflection-probe budget per frame (bounds the prefiltered cube-array slices + the metadata buffer).
    inline constexpr u32 kMaxReflectionProbes = 16;

    // A per-view draw entry: a sort key (computed against the view's camera) + the shared
    // render data it refers to. The per-view draw list is an Array<DrawItem> the renderer sorts
    // (radix) then walks. RenderData is borrowed from the ExtractedScene (immutable snapshot).
    struct DrawItem
    {
        u64 key = 0;
        const RenderData* data = nullptr;
    };

    // ---- sort keys -------------------------------------------------------------------------
    //
    // 64-bit key, MSB-first significance so a single ascending radix sort yields the desired
    // order: [category:16][state:24][depth:24]. Category groups draws by Renderer; `state`
    // (material/PSO identity) clusters same-pipeline draws to minimize state changes; `depth`
    // orders within that - front-to-back for opaque (early-Z), back-to-front for transparent
    // (correct blending). The producer inverts depth for transparent before packing.

    inline constexpr u32 kSortDepthBits = 24;
    inline constexpr u32 kSortStateBits = 24;

    // Fold two borrowed resource pointers into a batch-clustering key for the sort (was Views::BatchBits).
    // Pointer-derived identity is fine for a transient per-frame key - the renderer re-checks exact
    // equality when fusing draws, so a hash collision only costs a missed fusion, never a wrong draw.
    [[nodiscard]] inline u32 BatchKey(const void* a, const void* b) noexcept
    {
        const usize m = reinterpret_cast<usize>(a);
        const usize n = reinterpret_cast<usize>(b);
        const usize mixed = (m >> 4) * 1099511628211ull + (n >> 4);
        return static_cast<u32>(mixed & ((1u << kSortStateBits) - 1));
    }

    [[nodiscard]] inline u64 MakeSortKey(RenderCategory category, u32 stateBits,
                                         u32 depthBits) noexcept
    {
        const u64 cat = static_cast<u64>(category);
        const u64 state = static_cast<u64>(stateBits) & ((1ull << kSortStateBits) - 1);
        const u64 depth = static_cast<u64>(depthBits) & ((1ull << kSortDepthBits) - 1);
        return (cat << (kSortStateBits + kSortDepthBits)) | (state << kSortDepthBits) | depth;
    }

    // Quantize a normalized [0,1] depth to the 24-bit depth field. `invert` for back-to-front.
    [[nodiscard]] inline u32 QuantizeDepth(f32 depth01, bool invert) noexcept
    {
        f32 d = depth01 < 0.0f ? 0.0f : (depth01 > 1.0f ? 1.0f : depth01);
        if (invert)
        {
            d = 1.0f - d;
        }
        constexpr u32 kMax = (1u << kSortDepthBits) - 1;
        return static_cast<u32>(d * static_cast<f32>(kMax));
    }

    // ---- frame arena -----------------------------------------------------------------------
    //
    // A growable, chunked bump allocator for one frame's RenderData. Allocations are valid
    // until Reset() (which keeps the chunks for reuse next frame - no per-frame churn). Only
    // trivially-destructible types (RenderData subclasses) are allocated, so Reset() reclaims
    // without running destructors. (Phase 2 swaps this for the double-buffered RenderContext
    // with per-worker arenas; the New<T>/Reset contract stays.)
    class FrameArena
    {
    public:
        // The allocator (required - the owner decides) backs the arena's chunks.
        explicit FrameArena(IAllocator& allocator,
                            usize chunkSize = kDefaultChunkSize) noexcept
            : m_allocator(&allocator), m_chunkSize(chunkSize)
        {
        }
        ~FrameArena()
        {
            for (Chunk& c : m_chunks)
            {
                m_allocator->Free(c.data);
            }
        }

        FrameArena(const FrameArena&) = delete;
        FrameArena& operator=(const FrameArena&) = delete;

        template <typename T, typename... Args>
        [[nodiscard]] T* New(Args&&... args)
        {
            static_assert(std::is_trivially_destructible_v<T>,
                          "FrameArena types must be trivially destructible");
            void* p = Allocate(sizeof(T), alignof(T));
            return p != nullptr ? new (p) T{static_cast<Args&&>(args)...} : nullptr;
        }

        [[nodiscard]] void* Allocate(usize size, usize alignment);

        void Reset() noexcept;

        [[nodiscard]] usize ChunkCount() const noexcept { return m_chunks.Size(); }

    private:
        static constexpr usize kDefaultChunkSize = 64 * 1024;
        static constexpr usize kChunkAlign = 16; // >= any RenderData alignment (Float4x4 = 16)

        struct Chunk
        {
            byte* data = nullptr;
            usize size = 0;
        };

        bool AddChunk(usize size);
        IAllocator* m_allocator;

        Array<Chunk> m_chunks;
        usize m_chunkSize;
        usize m_current = 0; // index of the chunk being filled
        usize m_offset = 0;  // bump cursor within m_chunks[m_current]
    };

    // How the scene's environment radiance (sky + IBL source) is produced. The canonical enum lives
    // here in the snapshot layer; the authoring EnvironmentSettings (render.subsystem) references it.
    enum class SkyMode : u32
    {
        Procedural,
        Analytic,
        Color,
        HDREquirect,
        Cubemap
    };

    // Resolved per-view post-processing parameters. Lives in
    // the snapshot layer as PRIMITIVES (no authoring/pass enums) so it embeds in ViewSettings without
    // pulling in the subsystem; the render subsystem resolves a scene's authored PostProcessSettings
    // (or its global override) into this per RenderScene, and the compose passes read it per
    // view. Defaults MATCH the RenderSubsystem's globals, so a default view is unchanged.
    struct ViewPostConfig
    {
        f32 exposure = 1.0f;    // LINEAR multiplier (a scene's EV is resolved via exp2 upstream)
        bool agxTonemap = true; // true = AgX operator, false = clamp
        bool bloomEnabled = true;
        f32 bloomThreshold = 1.0f;
        f32 bloomKnee = 0.6f;
        f32 bloomIntensity = 0.05f;
        u32 aoMode = 0; // AoMode as u32 (0=Off,1=GTAO,2=SSAO) - keeps :data enum-free
        f32 aoStrength = 0.6f;
        f32 aoRadius = 0.5f;
        f32 aoIntensity = 1.0f;
        // Anti-aliasing (TAA and FXAA are mutually exclusive). taaMotionScale stays frame-global.
        bool taaEnabled = false;
        f32 taaBlend = 0.97f;
        f32 taaGamma = 1.25f;
        bool fxaaEnabled = false;
        f32 fxaaSubpixel = 0.75f;
        // Screen-space reflections (enable + intensity per view; the detailed SsrPass::Params config
        // stays frame-global).
        bool ssrEnabled = false;
        f32 ssrIntensity = 1.0f;
        // Screen-space GI (tier 1): one additive diffuse bounce.
        bool ssgiEnabled = false;
        f32 ssgiIntensity = 1.0f;
        // Auto-exposure (eye adaptation): the tonemap multiplies exposure by
        // clamp(key / adaptedLuminance, autoMin..autoMax) when enabled.
        bool autoExposure = false;
        f32 autoExposureKey = 0.18f;
        f32 autoExposureSpeed = 2.0f;        // adaptation rate (1/s)
        f32 autoExposureMin = 0.0625f;       // clamp as LINEAR multipliers (exp2 of the EV window)
        f32 autoExposureMax = 16.0f;
        // Display-referred color grading via a strip LUT (applied after the tonemap operator).
        // Resolved product view + uid (change detection NEVER by pointer); null = no grading.
        rhi::TextureView* gradingLut = nullptr;
        u64 gradingLutUid = 0;
        f32 gradingLutSize = 0.0f; // slice count (16 for a 256x16 strip); 0 = off
        f32 gradingIntensity = 1.0f;
        // Resolved by the subsystem: does this view need motion vectors? (taaEnabled || an SSR temporal
        // pass). Read at forward-pass EXECUTE via the bound view, so it must be pre-resolved here rather
        // than recomputed from frame-global state.
        bool needsMotion = false;
        // Scene-pass MSAA sample count for THIS view (1 = off, 2 or 4). Capability-clamped by the
        // subsystem before it lands here, so this is the count the pipeline actually renders at.
        // 1 leaves the whole single-sample path byte-identical.
        u8 msaaSamples = 1;
    };

    // The per-frame environment snapshot driving IBL + sky. Plain types (colors as Float3) so the
    // snapshot layer carries no authoring dependency.
    struct SkySnapshot
    {
        SkyMode mode = SkyMode::Procedural;
        f32 intensity = 1.0f; // env radiance master: baked into the cube -> background base AND IBL
        // Display-only multiplier on the VISIBLE sky (backdrop + probe reflections of it); NOT baked
        // into the env cube, so it never touches the IBL diffuse/specular lighting.
        f32 backgroundIntensity = 1.0f;
        f32 rotation = 0.0f; // yaw (radians) for HDR/cubemap
        // Resolved sky-texture product (HDREquirect/Cubemap modes; null = the programmatic
        // pixel path or none). Change detection keys on `textureUid`, NEVER the pointer
        // (reloads reuse freed addresses); `textureIsCube` routes it to the right env build.
        rhi::TextureView* texture = nullptr;
        u64 textureUid = 0;
        bool textureIsCube = false;
        Float3 horizon = Float3{0.60f, 0.70f, 0.85f};
        Float3 zenith = Float3{0.15f, 0.30f, 0.65f}; // also the Color-mode color
        Float3 ground = Float3{0.30f, 0.28f, 0.25f};
        f32 sunIntensity = 1.0f;
        f32 sunAngularSize = 0.5f; // sun disc size (degrees)
        f32 turbidity = 3.0f;      // Analytic (Preetham) atmospheric turbidity (~2..10)
        // Render-time dimmers on the sky's LIGHTING (never the visible sky): SH9 diffuse
        // irradiance and prefiltered specular, applied in the forward's IBL block.
        f32 iblDiffuseIntensity = 1.0f;
        f32 iblSpecularIntensity = 1.0f;
    };

    // ---- extracted scene -------------------------------------------------------------------
    //
    // The per-scene, once-per-frame, immutable snapshot pushed to the renderer: world-space
    // render data for one scene. Views of the same scene share it read-only (N cameras = 1
    // extraction). Carries renderables, lights, and environment.
    class ExtractedScene
    {
    public:
        // The allocator (required - the owner decides) backs the arena + item lists.
        explicit ExtractedScene(IAllocator& allocator)
            : m_arena(allocator), m_items(allocator), m_lights(allocator),
              m_localCasters(allocator)
        {
        }

        // Allocate a RenderData subclass from the arena and register it in the snapshot.
        template <typename T, typename... Args>
        [[nodiscard]] T* Add(Args&&... args)
        {
            T* p = m_arena.New<T>(static_cast<Args&&>(args)...);
            if (p != nullptr)
            {
                m_items.PushBack(static_cast<RenderData*>(p));
            }
            return p;
        }

        // Copy a POD array into the snapshot's arena and return the frame-owned view. For render
        // data that must be SELF-CONTAINED: the snapshot is read at record time (RenderFrame::End),
        // after arbitrary scene/manager mutations - borrowed pointers into producer-owned storage
        // are a use-after-free waiting for a mid-frame rebuild (the terrain PIE-start crash).
        // Empty/failed allocations return an empty span; producers treat that as "skip this item".
        template <typename T>
        [[nodiscard]] Span<const T> AddArray(Span<const T> source)
        {
            static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                          "arena arrays must be POD (Reset never runs destructors)");
            if (source.IsEmpty())
            {
                return Span<const T>{};
            }
            void* p = m_arena.Allocate(source.Size() * sizeof(T), alignof(T));
            if (p == nullptr)
            {
                return Span<const T>{};
            }
            MemCopy(p, source.Data(), source.Size() * sizeof(T));
            return Span<const T>{static_cast<const T*>(p), source.Size()};
        }

        // Adopt an externally-allocated RenderData into the snapshot (the data must outlive this
        // snapshot's use - e.g. it lives in a RenderContext per-worker arena owned by the producer).
        // Used by parallel extraction: workers fill their own arenas, then the merge adopts the
        // pointers here single-threaded.
        void AddExternal(RenderData* data);

        // Add a light to the snapshot (shading input, not a drawable).
        void AddLight(const GpuLight& light) { m_lights.PushBack(light); }

        // Register a spot/point light as a shadow caster (the ShadowSystem builds its atlas tile + matrix
        // at frame time). `lightIndex` must equal the light's position in the list (the index AddLight
        // assigns) so its shadowIndex can be patched once the atlas slot is known.
        void AddLocalShadowCaster(const LocalShadowCaster& c) { m_localCasters.PushBack(c); }
        [[nodiscard]] Span<const LocalShadowCaster> LocalShadowCasters() const noexcept;

        // Screen-space decals (consumed by the standalone DecalPass, not the Renderer dispatch).
        void AddDecal(const DecalInstance& d) { m_decals.PushBack(d); }
        [[nodiscard]] Span<const DecalInstance> Decals() const noexcept;

        // Reflection probes (consumed by the ReflectionProbeSystem: capture + prefilter + froxel assignment).
        void AddReflectionProbe(const ReflectionProbe& p) { m_probes.PushBack(p); }
        [[nodiscard]] Span<const ReflectionProbe> ReflectionProbes() const noexcept;

        // The scene's environment ambient (a flat indirect term until IBL lands). Premultiplied
        // color × intensity, applied as `albedo * ambient` in the forward shader.
        void SetAmbient(const Float3& ambient) noexcept { m_ambient = ambient; }
        [[nodiscard]] const Float3& Ambient() const noexcept { return m_ambient; }

        // The scene's sky/IBL environment settings for this frame.
        void SetSky(const SkySnapshot& s) noexcept { m_sky = s; }
        [[nodiscard]] const SkySnapshot& Sky() const noexcept { return m_sky; }

        // The active directional shadow caster. Set during light extraction.
        void SetDirectionalShadow(const DirectionalShadow& s) noexcept { m_shadow = s; }
        [[nodiscard]] const DirectionalShadow& DirectionalShadowData() const noexcept;
        void SetShadowSettings(const SceneShadowSettings& s) noexcept { m_shadowSettings = s; }
        [[nodiscard]] const SceneShadowSettings& ShadowSettings() const noexcept
        {
            return m_shadowSettings;
        }

        // The world position of the FIRST view that renders this snapshot, set by the render
        // subsystem before the scene's providers extract (a producer that thins by distance -
        // vegetation's fade prefix - reads it). One snapshot serves every view of the scene, so
        // a second view sees the first view's thinning; per-view prefixes are a follow-on.
        // HasViewOrigin() is false for a headless extraction (nothing is thinned).
        void SetViewOrigin(const Float3& origin) noexcept
        {
            m_viewOrigin = origin;
            m_hasViewOrigin = true;
        }
        [[nodiscard]] const Float3& ViewOrigin() const noexcept { return m_viewOrigin; }
        [[nodiscard]] bool HasViewOrigin() const noexcept { return m_hasViewOrigin; }

        // The SCENE's clock (this and last frame's seconds), stamped by the scene's environment
        // extraction from a clock that accumulates the scene's own dt - so the context, group and
        // scene time scales and a pause all apply (a paused world's grass stands still; the
        // editor's Simulate ticks the editing scene, so it sways there). The WIND sway phase.
        // HasTime() is false for a snapshot no scene stamped (a probe): the frame clock stands in.
        void SetTime(f32 seconds, f32 prevSeconds) noexcept
        {
            m_timeSeconds = seconds;
            m_prevTimeSeconds = prevSeconds;
            m_hasTime = true;
        }
        [[nodiscard]] f32 TimeSeconds() const noexcept { return m_timeSeconds; }
        [[nodiscard]] f32 PrevTimeSeconds() const noexcept { return m_prevTimeSeconds; }
        [[nodiscard]] bool HasTime() const noexcept { return m_hasTime; }

        // The scene this snapshot was extracted from (scene::Scene::Serial; 0 = none stamped, a
        // probe). Snapshots are pooled and reused across scenes, so this, not the snapshot's
        // address, tells a view's history passes that the scene changed under them.
        void SetSceneSerial(u64 serial) noexcept { m_sceneSerial = serial; }
        [[nodiscard]] u64 SceneSerial() const noexcept { return m_sceneSerial; }

        // Reset for a new frame: drop the item + light lists, rewind the (internal) arena.
        void Reset() noexcept;

        [[nodiscard]] Span<RenderData* const> Items() const noexcept;
        [[nodiscard]] Span<const GpuLight> Lights() const noexcept;
        [[nodiscard]] usize Size() const noexcept { return m_items.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_items.IsEmpty(); }

    private:
        FrameArena m_arena;
        Array<RenderData*> m_items;
        Array<GpuLight> m_lights;
        Array<LocalShadowCaster> m_localCasters; // spot/point shadow casters
        Array<DecalInstance> m_decals;           // screen-space decals (consumed by DecalPass)
        Array<ReflectionProbe> m_probes; // reflection probes (consumed by ReflectionProbeSystem)
        Float3 m_ambient = Float3{0.03f, 0.03f, 0.03f}; // default dim ambient
        SkySnapshot m_sky;                              // sky/IBL environment for this frame
        DirectionalShadow m_shadow;                     // active directional shadow caster
        SceneShadowSettings m_shadowSettings;           // the scene's cascade reach
        Float3 m_viewOrigin = Float3{0.0f, 0.0f, 0.0f}; // first view's camera position (see above)
        bool m_hasViewOrigin = false;
        f32 m_timeSeconds = 0.0f;     // the scene clock (see SetTime)
        f32 m_prevTimeSeconds = 0.0f;
        bool m_hasTime = false;
        u64 m_sceneSerial = 0; // see SetSceneSerial
    };

    // Extension seam (scene-agnostic, à la Sedulous's IRenderDataProvider): a downstream system - e.g.
    // a particle component manager - implements this to contribute render-data into the frame snapshot.
    // It knows nothing about scenes; the implementer already holds its own data (it's typically a scene
    // system that stored its scene). RenderSubsystem registers providers PER SCENE (registration knows
    // the scene; this interface does not) and calls the current scene's providers during extraction.
    class IRenderDataProvider
    {
    public:
        virtual ~IRenderDataProvider() = default;
        virtual void ExtractRenderData(ExtractedScene& snapshot) = 0;
    };

    // ---- radix sort ------------------------------------------------------------------------
    //
    // LSD radix sort of DrawItems by their 64-bit key, ascending - O(N), stable, 8 passes of
    // 8 bits. `scratch` is a caller-owned ping-pong buffer (reused across frames to avoid
    // per-frame allocation). After the call `items` is sorted; `scratch`'s contents are
    // unspecified.
    inline void RadixSortDrawItems(Array<DrawItem>& items, Array<DrawItem>& scratch)
    {
        const usize n = items.Size();
        if (n < 2)
        {
            return;
        }
        scratch.Resize(n);

        Array<DrawItem>* src = &items;
        Array<DrawItem>* dst = &scratch;
        for (u32 shift = 0; shift < 64; shift += 8)
        {
            usize counts[256] = {};
            for (usize i = 0; i < n; ++i)
            {
                ++counts[((*src)[i].key >> shift) & 0xFFu];
            }
            usize total = 0;
            for (u32 b = 0; b < 256; ++b)
            {
                const usize c = counts[b];
                counts[b] = total;
                total += c;
            }
            for (usize i = 0; i < n; ++i)
            {
                const u8 bucket = static_cast<u8>(((*src)[i].key >> shift) & 0xFFu);
                (*dst)[counts[bucket]++] = (*src)[i];
            }
            Array<DrawItem>* tmp = src;
            src = dst;
            dst = tmp;
        }
        // 8 passes (even) → result ends back in `items`; nothing to copy.
    }

} // namespace foundation::render
