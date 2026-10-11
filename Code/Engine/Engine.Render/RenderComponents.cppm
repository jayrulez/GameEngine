// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Render - the `:components` partition.
///
/// The render-facing scene components + their managers - the scene-coupled side of the
/// renderer (foundation.render itself stays scene-agnostic). A MeshComponent references a
/// mesh + material to draw at its entity's transform; a CameraComponent describes a
/// view frustum (its view comes from the entity's world transform). The RenderSubsystem
/// declares these managers for the scene composition; extraction (:extract) reads
/// them into a engine::render::ExtractedView that gets pushed to the renderer.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT (the profile products)

export module engine.render:components;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.rhi; // rhi::TextureView (a SpriteComponent references a texture to draw)
import foundation.texture.resource; // texture::Texture (cooked product behind sprite/decal texture refs)
import foundation.render; // SkySnapshot/SkyMode (snapshot layer; render.subsystem depends on render)
import foundation.script.facades; // script::Entity/Scene + CurrentRunResources (the SceneRender handle)

using namespace foundation::core;
using namespace foundation;
using namespace foundation::render;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace rhi = foundation::rhi;
namespace texture = foundation::texture;

export namespace engine::render
{
    // Foundation alias (sibling engine::scene would otherwise shadow foundation::scene).
    namespace scene = foundation::scene;

    // What to draw at an entity: a mesh + the material to draw it with. Resource refs
    // (resource::Ref): a serialized Guid resolved through the ResourceManager's proxy handles
    // (hot reload swaps the product behind every holder), or a direct RefPtr for code-created
    // meshes (samples/procedural - the direct object wins and is never serialized). `color` is a
    // per-instance tint (multiplied into the shaded color) - distinct per entity even when
    // many share one mesh + material, so it rides the per-instance data path.
    struct MeshComponent
    {
        foundation::resource::Ref<geometry::StaticMesh> mesh;
        // THE material list, indexed by SubMesh::materialIndex. Slot 0 doubles as the whole-mesh
        // material: single-material meshes hold ONE entry, and a submesh whose index is out of
        // range (or whose slot is unresolved) falls back to slot 0. Serialized identity lives in
        // `materials`; `materialCache` is the raw-pointer view EXTRACTION refreshes from the ref
        // proxies EVERY frame - late cooks and hot reloads heal live (the previous design
        // snapshotted RefPtrs once at resolve, pinning pre-cook nulls until a page reopen), and
        // the renderer stays resource-agnostic.
        Array<foundation::resource::Ref<materials::Material>> materials;
        Array<RefPtr<materials::Material>> materialCache; // runtime-only; refreshed at extract
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f};
        bool visible = true;
        // How far the mesh is faded out, 0 (solid) to 1 (gone), drawn as a screen-door dither: a
        // cutaway wall between the camera and the player. Only its camera pixels thin out; it still
        // casts its whole shadow, so the room behind a cut-away wall stays as dark as it was.
        f32 fade = 0.0f;
        // Material properties set for this mesh alone (runtime, not saved): its material in a slot
        // drawn with one of its own carrying them (a glow, a tint, a flash), the shared material
        // untouched. `materialOverrideVersion` changes with them, so the renderer re-applies.
        Array<foundation::render::MaterialPropertyOverride> materialOverrides;
        u32 materialOverrideVersion = 0;
        // Set (or replace) a property's value in `slot`; `size` is 4 for a float, 16 for a Float4.
        void SetMaterialProperty(u32 slot, StringView name, Float4 value, u32 size)
        {
            ++materialOverrideVersion;
            for (foundation::render::MaterialPropertyOverride& o : materialOverrides)
            {
                if (o.slot == slot && o.name == name)
                {
                    o.value = value;
                    o.size = size;
                    return;
                }
            }
            foundation::render::MaterialPropertyOverride o;
            o.slot = slot;
            o.size = size;
            o.value = value;
            o.name = String(name);
            materialOverrides.PushBack(Move(o));
        }
        // Back to the material's own value; false if it was not set.
        bool ClearMaterialProperty(u32 slot, StringView name)
        {
            for (usize i = 0; i < materialOverrides.Size(); ++i)
            {
                if (materialOverrides[i].slot == slot && materialOverrides[i].name == name)
                {
                    materialOverrides.RemoveAt(i);
                    ++materialOverrideVersion;
                    return true;
                }
            }
            return false;
        }
        // Slot-0 conveniences for runtime code (samples/spawners) - refs and raw objects both fit
        // (resource::Ref adopts direct pointers).
        void SetMaterial(const RefPtr<materials::Material>& m)
        {
            materials.Clear();
            materials.PushBack(foundation::resource::Ref<materials::Material>(m));
        }
        void SetMaterials(const Array<RefPtr<materials::Material>>& list)
        {
            materials.Clear();
            for (const RefPtr<materials::Material>& m : list)
            {
                materials.PushBack(foundation::resource::Ref<materials::Material>(m));
            }
        }

        // GPU skinning: per-bone skinning matrices for a skinned mesh, supplied per frame by the owner
        // (e.g. an AnimationPlayer's GetSkinningMatrices()). Borrowed - valid for the frame it's set;
        // null => the mesh draws static (bind pose). Extraction copies the pointer into MeshRenderData.
        const Float4x4* boneMatrices = nullptr;
        const Float4x4* prevBoneMatrices =
            nullptr; // previous-frame matrices (motion vectors); null => reuse current
        u32 boneCount = 0;

