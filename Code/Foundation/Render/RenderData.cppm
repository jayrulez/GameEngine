// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:data` partition.
///
/// The render-data contract, and the boundary that keeps the renderer scene-agnostic.
/// Render data is *extracted and pushed to* the renderer; the renderer never reaches back
/// into a scene (one-way: the scene-integration layer in engine.render depends
/// on this, not the reverse).
///
/// A `RenderData` is a unit of renderable work: a `RenderCategory` tag plus the data a
/// draw needs (`MeshRenderData`, for example, is a world matrix, a mesh and a material). It is
/// allocated from a per-frame `FrameArena` (bump allocator), is trivially destructible, and is
/// valid for one frame. An `ExtractedScene` is the per-scene, once-per-frame, immutable
/// snapshot of all a scene's render data; every view of that scene shares it read-only.
///
/// A `RenderData` carries no view-dependent state: the sort key (which depends on the
/// camera) lives on a per-view `DrawItem`, computed during the view's cull and sort against the
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

    // Forward MRT G-buffer aux target formats, shared by the PSO config (:mesh_renderer) and the pass
    // and transient declarations (:pipeline): target 1 is the view-space normal (octahedral XY), target 2
    // the screen-space motion vector (UV delta). RG16Float has enough range and precision for both.
    inline constexpr rhi::TextureFormat kGNormalFormat = rhi::TextureFormat::RG16Float;
    inline constexpr rhi::TextureFormat kGVelocityFormat = rhi::TextureFormat::RG16Float;
    // Target 3 holds material params (R roughness, G metallic), both in [0,1], so RG8Unorm. Consumed by
    // the screen-space reflection pass (roughness gates and fades SSR; metallic tints it). Written only by
    // the opaque and masked GBUFFER permutation, like normal and velocity.
    inline constexpr rhi::TextureFormat kGMaterialFormat = rhi::TextureFormat::RG8Unorm;
    // G-buffer diffuse albedo (rgb; a unused): what screen-space GI tints the bounce light it
    // gathers by, since what leaves a surface is the light arriving times its albedo. Written by
    // the same GBUFFER permutation. (Five targets total 22 bytes a sample, under WebGPU's 32.)
    inline constexpr rhi::TextureFormat kGAlbedoFormat = rhi::TextureFormat::RGBA8Unorm;

    // A renderable's category: the dispatch key that routes it to a `Renderer`. A plain u16
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
        inline constexpr RenderCategory WorldUI = 9; // after tonemap, depth-tested (world panels)
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
    // Opaque is the MRT opaque pass; Blended the color-only pass after TAA; None is not emitted by the
    // forward passes at all (Sky, Decal and Light have their own passes or are shading-only inputs).
    enum class PassAffinity : u8
    {
        Opaque,
        Blended,
        PostTonemap,
        None
    };

    // A dynamic registry of render categories: categories carry a name and sort and pass metadata and
    // are assigned ids at RegisterCategory time, so extensions (sprites, particles, custom passes) add
    // categories without editing the built-in enum. The core pre-registers its built-ins at their
    // well-known ids (so the RenderCategories:: constants stay valid); Register is idempotent by name.
    // There is one process-wide instance (Categories()); registration happens at init
    // (single-threaded), and reads (Sort, Affinity, during draw-list build) are lock-free afterwards.
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
            // World-space UI: drawn after tonemap, depth-tested against the scene, so the
            // panel keeps its authored colors (matching the screen tier) yet still occludes.
            Register(u8"WorldUI", SortMode::BackToFront, PassAffinity::PostTonemap);
        }

        // Register a category by name (idempotent: returns the existing id if the name is taken).
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
    // Not inline (RenderDataImpl.cpp): producers (Engine.Terrain, Engine.Particles)
    // register categories and the sorter reads them from Foundation.Render; per-library
    // copies in a shared-library build would diverge the ids and draw items would sort
    // into the wrong pass.
    [[nodiscard]] CategoryRegistry& Categories() noexcept;

    /// What a RenderData is, for the few frame-level passes that must read past the base fields
    /// (the shadow caster list reads skinned-caster bones). A one-byte answer with no RTTI: the
    /// subtype's constructor stamps it, so a subclass of MeshRenderData inherits Mesh. This is
    /// the only sanctioned way to downcast a RenderData outside its own renderer; the renderer
    /// id is a registration-order value an external renderer can hold too.
    enum class RenderDataKind : u8
    {
        Generic = 0, // base fields only (terrain, sprites, particles, any external renderer)
        Mesh,        // MeshRenderData or a subclass (MultiMeshRenderData)
    };

    // Base for a unit of renderable work. Arena-allocated, trivially destructible, valid one
    // frame. Dispatch is by `category` (not virtual): the registered `Renderer` knows the
    // concrete subclass and static_casts, so there is no vtable.
    struct RenderData
    {
        RenderCategory category = RenderCategories::Opaque;
        // Which renderer draws this item: the per-item dispatch key, so several renderers can share a
        // category (sprites and transparent meshes are both blended) and still be routed correctly. Its
        // value is the renderer's registration id (RendererRegistry assigns them in order); the default
        // 0 is the first registered renderer (the MeshRenderer in the standard subsystem, so mesh data
        // needs no change). That is a dispatch default, not a type: a frame that registers another
        // renderer first routes id 0 there. Non-mesh producers set their renderer's id. Never infer
        // the concrete type from this id; read `kind`.
        u16 rendererId = 0;
        RenderDataKind kind = RenderDataKind::Generic;
        // View-space depth sort center (world-space) and a batch-clustering key, read generically by the
        // draw-list builder without downcasting. worldCenter drives the depth sort; sortBatchKey folds
        // (mesh, material)-like identity into the sort so same-state draws stay contiguous (opaque only;
        // blended zeroes it so depth wins). Producers set both at extraction.
        Float3 worldCenter = Float3{0, 0, 0};
        // World-space bounding-sphere radius about worldCenter. Read generically by the draw-list builder
        // for view-frustum culling (every producer sets it: meshes from local bounds, sprites from size).
        f32 worldRadius = 0.0f;
        u32 sortBatchKey = 0;
        // Whether this item is a shadow caster (the sun cascades and the local-light tiles).
        // Producers that draw dense fillers (vegetation) opt out; everything else casts.
        bool castShadows = true;
        // The producer's entity tag (EntityTag layout: index low, generation high): what the
        // GPU pick pass writes and its readback decodes. 0 means untagged. Set by every producer
        // whose draws should be pickable (meshes, instanced sets, terrain).
        u64 entityId = 0;
    };

    // The one layout of the `entityId` tag a producer stamps on RenderData for picking: the scene
    // entity's slot index in the low word, its generation in the high word. Producers pack (the
    // engine extract), the pick readback unpacks, and nothing in between interprets it.
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

    // One material property set for one mesh alone (its owner's override, borrowed into the draw):
    // the draw's material in `slot` gets an instance of its own carrying `value`, as the material
    // authors it (a colour sRGB rgba, an HDR colour sRGB rgb with its intensity in w). `size` is the
    // bytes written: 4 for a float, 16 for a Float4.
    struct MaterialPropertyOverride
    {
        u32 slot = 0;
        u32 size = sizeof(f32);
        Float4 value = Float4{0, 0, 0, 0};
        String name;
    };

    // One mesh draw: a mesh and material at a world transform. Pointers are borrowed for the
    // frame (the producer keeps the resources alive).
    struct MeshRenderData : RenderData
    {
        MeshRenderData() noexcept { kind = RenderDataKind::Mesh; } // subclasses inherit the stamp
        Float4x4 world = Float4x4::Identity();
        // worldCenter and worldRadius live on the RenderData base (generic depth sort and cull).
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f}; // per-instance tint
        // Screen-door fade, 0 (solid) to 1 (gone). A faded mesh draws in the Masked category (out of
        // the depth prepass, whose depth would hide what shows through it) with the DITHER variant,
        // its fade riding the instance data or, drawn alone, the object block. Shadows ignore it.
        f32 fade = 0.0f;
        geometry::StaticMesh* mesh = nullptr;
        materials::Material* material = nullptr;
        // Optional per-submesh materials (borrowed array, indexed by SubMesh::materialIndex). When present,
        // the renderer draws each submesh with submeshMaterials[matIdx]; else `material` covers the mesh.
        const RefPtr<materials::Material>* submeshMaterials = nullptr;
        u32 submeshMaterialCount = 0;
        // Material properties set for this mesh alone (borrowed from its owner for the frame), and
        // their version, bumped whenever they change. A name is a shader property's, or an effect
        // group's value as `Effect.Property` (`Wind.Strength`). A mesh with any draws alone (it never
        // batches) with an instance of its own for each overridden slot; the depth, shadow and pick
        // passes use the shared material (so an overridden wind sways the visible mesh only, and an
        // effect switches on by the shared material's gate alone).
        const MaterialPropertyOverride* overrides = nullptr;
        u32 overrideCount = 0;
        u32 overrideVersion = 0;
        // entityId lives on the RenderData base (generic pick tag).
        // GPU skinning: per-bone skinning matrices for a skinned mesh (borrowed for the frame, from an
        // AnimationPlayer). When non-null and the mesh IsSkinned(), the renderer uploads them to its bone
        // pool and draws the SKINNED permutation; otherwise the mesh draws static (bind pose).
        const Float4x4* boneMatrices = nullptr;
        const Float4x4* prevBoneMatrices =
            nullptr; // previous-frame skinning matrices (motion vectors); null reuses current
        u32 boneCount = 0;
        // LOD knobs, copied from the component at extraction. Selection is
        // per view in the renderer (extraction is one snapshot shared by every view), from
        // projected-sphere coverage of worldCenter and worldRadius against the mesh's chain.
        f32 lodBias = 0.0f;
        i32 forceLod = -1; // -1 is automatic
        // Discriminator: when true this is a `MultiMeshRenderData` (an instanced set drawn as one
        // item). The Resolve loop only sees `MeshRenderData*`, so it branches on this flag and downcasts.
        // Regular meshes leave it false and are unaffected.
        bool multiMesh = false;
    };
    static_assert(std::is_trivially_destructible_v<MeshRenderData>);

    // How a skinned MultiMesh instance picks its pose out of the M shared palettes. Hashed decorrelates
    // from any spatial layout (splitmix-scattered), the natural default for a crowd of independent agents;
    // Sequential (i % M) makes a phase gradient or wave; Explicit lets the caller supply a per-instance pose
    // index (needed for layout-aware looks the renderer can't compute from the flat index i: columns,
    // spatial clusters, gameplay state). Explicit with a null or mismatched index array falls back to Hashed.
    // Unison puts every instance on a clip at the same phase: a crowd that moves in step (dancers on
    // a beat, a column marching).
    enum class PoseAssignment : u8
    {
        Hashed,
        Sequential,
        Explicit,
        Unison
    };

    // An instanced-mesh set (a "MultiMesh"): one shared mesh and material drawn `instanceCount` times,
    // whose per-instance transforms live in a persistent GPU buffer owned by the renderer (keyed by
    // `key`), uploaded only when `version` changes. Extraction emits one of these per
    // InstancedMeshComponent (not one per instance), so per-frame CPU cost is O(1) in the instance
    // count. The base `MeshRenderData` carries the shared mesh, material and color and the merged
    // bounds (worldCenter, worldRadius), so the set culls as one sphere; `world` is unused (each
    // instance has its own transform in `transforms`).
    struct MultiMeshRenderData : MeshRenderData
    {
        u64 key = 0; // stable per-component id, keying the renderer's persistent buffer slot
        const Float4x4* transforms =
            nullptr; // borrowed per-instance world transforms (instanceCount entries), valid this frame
        const Color* tints =
            nullptr; // optional borrowed per-instance tint (instanceCount entries); null uses `color`
        u32 instanceCount = 0;
        // Instances to upload when more than instanceCount are borrowed (0 means instanceCount): a
        // distance-faded set uploads its whole list once and draws a prefix that moves with the
        // camera without re-uploading (`transforms` and `tints` then hold uploadCount entries).
        u32 uploadCount = 0;
        // Per-instance distance fade (metres; fadeEnd <= 0 for none): full density inside fadeStart,
        // a smoothstep to nothing at fadeEnd, evaluated per instance in the vertex shaders against
        // its own camera distance and its rank (its position in the set's random order, which the
        // renderer writes into its tint alpha at upload). The draw-count prefix stays the coarse
        // bound; this is what removes the seam between chunks (a vegetation layer's window).
        f32 fadeStart = 0.0f;
        f32 fadeEnd = 0.0f;
        u32 version = 0; // bumps when `transforms` change; the renderer re-uploads only on a change
        // GPU-skinned crowds: a shared pose pool of `poseCount` palettes (each `boneCount` matrices),
        // borrowed for the frame. When non-null the set draws skinned, each instance using the pose
        // poseAssignment picks for it (SelectPose).
        const Float4x4* posePool = nullptr;
        const Float4x4* prevPosePool =
            nullptr;       // last frame's palettes (per-bone motion vectors); null reuses current
        u32 poseCount = 0; // M unique phase buckets
        u32 boneCount = 0; // bones per palette
        // Pose-selection policy and the optional per-instance index array it consumes when Explicit (borrowed,
        // instanceCount entries). poseIndices is null unless poseAssignment==Explicit and the caller's array
        // was present and correctly sized; the renderer then falls back to Hashed.
        PoseAssignment poseAssignment = PoseAssignment::Hashed;
        const u32* poseIndices = nullptr;
        // A pool of several clips: `posesPerClip` palettes per clip, a block per clip in order
        // (0 means one clip: the whole pool), and each instance's clip index (borrowed, instanceCount
        // entries; null puts every instance on clip 0). The clip picks the block; the policy the phase.
        const u32* instanceClips = nullptr;
        u32 posesPerClip = 0;
    };
    static_assert(std::is_trivially_destructible_v<MultiMeshRenderData>);

    // The renderer's per-instance pose pick out of the M shared palettes (used in the skinned-MultiMesh
    // offsets fill), factored out pure so it can be unit-tested without a GPU. `i` is the per-SET instance
    // index. Explicit consumes the caller's array (null means the array was absent or mismatched, so it
    // falls back to Hashed). Sequential is i % M (a phase gradient, meaningful only when a set's instances
    // are laid out in the order you want the gradient). Hashed is splitmix-scattered, decorrelated from any
    // layout.
    // poseCount == 0 returns 0. All branches index within [0, poseCount).
    //
    // A pool of several clips (`posesPerClip` > 0, a block of that many palettes per clip): `clip`
    // picks the block (wrapped to the clips the pool holds) and the policy picks the phase inside it,
    // so Explicit indices are phases within the clip. Unison is phase 0 for every instance: a clip's
    // instances move in step. One clip (posesPerClip 0, or the whole pool) selects across the whole pool.
    [[nodiscard]] inline u32 SelectPose(PoseAssignment policy, u32 i, u32 poseCount,
                                        const u32* explicitIndices, u32 clip = 0,
                                        u32 posesPerClip = 0) noexcept
    {
        if (poseCount == 0)
        {
            return 0;
        }
        const u32 m = (posesPerClip > 0 && posesPerClip <= poseCount) ? posesPerClip : poseCount;
        const u32 clips = poseCount / m;
        const u32 block = (clips > 0 ? clip % clips : 0) * m;
        if (policy == PoseAssignment::Explicit && explicitIndices != nullptr)
        {
            return block + explicitIndices[i] % m;
        }
        if (policy == PoseAssignment::Sequential)
        {
            return block + i % m;
        }
        if (policy == PoseAssignment::Unison)
        {
            return block;
        }
        return block + static_cast<u32>(HashInteger(i) % m);
    }

    // Crowd-authoring helpers for building an Explicit per-instance pose-index array from a character's grid
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
    // rendererId with the sprite renderer's id. `texture` is borrowed (the app or resource keeps it alive).
    struct SpriteRenderData : RenderData
    {
        Float2 size = Float2{1.0f, 1.0f};               // world-unit width and height
        Float4 uvRect = Float4{0.0f, 0.0f, 1.0f, 1.0f}; // atlas sub-rect (u, v, w, h)
        Color tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
        u32 orientation =
            0; // 0 camera-facing, 1 about world Y, 2 world XY, 3 entity-oriented
        bool additive = false;    // blend: false for alpha over, true for additive
        bool postTonemap = false; // WorldUI category: after tonemap, authored colors intact
        Float3 axisRight = Float3{1.0f, 0.0f, 0.0f}; // EntityOriented: world right axis
        Float3 axisUp = Float3{0.0f, 1.0f, 0.0f};    // EntityOriented: world up axis
        rhi::TextureView* texture = nullptr;
    };
    static_assert(std::is_trivially_destructible_v<SpriteRenderData>);

    // One screen-space projected decal. It is not a `RenderData` or drawable (it isn't dispatched through
    // the Renderer path); it's a snapshot list consumed by the standalone DecalPass, like the light list.
    // `world` is the decal's oriented unit box (its scale is the box size); it projects along local +Z.
    // `texture` is borrowed (the app or resource keeps it alive).
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

    // One light, packed for a GPU storage buffer (std430, 64 bytes, 4x float4). A shading input,
    // not a drawable: extracted into the ExtractedScene's light list, uploaded to a storage buffer,
    // and consumed by the forward shading loop. Directional: dir is the light direction; point and
    // spot: position and range (and spot cone cosines). type: 0 Directional, 1 Point, 2 Spot.
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
        // shadowIndex: -1 means this light casts no shadow. Otherwise a directional light's value
        // (any >= 0) selects the cascades, and a spot or point light's indexes its GpuLocalShadow
        // entries (scene-relative).
        f32 shadowIndex = -1.0f;
        // How dark the light's shadow gets: 1 is full, 0 none (the shader lerps toward lit).
        f32 shadowStrength = 1.0f;
    };
    static_assert(sizeof(GpuLight) == 64);

    // The share of a light's intensity that reaches `point`, before any shadow: the forward
    // shader's range falloff and spot cone (lighting.hlsli Attenuation and SpotAttenuation, kept
    // the same), on the CPU, for a game asking how lit a place is. 1 for a directional light.
    [[nodiscard]] inline f32 LightFalloff(const GpuLight& light, Float3 point) noexcept
    {
        if (light.type < 0.5f)
        {
            return 1.0f;
        }
        const Float3 toLight = light.positionWS - point;
        const f32 dist = Length(toLight);
        f32 falloff = 1.0f;
        if (light.range > 0.0f)
        {
            const f32 d = dist / light.range;
            const f32 d2 = d * d;
            const f32 window = Clamp(1.0f - d2 * d2, 0.0f, 1.0f);
            falloff = (window * window) / (dist * dist + 1e-4f);
        }
        if (light.type > 1.5f)
        {
            const Float3 l = toLight / Max(dist, 1e-4f);
            const f32 cosAngle = -Dot(l, light.directionWS);
            falloff *= Clamp((cosAngle - light.outerCos) / (light.innerCos - light.outerCos + 1e-4f),
                             0.0f, 1.0f);
        }
        return falloff;
    }

    /// ShadowBiasDefaults::kDepthBias, at namespace scope for another library to read (MSVC does not
    /// export a class's static constexpr member from a shared library).
    inline constexpr f32 kShadowBiasDefaultsDepthBias = 0.0009f;

    /// ShadowBiasDefaults::kLocalDepthBias, at namespace scope for another library to read (MSVC does not
    /// export a class's static constexpr member from a shared library).
    inline constexpr f32 kShadowBiasDefaultsLocalDepthBias = 0.0015f;

    /// ShadowBiasDefaults::kNormalBias, at namespace scope for another library to read (MSVC does not
    /// export a class's static constexpr member from a shared library).
    inline constexpr f32 kShadowBiasDefaultsNormalBias = 0.02f;

    // The shadow biases a light starts with, in one place (the light component and every renderer
    // read these). The normal offset is in shadow texels: the shader pushes a receiver along its
    // normal by this many texels of its cascade (or atlas tile), more as the surface turns from the
    // light. It stays small: the casters' slope-scaled hardware bias handles acne (a wall the sun
    // grazes at N.L 0.17 reads clean at 0.02), and a large offset parts a shadow from its caster.
    // The depth bias is the receiver's compare bias in NDC depth (toward the light, per depth.hlsli).
    struct ShadowBiasDefaults
    {
        static constexpr f32 kNormalBias = kShadowBiasDefaultsNormalBias;
        static constexpr f32 kDepthBias = kShadowBiasDefaultsDepthBias;      // directional (cascades)
        static constexpr f32 kLocalDepthBias = kShadowBiasDefaultsLocalDepthBias; // spot / point (atlas)
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

    // The active directional shadow caster for a scene (the extraction output): the light direction,
    // whether one exists, and its biases. The cascade matrices are derived later, in RenderFrame,
    // since CSM fitting needs the camera. valid == false means no shadow this frame.
    struct DirectionalShadow
    {
        Float3 direction = Float3{0, -1, 0};
        bool valid = false;
        f32 normalBias = ShadowBiasDefaults::kNormalBias; // texels
        f32 depthBias = ShadowBiasDefaults::kDepthBias;
        f32 strength = 1.0f;
    };

    /// ShadowCascades::kCount, at namespace scope for another library to read (MSVC does not export a
    /// class's static constexpr member from a shared library).
    inline constexpr u32 kShadowCascadesCount = 4;

    // Cascaded shadow map data for the directional caster, computed per frame from a view's frustum
    // and the light direction (each view fits its own). kCount cascades, each a world-to-light-clip
    // matrix, the view-space depth where it ends (cascade selection) and the world size of one shadow
    // texel (for normal-offset bias).
    struct ShadowCascades
    {
        static constexpr u32 kCount = kShadowCascadesCount;
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
        Float4x4 viewProj = Float4x4::Identity();   // world to light clip (perspective)
        Float4 atlasScaleBias = Float4{1, 1, 0, 0}; // xy uv scale, zw uv offset (tile in atlas)
        f32 depthBias = ShadowBiasDefaults::kLocalDepthBias; // depth-compare bias (NDC units; the
                                                             // shader applies it toward the light)
        f32 atlasSelect = 0.0f; // atlas array layer: 0 realtime, 1 static
        // The light's normal offset as world units per unit of distance from the light: its texels
        // times the tile's texel size at distance 1 (the shader multiplies by the distance).
        f32 normalBiasPerDistance = 0.0f;
        f32 pad2 = 0.0f;
    };
    static_assert(sizeof(GpuLocalShadow) == 96);

    // A spot or point light that should cast a shadow (the extraction output). The ShadowSystem assigns
    // it atlas tiles and builds its perspective view-projections at frame time (when the atlas layout is
    // known), then patches the source light's shadowIndex to point at the built entry. type matches
    // GpuLight (1 point, 2 spot); point lights expand to 6 cube faces.
    struct LocalShadowCaster
    {
        u32 type = 2; // 1 point, 2 spot
        Float3 positionWS = Float3{0, 0, 0};
        Float3 directionWS = Float3{0, -1, 0};
        f32 range = 10.0f;     // perspective far plane
        f32 outerAngle = 0.6f; // spot cone half-angle (radians); fov = 2 * outerAngle
        bool isStatic = false; // Static update mode: drawn into the cached static atlas layer
        f32 normalBias = ShadowBiasDefaults::kNormalBias; // texels of its atlas tile
        f32 depthBias = ShadowBiasDefaults::kLocalDepthBias;
    };

    // Atlas tile budget for local (spot and point) shadows per frame. A spot consumes 1 tile, a point 6
    // (cube faces). Extraction sets each caster's shadowIndex to its base tile (0-based, into the
    // GpuLocalShadow buffer) and caps total tiles here; the ShadowSystem's atlas must hold this many.
    inline constexpr u32 kMaxLocalShadowTiles = 16;
    // Frame-global cap on GpuLocalShadow entries (all scenes concatenated). The renderer's
    // local-shadow ring is sized to this; the pipeline writes degenerate entries instead of exceeding it.
    inline constexpr u32 kMaxLocalShadowEntries = 64;

    // How a reflection probe's captured cubemap refreshes (defined in the snapshot layer, reused by the
    // ReflectionProbeComponent). Static captures once with a full prefilter and caches until the probe
    // moves or is invalidated. Realtime re-captures on a round-robin cadence (cheap prefilter). Manual
    // captures only on request.
    enum class ProbeUpdateMode : u32
    {
        Static = 0,
        Realtime = 1,
        Manual = 2
    };

    // A reflection probe (extraction output). The ReflectionProbeSystem captures the scene into a cubemap
    // from `center`, prefilters it, and the forward samples it with parallax correction against the box
    // [center - halfExtents, center + halfExtents] (world-axis-aligned box, not an OBB). `blendDistance`
    // softens the influence toward the box edge so overlapping probes blend without a seam. `key` is a
    // stable per-entity tag (PackEntity) so the system maps a probe to a persistent GPU slot across frames.
    struct ReflectionProbe
    {
        u64 key = 0;
        Float3 center = Float3{0, 0, 0}; // capture center (world)
        Float3 halfExtents =
            Float3{5, 5, 5};      // box half-extents (world; influence and parallax proxy)
        f32 blendDistance = 1.0f; // soft falloff width inward from the box edge
        f32 intensity = 1.0f;     // reflection multiplier
        u32 resolution = 128;     // captured cube face size
        u32 priority = 0;         // tie-break when volumes overlap (higher wins)
        ProbeUpdateMode update = ProbeUpdateMode::Static;
        bool parallax = true; // box-project the reflection ray (rather than an infinite env)
        // Counts capture requests (a Manual probe's capture()); a value the system has not seen
        // recaptures the probe once, in any update mode.
        u32 captureRequest = 0;
    };

    // Reflection-probe budget per frame (bounds the prefiltered cube-array slices and the metadata buffer).
    inline constexpr u32 kMaxReflectionProbes = 16;

    // A per-view draw entry: a sort key (computed against the view's camera) and the shared
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
    // (material and PSO identity) clusters same-pipeline draws to minimize state changes; `depth`
    // orders within that: front to back for opaque (early-Z), back to front for transparent
    // (correct blending). The producer inverts depth for transparent before packing.

    inline constexpr u32 kSortDepthBits = 24;
    inline constexpr u32 kSortStateBits = 24;

    // Fold two borrowed resource pointers into a batch-clustering key for the sort.
    // Pointer-derived identity is fine for a transient per-frame key: the renderer re-checks exact
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
    // until Reset() (which keeps the chunks for reuse next frame, so there is no per-frame churn).
    // Only trivially destructible types (RenderData subclasses) are allocated, so Reset() reclaims
    // without running destructors. RenderContext holds one per job-system slot for parallel
    // extraction.
    class FrameArena
    {
    public:
        // The allocator (required; the owner decides) backs the arena's chunks.
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
        static constexpr usize kChunkAlign = 16; // >= any RenderData alignment (Float4x4 is 16)

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

    // How the scene's environment radiance (sky and IBL source) is produced. The canonical enum lives
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
    // the snapshot layer as primitives (no authoring or pass enums) so it embeds in ViewSettings without
    // pulling in the subsystem; the render subsystem resolves a scene's authored PostProcessSettings
    // (or its global override) into this per RenderScene, and the compose passes read it per
    // view. Defaults match the RenderSubsystem's globals, so a default view is unchanged.
    struct ViewPostConfig
    {
        f32 exposure = 1.0f;    // linear multiplier (a scene's EV is resolved via exp2 upstream)
        bool agxTonemap = true; // true for the AgX operator, false for clamp
        bool bloomEnabled = true;
        f32 bloomThreshold = 1.0f;
        f32 bloomKnee = 0.6f;
        f32 bloomIntensity = 0.05f;
        u32 aoMode = 0; // AoMode as u32 (0 Off, 1 GTAO, 2 SSAO), which keeps :data enum-free
        f32 aoStrength = 0.6f;
        f32 aoRadius = 0.5f;
        f32 aoIntensity = 1.0f;
        // Anti-aliasing (TAA and FXAA are mutually exclusive). taaMotionScale stays frame-global.
        bool taaEnabled = false;
        f32 taaBlend = 0.97f;
        f32 taaGamma = 1.25f;
        bool fxaaEnabled = false;
        f32 fxaaSubpixel = 0.75f;
        // Screen-space reflections (enable and intensity per view; the detailed SsrPass::Params config
        // stays frame-global).
        bool ssrEnabled = false;
        f32 ssrIntensity = 1.0f;
        // Screen-space GI: one additive diffuse bounce.
        bool ssgiEnabled = false;
        f32 ssgiIntensity = 1.0f;
        // Auto-exposure (eye adaptation): the tonemap multiplies exposure by
        // clamp(key / adaptedLuminance, autoMin..autoMax) when enabled.
        bool autoExposure = false;
        f32 autoExposureKey = 0.18f;
        f32 autoExposureSpeed = 2.0f;        // adaptation rate (1/s)
        f32 autoExposureMin = 0.0625f;       // clamp as linear multipliers (exp2 of the EV window)
        f32 autoExposureMax = 16.0f;
        // Display-referred color grading via a strip LUT (applied after the tonemap operator).
        // Resolved product view and uid (change detection is never by pointer); null means no grading.
        rhi::TextureView* gradingLut = nullptr;
        u64 gradingLutUid = 0;
        f32 gradingLutSize = 0.0f; // slice count (16 for a 256x16 strip); 0 is off
        f32 gradingIntensity = 1.0f;
        // Resolved by the subsystem: whether this view needs motion vectors (taaEnabled or a temporal SSR
        // pass). Read at forward-pass execute through the bound view, so it must be resolved here rather
        // than recomputed from frame-global state.
        bool needsMotion = false;
        // Scene-pass MSAA sample count for this view (1 is off, or 2 or 4). Capability-clamped by the
        // subsystem before it lands here, so this is the count the pipeline actually renders at.
        // 1 keeps the whole single-sample path.
        u8 msaaSamples = 1;
    };

    // The per-frame environment snapshot driving IBL and sky. Plain types (colors as Float3) so the
    // snapshot layer carries no authoring dependency.
    struct SkySnapshot
    {
        SkyMode mode = SkyMode::Procedural;
        f32 intensity = 1.0f; // env radiance master: baked into the cube, so it scales the background and IBL
        // Display-only multiplier on the visible sky (backdrop and probe reflections of it); not baked
        // into the env cube, so it never touches the IBL diffuse and specular lighting.
        f32 backgroundIntensity = 1.0f;
        f32 rotation = 0.0f; // yaw (radians) for HDR and cubemap
        // Resolved sky-texture product (HDREquirect and Cubemap modes; null for the programmatic
        // pixel path or none). Change detection keys on `textureUid`, never the pointer
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
        // Render-time dimmers on the sky's lighting (never the visible sky): SH9 diffuse
        // irradiance and prefiltered specular, applied in the forward's IBL block.
        f32 iblDiffuseIntensity = 1.0f;
        f32 iblSpecularIntensity = 1.0f;
    };

    // ---- extracted scene -------------------------------------------------------------------
    //
    // The per-scene, once-per-frame, immutable snapshot pushed to the renderer: world-space
    // render data for one scene. Views of the same scene share it read-only (N cameras, one
    // extraction). Carries renderables, lights and environment.
    class ExtractedScene
    {
    public:
        // The allocator (required; the owner decides) backs the arena and item lists.
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
        // data that must be self-contained: the snapshot is read at record time (RenderFrame::End),
        // after arbitrary scene and manager mutations, so borrowed pointers into producer-owned
        // storage would dangle after a mid-frame rebuild. Empty or failed allocations return an
        // empty span; producers treat that as "skip this item".
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
        // snapshot's use; it lives, say, in a RenderContext per-worker arena owned by the producer).
        // Used by parallel extraction: workers fill their own arenas, then the merge adopts the
        // pointers here single-threaded.
        void AddExternal(RenderData* data);

        // Add a light to the snapshot (shading input, not a drawable).
        void AddLight(const GpuLight& light) { m_lights.PushBack(light); }

        // Register a spot or point light as a shadow caster (the ShadowSystem builds its atlas tile and matrix
        // at frame time). `lightIndex` must equal the light's position in the list (the index AddLight
        // assigns) so its shadowIndex can be patched once the atlas slot is known.
        void AddLocalShadowCaster(const LocalShadowCaster& c) { m_localCasters.PushBack(c); }
        [[nodiscard]] Span<const LocalShadowCaster> LocalShadowCasters() const noexcept;

        // Screen-space decals (consumed by the standalone DecalPass, not the Renderer dispatch).
        void AddDecal(const DecalInstance& d) { m_decals.PushBack(d); }
        [[nodiscard]] Span<const DecalInstance> Decals() const noexcept;

        // Reflection probes (consumed by the ReflectionProbeSystem: capture, prefilter, froxel assignment).
        void AddReflectionProbe(const ReflectionProbe& p) { m_probes.PushBack(p); }
        [[nodiscard]] Span<const ReflectionProbe> ReflectionProbes() const noexcept;

        // The scene's environment ambient (the flat indirect term used when IBL is off). Premultiplied
        // color × intensity, applied as `albedo * ambient` in the forward shader.
        void SetAmbient(const Float3& ambient) noexcept { m_ambient = ambient; }
        [[nodiscard]] const Float3& Ambient() const noexcept { return m_ambient; }

        // The scene's sky and IBL environment settings for this frame.
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

        // The world position of the first view that renders this snapshot, set by the render
        // subsystem before the scene's providers extract (a producer that thins by distance,
        // such as vegetation's fade prefix, reads it). One snapshot serves every view of the
        // scene, so a second view sees the first view's thinning; prefixes are not per view.
        // HasViewOrigin() is false for a headless extraction (nothing is thinned).
        void SetViewOrigin(const Float3& origin) noexcept
        {
            m_viewOrigin = origin;
            m_hasViewOrigin = true;
        }
        [[nodiscard]] const Float3& ViewOrigin() const noexcept { return m_viewOrigin; }
        [[nodiscard]] bool HasViewOrigin() const noexcept { return m_hasViewOrigin; }

        // The scene's clock (this and last frame's seconds), stamped by the scene's environment
        // extraction from a clock that accumulates the scene's own dt, so the context, group and
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

        // The scene this snapshot was extracted from (scene::Scene::Serial; 0 when none is stamped, as for a
        // probe). Snapshots are pooled and reused across scenes, so this, not the snapshot's
        // address, tells a view's history passes that the scene changed under them.
        void SetSceneSerial(u64 serial) noexcept { m_sceneSerial = serial; }
        [[nodiscard]] u64 SceneSerial() const noexcept { return m_sceneSerial; }

        // Reset for a new frame: drop the item and light lists, rewind the (internal) arena.
        void Reset() noexcept;

        [[nodiscard]] Span<RenderData* const> Items() const noexcept;
        [[nodiscard]] Span<const GpuLight> Lights() const noexcept;
        [[nodiscard]] usize Size() const noexcept { return m_items.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_items.IsEmpty(); }

    private:
        FrameArena m_arena;
        Array<RenderData*> m_items;
        Array<GpuLight> m_lights;
        Array<LocalShadowCaster> m_localCasters; // spot and point shadow casters
        Array<DecalInstance> m_decals;           // screen-space decals (consumed by DecalPass)
        Array<ReflectionProbe> m_probes; // reflection probes (consumed by ReflectionProbeSystem)
        Float3 m_ambient = Float3{0.03f, 0.03f, 0.03f}; // default dim ambient
        SkySnapshot m_sky;                              // sky and IBL environment for this frame
        DirectionalShadow m_shadow;                     // active directional shadow caster
        SceneShadowSettings m_shadowSettings;           // the scene's cascade reach
        Float3 m_viewOrigin = Float3{0.0f, 0.0f, 0.0f}; // first view's camera position (see above)
        bool m_hasViewOrigin = false;
        f32 m_timeSeconds = 0.0f;     // the scene clock (see SetTime)
        f32 m_prevTimeSeconds = 0.0f;
        bool m_hasTime = false;
        u64 m_sceneSerial = 0; // see SetSceneSerial
    };

    // Extension point (scene-agnostic): a downstream system, such as a particle component manager,
    // implements this to contribute render data into the frame snapshot.
    // It knows nothing about scenes; the implementer already holds its own data (it's typically a scene
    // system that stored its scene). RenderSubsystem registers providers per scene (registration knows
    // the scene; this interface does not) and calls the current scene's providers during extraction.
    class IRenderDataProvider
    {
    public:
        virtual ~IRenderDataProvider() = default;
        virtual void ExtractRenderData(ExtractedScene& snapshot) = 0;
    };

    // ---- radix sort ------------------------------------------------------------------------
    //
    // LSD radix sort of DrawItems by their 64-bit key, ascending: O(N), stable, 8 passes of
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
        // 8 passes (an even count), so the result ends back in `items`; nothing to copy.
    }

} // namespace foundation::render