        // LOD knobs (payload v4). lodBias > 0 switches coarser sooner (each unit
        // halves the effective screen coverage; negative holds detail longer). forceLod
        // pins one level for debug/cinematics (-1 = automatic; clamped to the chain).
        // Copied into MeshRenderData at extraction; selection is per view in the renderer.
        f32 lodBias = 0.0f;
        i32 forceLod = -1;
    };

    // An INSTANCED mesh ("MultiMesh"): ONE shared mesh + material drawn at N per-instance transforms that
    // live (on the GPU) in a persistent buffer owned by the renderer. Its per-frame CPU cost is O(1) in the
    // instance count - the set is extracted as ONE render item, culled as one merged AABB, and drawn once
    // per pass (depth, forward, every shadow cascade all read the same buffer). Use it for static crowds /
    // scatter (foliage, props, debris); the per-entity MeshComponent stays for genuinely dynamic objects.
    // `instances` is the CPU source of truth; every mutator bumps `version`, and the renderer re-uploads the
    // GPU buffer (and extraction recomputes the merged bounds) ONLY when the version changes.
    struct InstancedMeshComponent
    {
        foundation::resource::Ref<geometry::StaticMesh> mesh;
        foundation::resource::Ref<materials::Material> material;
        // Seed one identity instance so a freshly added component (editor workflow) renders its mesh
        // at the entity's transform immediately; authored sets replace it (SetInstances/load).
        InstancedMeshComponent() { instances.PushBack(Float4x4::Identity()); }
        // Optional per-submesh materials (multi-material meshes): indexed by SubMesh::materialIndex. When
        // non-empty each submesh draws with its own material; otherwise `material` covers the whole mesh.
        // Runtime-only (RefPtr, not serialized) like MeshComponent's: per-submesh material REFS are
        // owned by prefabs, which own the model->entity workflow.
        Array<RefPtr<materials::Material>> submeshMaterials;
        // Per-instance transforms, ENTITY-RELATIVE: instance i draws at instances[i] * entityWorld,
        // so moving the owning entity moves the whole set (extraction composes + caches the world
        // array; a set on an unmoved identity entity costs the same as before).
        Array<Float4x4> instances;
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f}; // shared tint (used when `tints` is empty)
        // Optional per-instance tint (parallel to `instances`): when its size matches, each instance uses its
        // own tint; otherwise `color` covers all. Read at upload, so set it BEFORE SetInstances (which bumps version).
        Array<Color> tints;
        bool visible = true;

        // GPU-skinned crowds: a shared POSE POOL of `poseCount` skinning palettes (each `boneCount` matrices),
        // set per frame by the InstancedSkinningComponent companion (engine.animation). When posePool is
        // non-null the set draws SKINNED, and instance i uses pose (i % poseCount) - so N animated instances
        // cost only M = poseCount palette computes, not N. Borrowed (valid for the frame it's set); null => the
        // set draws static. The mesh must be a skinned mesh (has a skin stream).
        const Float4x4* posePool = nullptr;
        const Float4x4* prevPosePool =
            nullptr;       // last frame's palettes (per-bone motion vectors); null => reuse current
        u32 poseCount = 0; // M unique phase buckets
        u32 boneCount = 0; // bones per palette

        // Pose-selection policy for the shared pool (see render::PoseAssignment). Default Hashed = each
        // instance scattered to an unrelated pose (independent-agent crowd). Set to Explicit and fill
        // `poseIndices` (parallel to `instances`, values in [0,poseCount)) for layout-aware looks the
        // renderer can't derive from the flat index - columns, spatial clusters, gameplay. Explicit with an
        // empty/mismatched array falls back to Hashed. Not a mutator (doesn't bump version - it's read each
        // frame during the offsets fill, not uploaded to the instance buffer).
        PoseAssignment poseAssignment = PoseAssignment::Hashed;
        Array<u32> poseIndices;

        // Change counter: bumped by every mutator so the renderer knows to re-upload and extraction knows to
        // recompute the merged bounds. Starts at 1 so the first extract (uploadedVersion 0) always uploads.
        u32 version = 1;
        // Composed world-space transforms (instances[i] * entityWorld), rebuilt by extraction when the
        // authored set OR the entity's world matrix changed; `composedVersion` is what the renderer
        // sees, so an entity move re-uploads the GPU buffer like any other mutation. Runtime-only.
        Array<Float4x4> worldTransforms;
        // `tints` decoded to linear for the renderer (authored colours are sRGB), rebuilt with
        // `worldTransforms` since the renderer reads tints at the same upload. Runtime-only.
        Array<Color> linearTints;
        Float4x4 composedEntityWorld = Float4x4::Identity();
        u32 composedFromVersion = 0; // authored `version` the cache was built from (0 = never)
        u32 composedVersion = 0;     // bumped on every recompose (renderer upload key)
        // Cached merged world-space bounds (center + sphere radius), recomputed at extraction when
        // `boundsVersion != composedVersion`. Lets a static set skip the O(N) bounds pass every frame.
        Float3 cachedCenter = Float3{0, 0, 0};
        f32 cachedRadius = 0.0f;
        u32 boundsVersion = 0;

        [[nodiscard]] u32 Count() const noexcept { return static_cast<u32>(instances.Size()); }

        // Replace the whole set in one shot (fast path for static content) - a single version bump.
        void SetInstances(Span<const Float4x4> xf)
        {
            instances.Resize(xf.Size());
            if (!xf.IsEmpty())
            {
                MemCopy(instances.Data(), xf.Data(), xf.Size() * sizeof(Float4x4));
            }
            ++version;
        }
        void Add(const Float4x4& m)
        {
            instances.PushBack(m);
            ++version;
        }
        void SetInstance(u32 i, const Float4x4& m)
        {
            if (i < instances.Size())
            {
                instances[i] = m;
                ++version;
            }
        }
        void Reserve(u32 n) { instances.Reserve(n); }
        void Clear()
        {
            instances.Clear();
            ++version;
        }
    };

    // How a camera maps view space to the screen.
    enum class CameraProjection : u32
    {
        Perspective = 0,  // a frustum widening with depth (fovYRadians)
        Orthographic = 1, // a box of fixed size (orthoHeight): a top-down map, an isometric view
    };

    // A camera frustum. The view transform is the inverse of the entity's world matrix;
    // these fields define the projection. `primary` marks the camera the renderer uses.
    // `clearColor` is the backdrop the view is cleared to (per-camera, like Unity/Godot);
    // defaults to the cornflower sentinel. (Clear *mode* - skybox/solid/depth-only - later.)
    struct CameraComponent
    {
        f32 fovYRadians = 1.04719755f; // 60 degrees
        f32 aspect = 16.0f / 9.0f;
        f32 nearZ = 0.1f;
        f32 farZ = 1000.0f;
        Color clearColor = Color{0.392f, 0.584f, 0.929f, 1.0f}; // cornflower sentinel
        bool primary = true;
        CameraProjection projection = CameraProjection::Perspective;
        f32 orthoHeight = 10.0f; // Orthographic: the world-space height the view spans
        // A camera with a target renders into that texture (a render texture asset) instead of
        // the screen, every `targetInterval` frames; it is never the screen camera, `primary` or
        // not. A texture made at run time is assigned straight to the Ref and is never saved.
        foundation::resource::Ref<texture::Texture> target;
        u32 targetInterval = 1;

        [[nodiscard]] bool HasTarget() const noexcept
        {
            return !target.id.IsNil() || target.Get() != nullptr;
        }
    };

    // The projection matrix of `camera` at `aspect` (width over height). The one place a camera's
    // fields become a matrix: the renderer's camera pick and the editor's camera preview both
    // build through it, so the preview frames what the game draws.
    [[nodiscard]] inline Float4x4 MakeCameraProjection(const CameraComponent& camera, f32 aspect)
    {
        if (camera.projection == CameraProjection::Orthographic)
        {
            const f32 height = Max(camera.orthoHeight, 1.0e-4f);
            return Float4x4::OrthographicRH(height * aspect, height, camera.nearZ, camera.farZ);
        }
        return Float4x4::PerspectiveFovRH(camera.fovYRadians, aspect, camera.nearZ, camera.farZ);
    }

    // A light on an entity. Directional uses the entity's forward (-Z); Point/Spot use its world
    // position (+ range). Extraction packs these into render::GpuLight shading inputs.
    enum class LightType : u32
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    // How a spot/point light's shadow updates. Realtime = re-render every
    // frame (default). Static = render into the cached atlas layer, redrawn when the LIGHT moves or
    // changes, and where a caster moves, appears or goes (a figure walking, a door swinging) within its
    // reach. Cheap for scenes that mostly stand still.
    enum class ShadowUpdateMode : u32
    {
        Realtime = 0,
        Static = 1
    };

    struct LightComponent
    {
        LightType type = LightType::Directional;
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f};
        f32 intensity = 1.0f;
        f32 range = 10.0f;     // point/spot falloff distance
        f32 innerAngle = 0.5f; // spot cone inner half-angle (radians)
        f32 outerAngle = 0.6f; // spot cone outer half-angle (radians)
        ShadowUpdateMode shadowUpdate =
            ShadowUpdateMode::Realtime; // spot/point shadow caching
        bool enabled = true;
        bool castsShadows = false; // shadow caster
        // The shadow's tuning (v1), shown while castsShadows: the depth-compare bias as a scale of the
        // light type's default (1 = default; the sun's and a local light's depths are in different
        // spaces), the normal offset in shadow texels (what keeps a wall the light grazes from
        // shadowing itself), and how dark the shadow gets (1 = full, 0 = none).
        f32 shadowDepthBiasScale = 1.0f;
        f32 shadowNormalBias = foundation::render::ShadowBiasDefaults::kNormalBias;
        f32 shadowStrength = 1.0f;
    };

    // A textured billboard on an entity - drawn at the entity's world position, sized in world units,
    // facing the camera (or world-aligned). `texture` is borrowed: the app/resource owns it and must keep
    // it alive while the component is attached. Extraction reads this into a render::SpriteRenderData.
    // How a sprite billboard orients itself (mirrors the shader's orientation mode).
    enum class SpriteOrientation : u32
    {
        CameraFacing = 0,   // full billboard - always faces the camera
        CameraFacingY = 1,  // rotates about world-Y only (trees/characters)
        WorldAligned = 2,   // fixed world XY plane (decal-like flat art)
        EntityOriented = 3, // spanned by the ENTITY's world right/up axes (world panels)
    };

    struct SpriteComponent
    {
        rhi::TextureView* texture =
            nullptr; // runtime override (samples/procedural); wins over textureAsset
        foundation::resource::Ref<texture::Texture>
            textureAsset;                 // cooked texture (editor picker/serialized)
        Float2 size = Float2{1.0f, 1.0f}; // world-unit width/height
        Float4 uvRect = Float4{0.0f, 0.0f, 1.0f,
                               1.0f}; // atlas sub-rect (u, v, w, h) - whole texture by default
        Color tint = Color{1.0f, 1.0f, 1.0f, 1.0f};
        SpriteOrientation orientation = SpriteOrientation::CameraFacing;
        bool additive = false;    // false = alpha over, true = additive (glow)
        bool postTonemap = false; // draw AFTER tonemap (world UI: authored colors intact)
        bool visible = true;
    };

    // A screen-space projected decal on an entity - sprays `texture` onto whatever surface is under its
    // oriented box. The box projects along the entity's local +Z; `size` is the box extents (x,y = the
    // footprint, z = how far along the projection axis it reaches). Orient the entity so local +Z points
    // into the surface (e.g. rotate so +Z points down to project onto a floor). `texture` is borrowed.
    struct DecalComponent
    {
        rhi::TextureView* texture =
            nullptr; // runtime override (samples/procedural); wins over textureAsset
        foundation::resource::Ref<texture::Texture>
            textureAsset; // cooked texture (editor picker/serialized)
        Float3 size = Float3{1.0f, 1.0f, 1.0f};
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f};
        f32 fadeStart = 0.0f; // angle-fade start (radians)
        f32 fadeEnd = 1.30f;  // angle-fade end (radians ~75deg)
        bool visible = true;
    };

    // A local reflection probe: captures the scene into a cubemap from the entity's world position and gives
    // parallax-corrected specular reflections to surfaces inside its box volume. The box (world-axis-aligned,
    // from `halfExtents`) is BOTH the influence volume and the parallax proxy; `blendDistance` softens the
    // influence toward the box edge so overlapping probes blend without a seam. `update` selects the capture
    // cadence (ProbeUpdateMode, from the snapshot layer). Extraction reads this into a render::ReflectionProbe.
    struct ReflectionProbeComponent
    {
        Float3 halfExtents =
            Float3{5.0f, 5.0f, 5.0f}; // box influence/proxy half-extents (world units)
        f32 blendDistance = 1.0f;     // soft falloff width inward from the box edge
        f32 intensity = 1.0f;         // reflection multiplier
        u32 resolution = 128;         // captured cube face size (64/128/256)
        u32 priority = 0;             // tie-break when volumes overlap (higher wins)
        ProbeUpdateMode update = ProbeUpdateMode::Static;
        bool parallax = true; // box-project the reflection ray (vs infinite env)
        bool enabled = true;
    };

    // MeshComponent persists (scene round-trip): refs serialize their Guids; the direct
    // pointers and per-frame skinning state never touch disk.
    inline void Serialize(ISerializer& ar, MeshComponent& c)
    {
        foundation::core::Serialize(ar, "mesh", c.mesh);
        foundation::core::Serialize(ar, "materials", c.materials);
        foundation::core::Serialize(ar, "color", c.color);
        foundation::core::Serialize(ar, "visible", c.visible);
        foundation::core::Serialize(ar, "fade", c.fade);
        foundation::core::Serialize(ar, "lodBias", c.lodBias);
        foundation::core::Serialize(ar, "forceLod", c.forceLod);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager, MeshComponent& c)
    {
        c.mesh.Bind(manager);
        for (foundation::resource::Ref<materials::Material>& r : c.materials)
        {
            r.Bind(manager);
        }
        // materialCache is refreshed at EXTRACT time (per frame, through the proxies) - resolve
        // only attaches the bindings.
    }

    // Measures its entities for the scene (ISceneEntityBounds): a mesh's bounds through the
    // entity's world matrix, once the mesh is loaded. Answers rays against its meshes' triangles
    // (ISceneSurfaceQuery; a skinned mesh in its bind pose, the pose its CPU vertices hold).
    class MeshComponentManager final : public scene::SerializableComponentManager<MeshComponent>,
                                       public scene::ISceneEntityBounds,
                                       public scene::ISceneSurfaceQuery
    {
    public:
        MeshComponentManager() : scene::SerializableComponentManager<MeshComponent>(u8"mesh") {}
        [[nodiscard]] scene::ISceneEntityBounds* AsEntityBounds() noexcept override { return this; }
        [[nodiscard]] bool EntityBounds(scene::Scene& scene, scene::EntityHandle entity, AABB& out) override;
        [[nodiscard]] scene::ISceneSurfaceQuery* AsSurfaceQuery() noexcept override { return this; }
        [[nodiscard]] bool RaycastSurface(scene::Scene& scene, Float3 origin, Float3 direction, f32 maxDistance,
                                          const Function<bool(scene::EntityHandle)>& accept,
                                          scene::SceneSurfaceHit& out) override;
    };
    // Data-only components PERSIST (scene round-trip + full destroy-undo restore - a destroyed
    // entity's components are snapshotted through serializable managers only). Sprite/Decal/
    // InstancedMesh still hold raw GPU pointers and stay runtime-only until they move to refs.
    inline void Serialize(ISerializer& ar, LightComponent& c)
    {
        u8 type = static_cast<u8>(c.type);
        u8 shadowUpdate = static_cast<u8>(c.shadowUpdate);
        foundation::core::Serialize(ar, "type", type);
        foundation::core::Serialize(ar, "color", c.color);
        foundation::core::Serialize(ar, "intensity", c.intensity);
        foundation::core::Serialize(ar, "range", c.range);
        foundation::core::Serialize(ar, "innerAngle", c.innerAngle);
        foundation::core::Serialize(ar, "outerAngle", c.outerAngle);
        foundation::core::Serialize(ar, "shadowUpdate", shadowUpdate);
        foundation::core::Serialize(ar, "enabled", c.enabled);
        foundation::core::Serialize(ar, "castsShadows", c.castsShadows);
        foundation::core::Serialize(ar, "shadowDepthBiasScale", c.shadowDepthBiasScale);
        foundation::core::Serialize(ar, "shadowNormalBias", c.shadowNormalBias);
        foundation::core::Serialize(ar, "shadowStrength", c.shadowStrength);
        c.type = static_cast<LightType>(type);
        c.shadowUpdate = static_cast<ShadowUpdateMode>(shadowUpdate);
    }

    inline void Serialize(ISerializer& ar, CameraComponent& c)
    {
        foundation::core::Serialize(ar, "fovYRadians", c.fovYRadians);
        foundation::core::Serialize(ar, "aspect", c.aspect);
        foundation::core::Serialize(ar, "nearZ", c.nearZ);
        foundation::core::Serialize(ar, "farZ", c.farZ);
        foundation::core::Serialize(ar, "clearColor", c.clearColor);
        foundation::core::Serialize(ar, "primary", c.primary);
        u8 projection = static_cast<u8>(c.projection);
        foundation::core::Serialize(ar, "projection", projection);
        foundation::core::Serialize(ar, "orthoHeight", c.orthoHeight);
        foundation::core::Serialize(ar, "target", c.target);
        foundation::core::Serialize(ar, "targetInterval", c.targetInterval);
        c.projection = static_cast<CameraProjection>(projection);
    }
    inline void ResolveResources(foundation::resource::ResourceManager& manager, CameraComponent& c)
    {
        c.target.Bind(manager);
    }

    inline void Serialize(ISerializer& ar, ReflectionProbeComponent& c)
    {
        u8 update = static_cast<u8>(c.update);
        foundation::core::Serialize(ar, "halfExtents", c.halfExtents);
        foundation::core::Serialize(ar, "blendDistance", c.blendDistance);
        foundation::core::Serialize(ar, "intensity", c.intensity);
        foundation::core::Serialize(ar, "resolution", c.resolution);
        foundation::core::Serialize(ar, "priority", c.priority);
        foundation::core::Serialize(ar, "update", update);
        foundation::core::Serialize(ar, "parallax", c.parallax);
        foundation::core::Serialize(ar, "enabled", c.enabled);
        c.update = static_cast<ProbeUpdateMode>(update);
    }

    // Persist the refs + the authored placement data (instances/tints are the authored content of a
    // scatter set); the skinning pose pool + caches are runtime-only. Loaded sets start at version 1
    // with boundsVersion 0, so the first extract re-uploads and recomputes bounds.
    inline void Serialize(ISerializer& ar, InstancedMeshComponent& c)
    {
        foundation::core::Serialize(ar, "mesh", c.mesh);
        foundation::core::Serialize(ar, "material", c.material);
        foundation::core::Serialize(ar, "instances", c.instances);
        foundation::core::Serialize(ar, "color", c.color);
        foundation::core::Serialize(ar, "tints", c.tints);
        foundation::core::Serialize(ar, "visible", c.visible);
    }
    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 InstancedMeshComponent& c)
    {
        c.mesh.Bind(manager);
        c.material.Bind(manager);
    }

    // Measures its entities for the scene (ISceneEntityBounds): the set's merged world sphere,
    // which extraction keeps current, as a box.
    class InstancedMeshComponentManager final
        : public scene::SerializableComponentManager<InstancedMeshComponent>,
          public scene::ISceneEntityBounds
    {
    public:
        InstancedMeshComponentManager()
            : scene::SerializableComponentManager<InstancedMeshComponent>(u8"instanced_mesh")
        {
        }
        [[nodiscard]] scene::ISceneEntityBounds* AsEntityBounds() noexcept override { return this; }
        [[nodiscard]] bool EntityBounds(scene::Scene& scene, scene::EntityHandle entity, AABB& out) override;
    };
    // Persist the texture ref + plain fields; the raw view override is runtime-only.
    inline void Serialize(ISerializer& ar, SpriteComponent& c)
    {
        foundation::core::Serialize(ar, "texture", c.textureAsset);
        foundation::core::Serialize(ar, "size", c.size);
        foundation::core::Serialize(ar, "uvRect", c.uvRect);
        foundation::core::Serialize(ar, "tint", c.tint);
        u32 orientation = static_cast<u32>(c.orientation);
        foundation::core::Serialize(ar, "orientation", orientation);
        c.orientation = static_cast<SpriteOrientation>(orientation);
        foundation::core::Serialize(ar, "additive", c.additive);
        foundation::core::Serialize(ar, "visible", c.visible);
    }
    inline void ResolveResources(foundation::resource::ResourceManager& manager, SpriteComponent& c)
    {
        c.textureAsset.Bind(manager);
    }

    inline void Serialize(ISerializer& ar, DecalComponent& c)
    {
        foundation::core::Serialize(ar, "texture", c.textureAsset);
        foundation::core::Serialize(ar, "size", c.size);
        foundation::core::Serialize(ar, "color", c.color);
        foundation::core::Serialize(ar, "fadeStart", c.fadeStart);
        foundation::core::Serialize(ar, "fadeEnd", c.fadeEnd);
        foundation::core::Serialize(ar, "visible", c.visible);
    }
    inline void ResolveResources(foundation::resource::ResourceManager& manager, DecalComponent& c)
    {
        c.textureAsset.Bind(manager);
    }

    class SpriteComponentManager final : public scene::SerializableComponentManager<SpriteComponent>
    {
    public:
        SpriteComponentManager() : scene::SerializableComponentManager<SpriteComponent>(u8"sprite")
        {
        }
    };
    class DecalComponentManager final : public scene::SerializableComponentManager<DecalComponent>
    {
    public:
        DecalComponentManager() : scene::SerializableComponentManager<DecalComponent>(u8"decal") {}
    };
    class CameraComponentManager final : public scene::SerializableComponentManager<CameraComponent>
    {
    public:
        CameraComponentManager() : scene::SerializableComponentManager<CameraComponent>(u8"camera")
        {
        }
    };
    class LightComponentManager final : public scene::SerializableComponentManager<LightComponent>
    {
    public:
        LightComponentManager() : scene::SerializableComponentManager<LightComponent>(u8"light") {}
    };
    class ReflectionProbeComponentManager final
        : public scene::SerializableComponentManager<ReflectionProbeComponent>
    {
    public:
        ReflectionProbeComponentManager()
            : scene::SerializableComponentManager<ReflectionProbeComponent>(u8"reflection_probe")
        {
        }
    };

    // SkyMode is defined in the snapshot layer (foundation.render :data) and reused here.

    // Where a scene settings block's values come from: the scene's own (stored in the scene), or a
    // shared profile asset the block references. Values live in one place, chosen by this.
    enum class SettingsSource : u8
    {
        Scene,
        Profile,
    };

    class EnvironmentProfile; // a shared Environment Profile asset's product (below)
    class PostProcessProfile; // a shared Post Process Profile asset's product (below)

    // The scene's environment - ONE per scene (not a component). Drives both the IBL ambient and the
    // (upcoming) visible sky. A plain SceneSystem injected by the RenderSubsystem; extraction reads it
    // into the snapshot. When IBL is active these sky settings drive shading; `ambientColor ×
    // ambientIntensity` remains the flat fallback used when no environment is active.
    struct EnvironmentSettings
    {
        // Flat ambient FILL: adds on top of the environment's image-based ambient in every sky
        // mode (intensity 0 = pure IBL); when IBL is unavailable it is the only ambient.
        // sRGB, like every colour (the default is the look it had when colours were read raw).
        Color ambientColor = Color{0.349f, 0.381f, 0.437f, 1.0f};
        f32 ambientIntensity = 0.3f;

        SkyMode skyMode = SkyMode::Procedural;
        // Env RADIANCE master: baked once into the env cube, so it scales the visible sky's base
        // AND the IBL diffuse/specular lighting derived from that cube (the old sky pass wrongly
        // double-applied it, squaring only the background). Dim it to lower the whole environment
        // (sky + image-based lighting) together.
        f32 skyIntensity = 1.0f;
        // Display-only backdrop dimmer: an EXTRA multiplier on the VISIBLE sky (main view + probe
        // reflections of it) layered on top of skyIntensity. Does NOT touch IBL lighting - use it
        // to calm a too-bright sky while keeping the scene lit. Default 0.7 (Unity-like backdrop).
        f32 skyBackgroundIntensity = 0.5f;
        f32 skyRotation = 0.0f; // yaw (radians) for HDR/cubemap
        // The textured modes' source: HDREquirect = a 2D .hdr texture asset; Cubemap = a
        // cube-shaped texture asset. Ignored by the untextured modes. (The programmatic
        // RenderSubsystem::SetSkyEquirect/SetSkyCubemap pixel paths remain for tools/samples.)
        foundation::resource::Ref<texture::Texture> skyTexture;
        // Procedural sky (Unity-default-like: a soft, hazy, low-saturation daytime blue rather than
        // a punchy vivid one - dimmer horizon, desaturated zenith, near-neutral ground). sRGB.
        Color skyHorizon = Color{0.748f, 0.798f, 0.854f, 1.0f};
        Color skyZenith = Color{0.485f, 0.634f, 0.786f, 1.0f}; // also the Color-mode color
        Color skyGround = Color{0.547f, 0.547f, 0.547f, 1.0f};
        f32 sunIntensity = 1.0f;
        f32 sunAngularSize = 0.5f; // sun disc size (degrees)
        f32 turbidity = 3.0f;      // Analytic (Preetham) haze (~2..10)
        // Render-time dimmers on the sky's LIGHTING contribution, independent of its visible
        // appearance (the lever skyIntensity is not: that scales backdrop + lighting together).
        // Godot's ambient_light_sky_contribution / Lumix's indirect_intensity, split by term:
        // diffuse scales the SH9 irradiance, specular the prefiltered reflections. 1 = full
        // physical strength (a blue sky legitimately blue-lights an unlit scene); dial diffuse
        // down to calm the cast without touching the sky or its reflections.
        f32 iblDiffuseIntensity = 1.0f;
        f32 iblSpecularIntensity = 1.0f;
        // The sun's shadow reach (v5): how far from the camera its cascades cover (clamped to the
        // camera's far plane, but independent of it: a street-scale scene keeps its near shadows
        // sharp with a short reach while the camera sees far), how the cascade splits blend (0 =
        // even, 1 = more of the map near the camera) and the width it fades out over at the reach.
        f32 shadowDistance = 300.0f;
        f32 shadowCascadeSplit = 0.5f;
        f32 shadowFadeDistance = 40.0f;

        // Where the values come from (v6): the scene's own above, or `profile`'s (an Environment
        // Profile asset) while the source is Profile. Scene-only: a profile carries no source.
        SettingsSource source = SettingsSource::Scene;
        foundation::resource::Ref<EnvironmentProfile> profile;
    };

    // The environment's value fields, in one order for the scene block and the profile records (a
    // field added to the block reaches its profile). `shadowReach` = the v5 fields are present
    // (a scene block read at version 4 has none).
    inline void SerializeEnvironmentValues(ISerializer& ar, EnvironmentSettings& e, bool shadowReach)
    {
        foundation::core::Serialize(ar, "skyTexture", e.skyTexture);
        foundation::core::Serialize(ar, "ambientColor", e.ambientColor);
        foundation::core::Serialize(ar, "ambientIntensity", e.ambientIntensity);
        u32 mode = static_cast<u32>(e.skyMode);
        foundation::core::Serialize(ar, "skyMode", mode);
        if (ar.Mode() == SerializeMode::Read)
        {
            e.skyMode = static_cast<SkyMode>(mode);
        }
        foundation::core::Serialize(ar, "skyIntensity", e.skyIntensity);
        foundation::core::Serialize(ar, "skyBackgroundIntensity", e.skyBackgroundIntensity);
        foundation::core::Serialize(ar, "skyRotation", e.skyRotation);
        foundation::core::Serialize(ar, "skyHorizon", e.skyHorizon);
        foundation::core::Serialize(ar, "skyZenith", e.skyZenith);
        foundation::core::Serialize(ar, "skyGround", e.skyGround);
        foundation::core::Serialize(ar, "sunIntensity", e.sunIntensity);
        foundation::core::Serialize(ar, "sunAngularSize", e.sunAngularSize);
        foundation::core::Serialize(ar, "turbidity", e.turbidity);
        foundation::core::Serialize(ar, "iblDiffuseIntensity", e.iblDiffuseIntensity);
        foundation::core::Serialize(ar, "iblSpecularIntensity", e.iblSpecularIntensity);
        if (shadowReach)
        {
            foundation::core::Serialize(ar, "shadowDistance", e.shadowDistance);
            foundation::core::Serialize(ar, "shadowCascadeSplit", e.shadowCascadeSplit);
            foundation::core::Serialize(ar, "shadowFadeDistance", e.shadowFadeDistance);
        }
    }

    // A shared Environment Profile: the values a scene's environment takes while its source is
    // Profile (the loaded asset; a script's change to it is in memory, seen by every scene of the
    // run sharing it).
    class EnvironmentProfile final : public Object
    {
        RTTI_OBJECT(EnvironmentProfile, Object)
    public:
        EnvironmentSettings values; // source / profile unused here
    };

    class EnvironmentSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] EnvironmentSettings& Environment() noexcept { return m_env; }
        [[nodiscard]] const EnvironmentSettings& Environment() const noexcept { return m_env; }

        // The scene's render clock: seconds accumulated from the scene's OWN dt (the context,
        // group and scene time scales composed by the scene manager), so a paused or slowed
        // scene's time-driven shading (the WIND sway) pauses or slows with it. Advances once per
        // frame in the Update phase and ONLY while the scene simulates: the editor's editing
        // scene ticks every frame with simulation disabled (Simulate enables it), and a frozen
        // world's grass must stand still there. The previous frame's value rides along for
        // motion vectors. Runtime state: never serialized.
        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase == scene::ScenePhase::Update &&
                (m_scene == nullptr || m_scene->SimulationEnabled()))
            {
                m_prevTimeSeconds = m_timeSeconds;
                m_timeSeconds += deltaTime;
            }
        }
        [[nodiscard]] f32 TimeSeconds() const noexcept { return m_timeSeconds; }
        [[nodiscard]] f32 PrevTimeSeconds() const noexcept { return m_prevTimeSeconds; }

        // Scene-level settings seam: the editor's scene inspector edits m_env through the
        // reflected type; SerializeScene persists it (wrapped in the type's versioned payload -
        // bump the reflected dataVersion whenever a field is added).
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<EnvironmentSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_env; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"environment"; }
        // (the clock members sit with the settings below)
        void ResolveResources(foundation::resource::ResourceManager& manager) override
        {
            m_env.skyTexture.Bind(manager);
            m_env.profile.Bind(manager);
            if (EnvironmentProfile* profile = m_env.profile.Get())
            {
                profile->values.skyTexture.Bind(manager); // a profile edit's new reference
            }
        }
        void SerializeSettings(ISerializer& ar) override
        {
            // v5 added the shadow reach, v6 the source (the legacy reader takes v4 and v5 payloads,
            // which read the defaults for what they lack).
            const bool write = ar.Mode() == SerializeMode::Write;
            SerializeEnvironmentValues(ar, m_env, write || ar.Version() >= 5);
            if (write || ar.Version() >= 6)
            {
                u32 source = static_cast<u32>(m_env.source);
                foundation::core::Serialize(ar, "source", source);
                if (!write)
                {
                    m_env.source = static_cast<SettingsSource>(source);
                }
                foundation::core::Serialize(ar, "profile", m_env.profile);
            }
        }

        /// The values in effect: the profile's while the source is Profile and it is loaded, the
        /// scene's own otherwise (what the renderer, the scene's script handle and the inspector
        /// read). Environment() is always the scene's own block (its source, its stored values).
        [[nodiscard]] EnvironmentSettings& Effective() noexcept
        {
            if (m_env.source == SettingsSource::Profile)
            {
                if (EnvironmentProfile* profile = m_env.profile.Get())
                {
                    return profile->values;
                }
            }
            return m_env;
        }
        [[nodiscard]] const EnvironmentSettings& Effective() const noexcept
        {
            return const_cast<EnvironmentSystem*>(this)->Effective();
        }
        [[nodiscard]] const TypeInfo* SettingsProfileType() const noexcept override
        {
            return &EnvironmentProfile::StaticType();
        }
        [[nodiscard]] void* EffectiveSettingsInstance() noexcept override { return &Effective(); }
        [[nodiscard]] Guid SettingsProfile() noexcept override
        {
            return (m_env.source == SettingsSource::Profile && m_env.profile.Get() != nullptr)
                       ? m_env.profile.id
                       : Guid{};
        }
        void UseSettingsProfile(const Guid& profile) override
        {
            m_env.source = profile.IsNil() ? SettingsSource::Scene : SettingsSource::Profile;
            m_env.profile.SetId(profile); // bound by the next ResolveResources
        }
        void CopySettingsProfileIntoScene() override
        {
            const EnvironmentSettings values = Effective(); // a copy: Effective may be this block
            const auto profile = m_env.profile;
            m_env = values;
            m_env.source = SettingsSource::Scene;
            m_env.profile = profile; // kept, so the profile is a pick away
        }

    private:
        scene::Scene* m_scene = nullptr; // borrowed (the scene owns its systems)
        f32 m_timeSeconds = 0.0f;        // the scene render clock (OnUpdate)
        f32 m_prevTimeSeconds = 0.0f;
        EnvironmentSettings m_env;
    };

    // ============================================================================================
    // Post-processing: the authored "look" of a scene -
    // exposure/tonemap, bloom, AO, SSR, anti-aliasing. ONE per scene (like EnvironmentSettings),
    // reflected + serialized + inspector-surfaced with no bespoke UI, extracted per frame and
    // applied per view. Defaults MATCH today's RenderSubsystem values, so a scene looks identical
    // until an artist edits the block.
    // ============================================================================================

    enum class TonemapOperator : u32
    {
        Clamp = 0,
        AgX = 1
    }; // CM1a (clamp) / CM1b (AgX)
    enum class AaMode : u32
    {
        Off = 0,
        FXAA = 1,
        TAA = 2
    }; // one enum: TAA and FXAA are exclusive

    struct PostProcessSettings
    {
        // Exposure / tonemap. exposureEV is photographic stops: the tonemap applies 2^EV, so 0 =
        // neutral (the old fixed 1.0 multiplier), +1 = one stop brighter, -1 = one stop darker.
        f32 exposureEV = 0.0f;
        TonemapOperator tonemapOperator = TonemapOperator::AgX;

        // Bloom.
        bool bloomEnabled = true;
        f32 bloomThreshold = 1.0f;
        f32 bloomKnee = 0.6f;
        f32 bloomIntensity = 0.05f;

        // Ambient occlusion. `aoStrength` is the master mix (0..1); GTAO's own radius/intensity below.
        AoMode aoMode = AoMode::Off;
        f32 aoStrength = 0.6f;
        f32 aoRadius = 0.5f;
        f32 aoIntensity = 1.0f;

        // Screen-space reflections.
        bool ssrEnabled = false;
        f32 ssrIntensity = 1.0f;

        // Screen-space global illumination (one additive diffuse bounce, temporal).
        bool ssgiEnabled = false;
        f32 ssgiIntensity = 1.0f;

        // Auto-exposure (eye adaptation): exposure follows the scene's average luminance,
        // clamped to +-EV around the authored exposureEV. Off = the fixed EV alone.
        bool autoExposure = false;
        f32 autoExposureKey = 0.18f;    // target "middle gray" the average maps to
        f32 autoExposureSpeed = 2.0f;   // adaptation rate (1/s)
        f32 autoExposureMinEV = -4.0f;  // clamp window (stops, relative)
        f32 autoExposureMaxEV = 4.0f;

        // Display-referred color grading: a strip LUT texture (width = size*size, height =
        // size; author a neutral 256x16 strip, grade it in an image editor, import with
        // Color Space = Linear so the samples pass through undecoded). Nil = no grading.
        foundation::resource::Ref<texture::Texture> gradingLut;
        f32 gradingIntensity = 1.0f;

        // Anti-aliasing (aaMode selects the exclusive path; the others' params are ignored).
        AaMode aaMode = AaMode::Off;
        f32 taaBlendFactor = 0.97f;   // history weight   (aaMode == TAA)
        f32 taaVarianceGamma = 1.25f; // variance-clip box half-width (aaMode == TAA)
        f32 fxaaSubpixel = 0.75f;     // subpixel aliasing removal    (aaMode == FXAA)

        // Where the values come from (v4): the scene's own above, or `profile`'s (a Post Process
        // Profile asset) while the source is Profile. Scene-only: a profile carries no source.
        SettingsSource source = SettingsSource::Scene;
        foundation::resource::Ref<PostProcessProfile> profile;
    };

    // The post settings' value fields, in one order for the scene block and the profile records.
    inline void SerializePostValues(ISerializer& ar, PostProcessSettings& p)
    {
        const auto enumField = [&ar]<typename E>(const char* key, E& value)
        {
            u32 raw = static_cast<u32>(value);
            foundation::core::Serialize(ar, key, raw);
            if (ar.Mode() == SerializeMode::Read)
            {
                value = static_cast<E>(raw);
            }
        };
        foundation::core::Serialize(ar, "exposureEV", p.exposureEV);
        enumField("tonemapOperator", p.tonemapOperator);
        foundation::core::Serialize(ar, "bloomEnabled", p.bloomEnabled);
        foundation::core::Serialize(ar, "bloomThreshold", p.bloomThreshold);
        foundation::core::Serialize(ar, "bloomKnee", p.bloomKnee);
        foundation::core::Serialize(ar, "bloomIntensity", p.bloomIntensity);
        enumField("aoMode", p.aoMode);
        foundation::core::Serialize(ar, "aoStrength", p.aoStrength);
        foundation::core::Serialize(ar, "aoRadius", p.aoRadius);
        foundation::core::Serialize(ar, "aoIntensity", p.aoIntensity);
        foundation::core::Serialize(ar, "ssrEnabled", p.ssrEnabled);
        foundation::core::Serialize(ar, "ssrIntensity", p.ssrIntensity);
        enumField("aaMode", p.aaMode);
        foundation::core::Serialize(ar, "taaBlendFactor", p.taaBlendFactor);
        foundation::core::Serialize(ar, "taaVarianceGamma", p.taaVarianceGamma);
        foundation::core::Serialize(ar, "fxaaSubpixel", p.fxaaSubpixel);
        foundation::core::Serialize(ar, "autoExposure", p.autoExposure);
        foundation::core::Serialize(ar, "autoExposureKey", p.autoExposureKey);
        foundation::core::Serialize(ar, "autoExposureSpeed", p.autoExposureSpeed);
        foundation::core::Serialize(ar, "autoExposureMinEV", p.autoExposureMinEV);
        foundation::core::Serialize(ar, "autoExposureMaxEV", p.autoExposureMaxEV);
        foundation::core::Serialize(ar, "gradingLut", p.gradingLut);
        foundation::core::Serialize(ar, "gradingIntensity", p.gradingIntensity);
        foundation::core::Serialize(ar, "ssgiEnabled", p.ssgiEnabled);
        foundation::core::Serialize(ar, "ssgiIntensity", p.ssgiIntensity);
    }

    // A shared Post Process Profile: the values a scene's post settings take while its source is
    // Profile.
    class PostProcessProfile final : public Object
    {
        RTTI_OBJECT(PostProcessProfile, Object)
    public:
        PostProcessSettings values; // source / profile unused here
    };

    RTTI_DEFINE_OBJECT(EnvironmentProfile, "rtti::engine::render")
    RTTI_DEFINE_OBJECT(PostProcessProfile, "rtti::engine::render")

    class PostProcessSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] PostProcessSettings& Post() noexcept { return m_post; }
        [[nodiscard]] const PostProcessSettings& Post() const noexcept { return m_post; }

        // Scene-settings seam (same shape as EnvironmentSystem): the inspector edits m_post through
        // the reflected type; SerializeScene persists it (versioned payload; bump the dataVersion
        // whenever a field is added).
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<PostProcessSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_post; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"postprocess"; }
        void SerializeSettings(ISerializer& ar) override
        {
            SerializePostValues(ar, m_post);
            // v4 added the source (the legacy reader takes a v3 payload: source Scene).
            const bool write = ar.Mode() == SerializeMode::Write;
            if (write || ar.Version() >= 4)
            {
                u32 source = static_cast<u32>(m_post.source);
                foundation::core::Serialize(ar, "source", source);
                if (!write)
                {
                    m_post.source = static_cast<SettingsSource>(source);
                }
                foundation::core::Serialize(ar, "profile", m_post.profile);
            }
        }

        void ResolveResources(foundation::resource::ResourceManager& manager) override
        {
            m_post.gradingLut.Bind(manager);
            m_post.profile.Bind(manager);
            if (PostProcessProfile* profile = m_post.profile.Get())
            {
                profile->values.gradingLut.Bind(manager); // a profile edit's new reference
            }
        }

        /// The values in effect (as EnvironmentSystem::Effective): the profile's while the source
        /// is Profile and it is loaded, the scene's own otherwise.
        [[nodiscard]] PostProcessSettings& Effective() noexcept
        {
            if (m_post.source == SettingsSource::Profile)
            {
                if (PostProcessProfile* profile = m_post.profile.Get())
                {
                    return profile->values;
                }
            }
            return m_post;
        }
        [[nodiscard]] const PostProcessSettings& Effective() const noexcept
        {
            return const_cast<PostProcessSystem*>(this)->Effective();
        }
        [[nodiscard]] const TypeInfo* SettingsProfileType() const noexcept override
        {
            return &PostProcessProfile::StaticType();
        }
        [[nodiscard]] void* EffectiveSettingsInstance() noexcept override { return &Effective(); }
        [[nodiscard]] Guid SettingsProfile() noexcept override
        {
            return (m_post.source == SettingsSource::Profile && m_post.profile.Get() != nullptr)
                       ? m_post.profile.id
                       : Guid{};
        }
        void UseSettingsProfile(const Guid& profile) override
        {
            m_post.source = profile.IsNil() ? SettingsSource::Scene : SettingsSource::Profile;
            m_post.profile.SetId(profile); // bound by the next ResolveResources
        }
        void CopySettingsProfileIntoScene() override
        {
            const PostProcessSettings values = Effective(); // a copy: Effective may be this block
            const auto profile = m_post.profile;
            m_post = values;
            m_post.source = SettingsSource::Scene;
            m_post.profile = profile; // kept, so the profile is a pick away
        }

    private:
        PostProcessSettings m_post;
    };

    // Resolve a scene's authored PostProcessSettings into the renderer's per-view ViewPostConfig:
    // exposure EV/stops -> the tonemap's linear multiplier (2^EV), authoring enums -> pass primitives.
    // `needsMotion` gets only the TAA part here (SSR's temporal contribution depends on the frame-global
    // SsrPass::Params, so the caller ORs it in).
    [[nodiscard]] inline ViewPostConfig ResolveScenePost(const PostProcessSettings& s)
    {
        ViewPostConfig vp;
        vp.exposure = foundation::core::Pow(2.0f, s.exposureEV);
        vp.agxTonemap = (s.tonemapOperator == TonemapOperator::AgX);
        vp.bloomEnabled = s.bloomEnabled;
        vp.bloomThreshold = s.bloomThreshold;
        vp.bloomKnee = s.bloomKnee;
        vp.bloomIntensity = s.bloomIntensity;
        vp.aoMode = static_cast<u32>(s.aoMode);
        vp.aoStrength = s.aoStrength;
        vp.aoRadius = s.aoRadius;
        vp.aoIntensity = s.aoIntensity;
        vp.taaEnabled = (s.aaMode == AaMode::TAA);
        vp.taaBlend = s.taaBlendFactor;
        vp.taaGamma = s.taaVarianceGamma;
        vp.fxaaEnabled = (s.aaMode == AaMode::FXAA);
        vp.fxaaSubpixel = s.fxaaSubpixel;
        vp.ssrEnabled = s.ssrEnabled;
        vp.ssrIntensity = s.ssrIntensity;
        vp.ssgiEnabled = s.ssgiEnabled;
        vp.ssgiIntensity = s.ssgiIntensity;
        vp.needsMotion = vp.taaEnabled; // caller ORs in (ssrEnabled && ssr-temporal)
        vp.autoExposure = s.autoExposure;
        vp.autoExposureKey = s.autoExposureKey;
        vp.autoExposureSpeed = s.autoExposureSpeed;
        // The clamp window is authored in relative EV stops; the tonemap clamps a LINEAR
        // multiplier.
        vp.autoExposureMin = foundation::core::Pow(2.0f, s.autoExposureMinEV);
        vp.autoExposureMax = foundation::core::Pow(2.0f, s.autoExposureMaxEV);
        if (texture::Texture* lut = s.gradingLut.Get())
        {
            // Strip layout: height = the LUT size (a 256x16 strip = 16 slices). A texture
            // that is not wider than tall is not a strip - ignore it rather than garble.
            if (lut->View() != nullptr && lut->Height() >= 2 &&
                lut->Width() == lut->Height() * lut->Height())
            {
                vp.gradingLut = lut->View();
                vp.gradingLutUid = lut->Uid();
                vp.gradingLutSize = static_cast<f32>(lut->Height());
                vp.gradingIntensity = s.gradingIntensity;
            }
        }
        return vp;
    }

    // Apply an editor viewport's ephemeral post "show flags" to a resolved ViewPostConfig, stripping
    // effects for editing clarity (never written back to the scene). Does NOT recompute needsMotion -
    // the caller finalizes that after (it also depends on the frame-global SSR `temporal` flag).
    inline void ApplyViewPostOverride(ViewPostConfig& vp, const ViewPostOverride& o)
    {
        if (o.disablePost || o.disableBloom)
        {
            vp.bloomEnabled = false;
        }
        if (o.disablePost || o.disableAo)
        {
            vp.aoMode = 0u;
        } // AoMode::Off
        if (o.disablePost || o.disableSsr)
        {
            vp.ssrEnabled = false;
        }
        if (o.disablePost || o.disableSsgi)
        {
            vp.ssgiEnabled = false;
        }
        if (o.disablePost || o.disableAa)
        {
            vp.taaEnabled = false;
            vp.fxaaEnabled = false;
        }
        if (o.disablePost)
        {
            // Editing clarity: the viewport must not shift brightness as the camera moves
            // between dark and bright regions - freeze to the fixed authored EV.
            vp.autoExposure = false;
        }
        // MSAA is an INDEPENDENT toggle: disablePost/disableAa do NOT touch it.
        // The editor viewport forces its own off/2x/4x count here (0 = leave the resolved count).
        if (o.msaaOverride != 0)
        {
            vp.msaaSamples = o.msaaOverride;
        }
    }

    // The screen-space passes rebuild view-space positions from depth with perspective math (AO,
    // SSR, SSGI) or linearize depth as perspective (TAA's rejection), so an orthographic view runs
    // without them. Like ApplyViewPostOverride, needsMotion is the caller's to finalize after.
    inline void LimitPostForOrthographic(ViewPostConfig& vp)
    {
        vp.aoMode = 0u; // AoMode::Off
        vp.ssrEnabled = false;
        vp.ssgiEnabled = false;
        vp.taaEnabled = false;
    }

    // Live re-resolving handles over the render scene-SYSTEMS' one-per-scene settings:
    // EnvironmentSettings.of(scene) / PostProcessSettings.of(scene) return a handle typed as the
    // settings struct that re-resolves the scene's LIVE settings on every field access - so a
    // behavior edits the scene's actual sky/fog/ambient (EnvironmentSystem) or exposure/tonemap/
    // bloom/AA (PostProcessSystem). Same re-resolving-handle idea as a component's `.of`, but
    // scene-scoped: these settings are one-per-scene, owned by the render scene-systems, not entities.
    namespace settingsref
    {
        struct SceneSettingsCtx
        {
            scene::Scene* scene = nullptr;
        };

        [[nodiscard]] inline void* ResolveEnvironmentSettings(const Variant& value)
        {
            const auto* ctx = static_cast<const SceneSettingsCtx*>(value.ResolveContext());
            if (ctx == nullptr || ctx->scene == nullptr)
            {
                return nullptr;
            }
            EnvironmentSystem* system = ctx->scene->GetSystem<EnvironmentSystem>();
            return system != nullptr ? &system->Effective() : nullptr; // the values in effect
        }
        [[nodiscard]] inline void* ResolvePostProcessSettings(const Variant& value)
        {
            const auto* ctx = static_cast<const SceneSettingsCtx*>(value.ResolveContext());
            if (ctx == nullptr || ctx->scene == nullptr)
            {
                return nullptr;
            }
            PostProcessSystem* system = ctx->scene->GetSystem<PostProcessSystem>();
            return system != nullptr ? &system->Effective() : nullptr; // the values in effect
        }
    }

    // EnvironmentSettings.of(scene): a re-resolving handle over the scene's live environment settings
    // (empty if the scene has no EnvironmentSystem). Reflected on EnvironmentSettings with the
    // ReturnType-override so the declared script return IS EnvironmentSettings.
    [[nodiscard]] inline Variant EnvironmentSettingsOf(foundation::script::Scene sceneHandle)
    {
        return Variant::Resolving(&TypeOf<EnvironmentSettings>(),
                                  &settingsref::ResolveEnvironmentSettings,
                                  settingsref::SceneSettingsCtx{sceneHandle.scene});
    }
    // PostProcessSettings.of(scene): a re-resolving handle over the scene's live post settings.
    [[nodiscard]] inline Variant PostProcessSettingsOf(foundation::script::Scene sceneHandle)
    {
        return Variant::Resolving(&TypeOf<PostProcessSettings>(),
                                  &settingsref::ResolvePostProcessSettings,
                                  settingsref::SceneSettingsCtx{sceneHandle.scene});
    }

    // A scene-bound RENDER handle (SceneRender.of(scene)): runtime WORLD ops on render components
    // that need a run service the component data cannot reach - here, swapping a resource::Ref by id
    // (mesh / slot-0 material). Keyed by entity, mirroring ScenePhysics (component = auto-reflected
    // DATA; scene-handle = world ops keyed by entity). A value type carrying the scene ptr.
    struct SceneRender
    {
        scene::Scene* scene = nullptr;

        // How much light reaches `position` (linear RGB, the units lights are authored in): every
        // enabled light with the renderer's own range falloff and spot cone, a light that casts
        // shadows stopped by what stands between (a ray through the scene's solid surfaces, among
        // the collision groups in `groupMask`), plus the ambient. The light a surface facing each
        // light would get, before its colour and angle: a CPU estimate of the shading for a light
        // meter or a guard's eye, not a read of the frame (the sky's image-based light is not in
        // it). Ask from a point off any surface: a ray that starts inside a wall is stopped by it.
        [[nodiscard]] Float3 lightAt(Float3 position) const;
        [[nodiscard]] Float3 lightAt(Float3 position, u32 groupMask) const;

        // Swap the entity's MeshComponent mesh to resource `id`, binding it through the run's
        // resource manager so the swap takes effect live (a bare VM with no manager sets the id
        // only, unbound). False (a no-op) if the scene is null or the entity has no MeshComponent.
        bool setMesh(foundation::script::Entity entity, Guid id) const
        {
            MeshComponent* mesh = MeshOf(entity);
            if (mesh == nullptr)
            {
                return false;
            }
            mesh->mesh.SetId(id);
            if (auto* resources = foundation::script::CurrentRunResources())
            {
                mesh->mesh.Bind(*resources);
            }
            return true;
        }

        // Swap the entity's material in `slot` (slot 0: single-material meshes / the whole-mesh slot)
        // to resource `id`, binding it through the run's resource manager; the slots grow to reach
        // it. False for an entity without a mesh or a negative slot (Sedulous's SetMaterial).
        bool setMaterial(foundation::script::Entity entity, Guid id) const
        {
            return setMaterial(entity, id, 0);
        }
        bool setMaterial(foundation::script::Entity entity, Guid id, i32 slot) const
        {
            MeshComponent* mesh = MeshOf(entity);
            if (mesh == nullptr || slot < 0)
            {
                return false;
            }
            while (mesh->materials.Size() <= static_cast<usize>(slot))
            {
                mesh->materials.PushBack(foundation::resource::Ref<materials::Material>{});
            }
            mesh->materials[static_cast<usize>(slot)].SetId(id);
            if (auto* resources = foundation::script::CurrentRunResources())
            {
                mesh->materials[static_cast<usize>(slot)].Bind(*resources);
            }
            return true;
        }

        // Set one of the entity's material properties for it alone: the material in `slot` draws with
        // `value` for the property `name` (as the material editor shows it: Roughness, EmissiveColor),
        // every other mesh using that material unchanged. A Float4 for a colour or a vector: a colour
        // as authored, sRGB rgba, and an HDR colour (EmissiveColor) sRGB rgb with its intensity in w.
        // A name the material does not have, or a value wider than the property, changes nothing.
        // Runtime only: not saved with the scene. False for an entity without a mesh or a negative slot.
        bool setMaterialFloat(foundation::script::Entity entity, i32 slot, String name, f32 value) const
        {
            MeshComponent* mesh = MeshOf(entity);
            if (mesh == nullptr || slot < 0)
            {
                return false;
            }
            mesh->SetMaterialProperty(static_cast<u32>(slot), name, Float4{value, 0, 0, 0}, sizeof(f32));
            return true;
        }
        bool setMaterialFloat4(foundation::script::Entity entity, i32 slot, String name, Float4 value) const
        {
            MeshComponent* mesh = MeshOf(entity);
            if (mesh == nullptr || slot < 0)
            {
                return false;
            }
            mesh->SetMaterialProperty(static_cast<u32>(slot), name, value, sizeof(Float4));
            return true;
        }
        // Put a property set by setMaterialFloat/Float4 back to the material's own value; false if it
        // was not set.
        bool clearMaterialProperty(foundation::script::Entity entity, i32 slot, String name) const
        {
            MeshComponent* mesh = MeshOf(entity);
            return mesh != nullptr && slot >= 0 && mesh->ClearMaterialProperty(static_cast<u32>(slot), name);
        }

        // Point the entity's camera at the texture `id` (a render texture asset): it then draws
        // into that texture instead of the screen. A nil id clears the target, so the camera may
        // be the screen's again. Bound through the run's resource manager, like setMesh. False
        // (a no-op) for an entity without a CameraComponent.
        bool setCameraTarget(foundation::script::Entity entity, Guid id) const
        {
            CameraComponentManager* cameras =
                (scene != nullptr) ? scene->GetSystem<CameraComponentManager>() : nullptr;
            CameraComponent* camera = (cameras != nullptr) ? cameras->Get(entity.Handle()) : nullptr;
            if (camera == nullptr)
            {
                return false;
            }
            camera->target = foundation::resource::Ref<texture::Texture>{};
            camera->target.SetId(id);
            if (auto* resources = foundation::script::CurrentRunResources(); resources != nullptr &&
                                                                             !id.IsNil())
            {
                camera->target.Bind(*resources);
            }
            return true;
        }

        [[nodiscard]] static SceneRender of(foundation::script::Scene sceneHandle)
        {
            return SceneRender{sceneHandle.scene};
        }

    private:
        [[nodiscard]] MeshComponent* MeshOf(foundation::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            MeshComponentManager* meshes = scene->GetSystem<MeshComponentManager>();
            return (meshes != nullptr) ? meshes->Get(entity.Handle()) : nullptr;
        }
    };

    // DebugDraw.of(scene): immediate-mode debug drawing into a scene's per-scene gizmo list - drawn
    // in EVERY view of that scene (incl. play-in-editor), CLEARED each frame, so a persistent overlay
    // (an aim path, a marker) must be re-issued every tick from a script's update. A value handle
    // carrying the scene ptr; each call resolves the RenderSubsystem via the per-context render
    // service and appends to DebugScene(scene). Colors are 0..1 sRGB floats, as entered everywhere
    // (alpha 1); world units.
    // Methods are defined in RenderComponentsImpl.cpp (the impl unit can see :subsystem's
    // RenderSubsystem; this :components interface cannot).
    struct DebugDraw
    {
        scene::Scene* scene = nullptr;

        // A depth-tested world-space segment from (x0,y0,z0) to (x1,y1,z1).
        void line(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 r, f32 g, f32 b) const;
        // A segment from `origin` extending along `direction` (drawn to origin+direction).
        void ray(f32 x, f32 y, f32 z, f32 dx, f32 dy, f32 dz, f32 r, f32 g, f32 b) const;
        // A line with an arrowhead at the end point (direction cue).
        void arrow(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 r, f32 g, f32 b) const;
        // A wireframe sphere / a 3-axis cross marker at a point.
        void sphere(f32 x, f32 y, f32 z, f32 radius, f32 r, f32 g, f32 b) const;
        void cross(f32 x, f32 y, f32 z, f32 size, f32 r, f32 g, f32 b) const;
        // A world-space text label anchored at (x,y,z).
        void text(f32 x, f32 y, f32 z, String label, f32 r, f32 g, f32 b) const;

        // ---- as Sedulous's Debug facade: points by Float3, a Color, and `overlay` to draw over
        // the scene instead of depth tested ----
        void line(Float3 from, Float3 to, Color color) const;
        void line(Float3 from, Float3 to, Color color, bool overlay) const;
        void ray(Float3 origin, Float3 direction, Color color) const;
        void ray(Float3 origin, Float3 direction, Color color, bool overlay) const;
        void wireBox(Float3 min, Float3 max, Color color) const;
        void wireBox(Float3 min, Float3 max, Color color, bool overlay) const;
        void wireSphere(Float3 center, f32 radius, Color color) const;
        void cross(Float3 center, f32 size, Color color) const;
        void cross(Float3 center, f32 size, Color color, bool overlay) const;
        void arrow(Float3 start, Float3 end, Color color) const;
        void arrow(Float3 start, Float3 end, Color color, f32 headSize) const;
        void text(Float3 worldPosition, String label, Color color) const;
        // Text in screen pixels from the top left, at `scale`.
        void screenText(f32 x, f32 y, String label, Color color) const;
        void screenText(f32 x, f32 y, String label, Color color, f32 scale) const;

        [[nodiscard]] static DebugDraw of(foundation::script::Scene sceneHandle)
        {
            return DebugDraw{sceneHandle.scene};
        }
    };

} // namespace foundation::render (exported)

// Registers all render component/enum reflection (idempotent). Called by RenderSubsystem::OnInit
// so every app with a renderer gets reflected components for free. Non-inline: the body touches
// module-linkage registration functions, which an exported inline definition may not.
export namespace engine::render
{
    void RegisterRenderComponentReflection();

    // Surfaces the render components to SCRIPT (Track A): registers MeshComponent/LightComponent
    // in the global type registry, seeds their emission roots, and makes their class names
    // import-visible in behavior/Level preludes, so `MeshComponent.of(entity).visible = false` etc.
    // work on both backends. Called by the composition root (like RegisterPhysicsScriptFacade).
    void RegisterRenderScriptFacade();
}
