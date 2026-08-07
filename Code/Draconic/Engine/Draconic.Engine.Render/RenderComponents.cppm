/// Draconic::RenderSubsystem - the `:components` partition.
///
/// The render-facing scene components + their managers - the scene-coupled side of the
/// renderer (draconic.render itself stays scene-agnostic). A MeshComponent references a
/// mesh + material to draw at its entity's transform; a CameraComponent describes a
/// view frustum (its view comes from the entity's world transform). The RenderSubsystem
/// injects these managers into each scene (via ISceneAware); extraction (:extract) reads
/// them into a render::ExtractedView that gets pushed to the renderer.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.render:components;

import draconic.foundation;
import draconic.resource;
import draconic.scene;
import draconic.geometry;
import draconic.materials;
import draconic.rhi; // rhi::TextureView (a SpriteComponent references a texture to draw)
import draconic.texture.resource; // texture::Texture (cooked product behind sprite/decal texture refs)
import draconic.render; // SkySnapshot/SkyMode (snapshot layer; render.subsystem depends on render)
import draconic.script.facades; // script::Entity/Scene + CurrentRunResources (the SceneRender handle)

using namespace draconic::foundation;

export namespace draconic::render
{

    // What to draw at an entity: a mesh + the material to draw it with. Resource refs
    // (resource::Ref): a serialized Guid resolved through the ResourceManager's proxy handles
    // (hot reload swaps the product behind every holder), or a direct RefPtr for code-created
    // meshes (samples/procedural - the direct object wins and is never serialized). `color` is a
    // per-instance tint (multiplied into the shaded color) - distinct per entity even when
    // many share one mesh + material, so it rides the per-instance data path.
    struct MeshComponent
    {
        draconic::resource::Ref<geometry::StaticMesh> mesh;
        // THE material list, indexed by SubMesh::materialIndex. Slot 0 doubles as the whole-mesh
        // material: single-material meshes hold ONE entry, and a submesh whose index is out of
        // range (or whose slot is unresolved) falls back to slot 0. Serialized identity lives in
        // `materials`; `materialCache` is the raw-pointer view EXTRACTION refreshes from the ref
        // proxies EVERY frame - late cooks and hot reloads heal live (the previous design
        // snapshotted RefPtrs once at resolve, pinning pre-cook nulls until a page reopen), and
        // the renderer stays resource-agnostic.
        Array<draconic::resource::Ref<materials::Material>> materials;
        Array<RefPtr<materials::Material>> materialCache; // runtime-only; refreshed at extract
        Color color = Color{1.0f, 1.0f, 1.0f, 1.0f};
        bool visible = true;
        // Slot-0 conveniences for runtime code (samples/spawners) - refs and raw objects both fit
        // (resource::Ref adopts direct pointers).
        void SetMaterial(const RefPtr<materials::Material>& m)
        {
            materials.Clear();
            materials.PushBack(draconic::resource::Ref<materials::Material>(m));
        }
        void SetMaterials(const Array<RefPtr<materials::Material>>& list)
        {
            materials.Clear();
            for (const RefPtr<materials::Material>& m : list)
            {
                materials.PushBack(draconic::resource::Ref<materials::Material>(m));
            }
        }

        // GPU skinning: per-bone skinning matrices for a skinned mesh, supplied per frame by the owner
        // (e.g. an AnimationPlayer's GetSkinningMatrices()). Borrowed - valid for the frame it's set;
        // null => the mesh draws static (bind pose). Extraction copies the pointer into MeshRenderData.
        const Float4x4* boneMatrices = nullptr;
        const Float4x4* prevBoneMatrices =
            nullptr; // previous-frame matrices (motion vectors); null => reuse current
        u32 boneCount = 0;
    };

    // An INSTANCED mesh ("MultiMesh"): ONE shared mesh + material drawn at N per-instance transforms that
    // live (on the GPU) in a persistent buffer owned by the renderer. Its per-frame CPU cost is O(1) in the
    // instance count - the set is extracted as ONE render item, culled as one merged AABB, and drawn once
    // per pass (depth, forward, every shadow cascade all read the same buffer). Use it for static crowds /
    // scatter (foliage, props, debris); the per-entity MeshComponent stays for genuinely dynamic objects.
    // `instances` is the CPU source of truth; every mutator bumps `version`, and the renderer re-uploads the
    // GPU buffer (and extraction recomputes the merged bounds) ONLY when the version changes. See
    // docs/design/instanced-mesh.md.
    struct InstancedMeshComponent
    {
        draconic::resource::Ref<geometry::StaticMesh> mesh;
        draconic::resource::Ref<materials::Material> material;
        // Seed one identity instance so a freshly added component (editor workflow) renders its mesh
        // at the entity's transform immediately; authored sets replace it (SetInstances/load).
        InstancedMeshComponent() { instances.PushBack(Float4x4::Identity()); }
        // Optional per-submesh materials (multi-material meshes): indexed by SubMesh::materialIndex. When
        // non-empty each submesh draws with its own material; otherwise `material` covers the whole mesh.
        // Runtime-only (RefPtr, not serialized) like MeshComponent's: per-submesh material REFS land
        // with prefabs (phase 7), which owns the model->entity workflow.
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
        // set per frame by the InstancedSkinningComponent companion (draconic.engine.animation). When posePool is
        // non-null the set draws SKINNED, and instance i uses pose (i % poseCount) - so N animated instances
        // cost only M = poseCount palette computes, not N. Borrowed (valid for the frame it's set); null => the
        // set draws static. The mesh must be a skinned mesh (has a skin stream). See docs/design/instanced-mesh.md SS7.
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
    };

    // A light on an entity. Directional uses the entity's forward (-Z); Point/Spot use its world
    // position (+ range). Extraction packs these into render::GpuLight shading inputs.
    enum class LightType : u32
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    // How a spot/point light's shadow updates (phase 5.4 static caching). Realtime = re-render every
    // frame (default). Static = render once into the cached atlas layer; the caster geometry is assumed
    // not to move (the cache only refreshes if the LIGHT itself moves / changes). Cheap for static scenes.
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
            ShadowUpdateMode::Realtime; // spot/point shadow caching (5.4)
        bool enabled = true;
        bool castsShadows = false; // phase 5 shadow caster
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
        draconic::resource::Ref<texture::Texture>
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
        draconic::resource::Ref<texture::Texture>
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
        draconic::foundation::Serialize(ar, "mesh", c.mesh);
        if (ar.Version() >= 3)
        { // v3: ONE materials array (slot 0 = whole-mesh)
            draconic::foundation::Serialize(ar, "materials", c.materials);
            draconic::foundation::Serialize(ar, "color", c.color);
            draconic::foundation::Serialize(ar, "visible", c.visible);
        }
        else
        {
            // v1/v2 migration: singular `material` + optional v2 submesh array fold into the
            // unified list (submesh array wins - it was the complete per-index set).
            draconic::resource::Ref<materials::Material> single;
            draconic::foundation::Serialize(ar, "material", single);
            draconic::foundation::Serialize(ar, "color", c.color);
            draconic::foundation::Serialize(ar, "visible", c.visible);
            Array<draconic::resource::Ref<materials::Material>> submesh;
            if (ar.Version() >= 2)
            {
                draconic::foundation::Serialize(ar, "submeshMaterials", submesh);
            }
            if (ar.Mode() == SerializeMode::Read)
            {
                c.materials.Clear();
                if (!submesh.IsEmpty())
                {
                    c.materials =
                        static_cast<Array<draconic::resource::Ref<materials::Material>>&&>(submesh);
                }
                else if (!single.id.IsNil() || single.Get() != nullptr)
                {
                    c.materials.PushBack(single);
                }
            }
        }
    }

    inline void ResolveResources(draconic::resource::ResourceManager& manager, MeshComponent& c)
    {
        c.mesh.Bind(manager);
        for (draconic::resource::Ref<materials::Material>& r : c.materials)
        {
            r.Bind(manager);
        }
        // materialCache is refreshed at EXTRACT time (per frame, through the proxies) - resolve
        // only attaches the bindings.
    }

    class MeshComponentManager final : public scene::SerializableComponentManager<MeshComponent>
    {
    public:
        MeshComponentManager() : scene::SerializableComponentManager<MeshComponent>(u8"mesh") {}
    };
    // Data-only components PERSIST (scene round-trip + full destroy-undo restore - a destroyed
    // entity's components are snapshotted through serializable managers only). Sprite/Decal/
    // InstancedMesh still hold raw GPU pointers and stay runtime-only until they move to refs.
    inline void Serialize(ISerializer& ar, LightComponent& c)
    {
        u8 type = static_cast<u8>(c.type);
        u8 shadowUpdate = static_cast<u8>(c.shadowUpdate);
        draconic::foundation::Serialize(ar, "type", type);
        draconic::foundation::Serialize(ar, "color", c.color);
        draconic::foundation::Serialize(ar, "intensity", c.intensity);
        draconic::foundation::Serialize(ar, "range", c.range);
        draconic::foundation::Serialize(ar, "innerAngle", c.innerAngle);
        draconic::foundation::Serialize(ar, "outerAngle", c.outerAngle);
        draconic::foundation::Serialize(ar, "shadowUpdate", shadowUpdate);
        draconic::foundation::Serialize(ar, "enabled", c.enabled);
        draconic::foundation::Serialize(ar, "castsShadows", c.castsShadows);
        c.type = static_cast<LightType>(type);
        c.shadowUpdate = static_cast<ShadowUpdateMode>(shadowUpdate);
    }

    inline void Serialize(ISerializer& ar, CameraComponent& c)
    {
        draconic::foundation::Serialize(ar, "fovYRadians", c.fovYRadians);
        draconic::foundation::Serialize(ar, "aspect", c.aspect);
        draconic::foundation::Serialize(ar, "nearZ", c.nearZ);
        draconic::foundation::Serialize(ar, "farZ", c.farZ);
        draconic::foundation::Serialize(ar, "clearColor", c.clearColor);
        draconic::foundation::Serialize(ar, "primary", c.primary);
    }

    inline void Serialize(ISerializer& ar, ReflectionProbeComponent& c)
    {
        u8 update = static_cast<u8>(c.update);
        draconic::foundation::Serialize(ar, "halfExtents", c.halfExtents);
        draconic::foundation::Serialize(ar, "blendDistance", c.blendDistance);
        draconic::foundation::Serialize(ar, "intensity", c.intensity);
        draconic::foundation::Serialize(ar, "resolution", c.resolution);
        draconic::foundation::Serialize(ar, "priority", c.priority);
        draconic::foundation::Serialize(ar, "update", update);
        draconic::foundation::Serialize(ar, "parallax", c.parallax);
        draconic::foundation::Serialize(ar, "enabled", c.enabled);
        c.update = static_cast<ProbeUpdateMode>(update);
    }

    // Persist the refs + the authored placement data (instances/tints are the authored content of a
    // scatter set); the skinning pose pool + caches are runtime-only. Loaded sets start at version 1
    // with boundsVersion 0, so the first extract re-uploads and recomputes bounds.
    inline void Serialize(ISerializer& ar, InstancedMeshComponent& c)
    {
        draconic::foundation::Serialize(ar, "mesh", c.mesh);
        draconic::foundation::Serialize(ar, "material", c.material);
        draconic::foundation::Serialize(ar, "instances", c.instances);
        draconic::foundation::Serialize(ar, "color", c.color);
        draconic::foundation::Serialize(ar, "tints", c.tints);
        draconic::foundation::Serialize(ar, "visible", c.visible);
    }
    inline void ResolveResources(draconic::resource::ResourceManager& manager,
                                 InstancedMeshComponent& c)
    {
        c.mesh.Bind(manager);
        c.material.Bind(manager);
    }

    class InstancedMeshComponentManager final
        : public scene::SerializableComponentManager<InstancedMeshComponent>
    {
    public:
        InstancedMeshComponentManager()
            : scene::SerializableComponentManager<InstancedMeshComponent>(u8"instanced_mesh")
        {
        }
    };
    // Persist the texture ref + plain fields; the raw view override is runtime-only.
    inline void Serialize(ISerializer& ar, SpriteComponent& c)
    {
        draconic::foundation::Serialize(ar, "texture", c.textureAsset);
        draconic::foundation::Serialize(ar, "size", c.size);
        draconic::foundation::Serialize(ar, "uvRect", c.uvRect);
        draconic::foundation::Serialize(ar, "tint", c.tint);
        u32 orientation = static_cast<u32>(c.orientation);
        draconic::foundation::Serialize(ar, "orientation", orientation);
        c.orientation = static_cast<SpriteOrientation>(orientation);
        draconic::foundation::Serialize(ar, "additive", c.additive);
        draconic::foundation::Serialize(ar, "visible", c.visible);
    }
    inline void ResolveResources(draconic::resource::ResourceManager& manager, SpriteComponent& c)
    {
        c.textureAsset.Bind(manager);
    }

    inline void Serialize(ISerializer& ar, DecalComponent& c)
    {
        draconic::foundation::Serialize(ar, "texture", c.textureAsset);
        draconic::foundation::Serialize(ar, "size", c.size);
        draconic::foundation::Serialize(ar, "color", c.color);
        draconic::foundation::Serialize(ar, "fadeStart", c.fadeStart);
        draconic::foundation::Serialize(ar, "fadeEnd", c.fadeEnd);
        draconic::foundation::Serialize(ar, "visible", c.visible);
    }
    inline void ResolveResources(draconic::resource::ResourceManager& manager, DecalComponent& c)
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

    // SkyMode is defined in the snapshot layer (draconic.render :data) and reused here.

    // The scene's environment - ONE per scene (not a component). Drives both the IBL ambient and the
    // (upcoming) visible sky. A plain SceneSystem injected by the RenderSubsystem; extraction reads it
    // into the snapshot. When IBL is active these sky settings drive shading; `ambientColor ×
    // ambientIntensity` remains the flat fallback used when no environment is active.
    struct EnvironmentSettings
    {
        // Flat ambient FILL: adds on top of the environment's image-based ambient in every sky
        // mode (intensity 0 = pure IBL); when IBL is unavailable it is the only ambient.
        Color ambientColor = Color{0.10f, 0.12f, 0.16f, 1.0f};
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
        draconic::resource::Ref<texture::Texture> skyTexture;
        // Procedural sky (Unity-default-like: a soft, hazy, low-saturation daytime blue rather than
        // a punchy vivid one - dimmer horizon, desaturated zenith, near-neutral ground).
        Color skyHorizon = Color{0.52f, 0.60f, 0.70f, 1.0f};
        Color skyZenith = Color{0.20f, 0.36f, 0.58f, 1.0f}; // also the Color-mode color
        Color skyGround = Color{0.26f, 0.26f, 0.26f, 1.0f};
        f32 sunIntensity = 1.0f;
        f32 sunAngularSize = 0.5f; // sun disc size (degrees)
        f32 turbidity = 3.0f;      // Analytic (Preetham) haze (~2..10)
    };

    class EnvironmentSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] EnvironmentSettings& Environment() noexcept { return m_env; }
        [[nodiscard]] const EnvironmentSettings& Environment() const noexcept { return m_env; }

        // Scene-level settings seam: the editor's scene inspector edits m_env through the
        // reflected type; SerializeScene persists it (wrapped in the type's versioned payload -
        // reflection registration stamps dataVersion 1, so future fields gate on ar.Version()).
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<EnvironmentSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_env; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"environment"; }
        void ResolveResources(draconic::resource::ResourceManager& manager) override
        {
            m_env.skyTexture.Bind(manager);
        }
        void SerializeSettings(ISerializer& ar) override
        {
            if (ar.Version() >= 2)
            { // v2 added the sky texture reference
                draconic::foundation::Serialize(ar, "skyTexture", m_env.skyTexture);
            }
            draconic::foundation::Serialize(ar, "ambientColor", m_env.ambientColor);
            draconic::foundation::Serialize(ar, "ambientIntensity", m_env.ambientIntensity);
            u32 mode = static_cast<u32>(m_env.skyMode);
            draconic::foundation::Serialize(ar, "skyMode", mode);
            if (ar.Mode() == SerializeMode::Read)
            {
                m_env.skyMode = static_cast<SkyMode>(mode);
            }
            draconic::foundation::Serialize(ar, "skyIntensity", m_env.skyIntensity);
            if (ar.Version() >= 3)
            { // v3 split the visible-sky backdrop dimmer out of skyIntensity
                draconic::foundation::Serialize(ar, "skyBackgroundIntensity",
                                          m_env.skyBackgroundIntensity);
            }
            draconic::foundation::Serialize(ar, "skyRotation", m_env.skyRotation);
            draconic::foundation::Serialize(ar, "skyHorizon", m_env.skyHorizon);
            draconic::foundation::Serialize(ar, "skyZenith", m_env.skyZenith);
            draconic::foundation::Serialize(ar, "skyGround", m_env.skyGround);
            draconic::foundation::Serialize(ar, "sunIntensity", m_env.sunIntensity);
            draconic::foundation::Serialize(ar, "sunAngularSize", m_env.sunAngularSize);
            draconic::foundation::Serialize(ar, "turbidity", m_env.turbidity);
        }

    private:
        EnvironmentSettings m_env;
    };

    // ============================================================================================
    // Post-processing (docs/design/post-processing-config.md): the authored "look" of a scene -
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

        // Anti-aliasing (aaMode selects the exclusive path; the others' params are ignored).
        AaMode aaMode = AaMode::Off;
        f32 taaBlendFactor = 0.97f;   // history weight   (aaMode == TAA)
        f32 taaVarianceGamma = 1.25f; // variance-clip box half-width (aaMode == TAA)
        f32 fxaaSubpixel = 0.75f;     // subpixel aliasing removal    (aaMode == FXAA)
    };

    class PostProcessSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] PostProcessSettings& Post() noexcept { return m_post; }
        [[nodiscard]] const PostProcessSettings& Post() const noexcept { return m_post; }

        // Scene-settings seam (same shape as EnvironmentSystem): the inspector edits m_post through
        // the reflected type; SerializeScene persists it (versioned payload; future fields gate on
        // ar.Version()).
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<PostProcessSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_post; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"postprocess"; }
        void SerializeSettings(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "exposureEV", m_post.exposureEV);
            SerializeEnum(ar, "tonemapOperator", m_post.tonemapOperator);
            draconic::foundation::Serialize(ar, "bloomEnabled", m_post.bloomEnabled);
            draconic::foundation::Serialize(ar, "bloomThreshold", m_post.bloomThreshold);
            draconic::foundation::Serialize(ar, "bloomKnee", m_post.bloomKnee);
            draconic::foundation::Serialize(ar, "bloomIntensity", m_post.bloomIntensity);
            SerializeEnum(ar, "aoMode", m_post.aoMode);
            draconic::foundation::Serialize(ar, "aoStrength", m_post.aoStrength);
            draconic::foundation::Serialize(ar, "aoRadius", m_post.aoRadius);
            draconic::foundation::Serialize(ar, "aoIntensity", m_post.aoIntensity);
            draconic::foundation::Serialize(ar, "ssrEnabled", m_post.ssrEnabled);
            draconic::foundation::Serialize(ar, "ssrIntensity", m_post.ssrIntensity);
            SerializeEnum(ar, "aaMode", m_post.aaMode);
            draconic::foundation::Serialize(ar, "taaBlendFactor", m_post.taaBlendFactor);
            draconic::foundation::Serialize(ar, "taaVarianceGamma", m_post.taaVarianceGamma);
            draconic::foundation::Serialize(ar, "fxaaSubpixel", m_post.fxaaSubpixel);
        }

    private:
        // u32-round-trip an enum field (mirrors EnvironmentSystem's skyMode handling).
        template <typename E>
        static void SerializeEnum(ISerializer& ar, const char* key, E& value)
        {
            u32 raw = static_cast<u32>(value);
            draconic::foundation::Serialize(ar, key, raw);
            if (ar.Mode() == SerializeMode::Read)
            {
                value = static_cast<E>(raw);
            }
        }
        PostProcessSettings m_post;
    };

    // Resolve a scene's authored PostProcessSettings into the renderer's per-view ViewPostConfig:
    // exposure EV/stops -> the tonemap's linear multiplier (2^EV), authoring enums -> pass primitives.
    // `needsMotion` gets only the TAA part here (SSR's temporal contribution depends on the frame-global
    // SsrPass::Params, so the caller ORs it in).
    [[nodiscard]] inline ViewPostConfig ResolveScenePost(const PostProcessSettings& s)
    {
        ViewPostConfig vp;
        vp.exposure = draconic::foundation::Pow(2.0f, s.exposureEV);
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
        vp.needsMotion = vp.taaEnabled; // caller ORs in (ssrEnabled && ssr-temporal)
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
        if (o.disablePost || o.disableAa)
        {
            vp.taaEnabled = false;
            vp.fxaaEnabled = false;
        }
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
            return system != nullptr ? &system->Environment() : nullptr;
        }
        [[nodiscard]] inline void* ResolvePostProcessSettings(const Variant& value)
        {
            const auto* ctx = static_cast<const SceneSettingsCtx*>(value.ResolveContext());
            if (ctx == nullptr || ctx->scene == nullptr)
            {
                return nullptr;
            }
            PostProcessSystem* system = ctx->scene->GetSystem<PostProcessSystem>();
            return system != nullptr ? &system->Post() : nullptr;
        }
    }

    // EnvironmentSettings.of(scene): a re-resolving handle over the scene's live environment settings
    // (empty if the scene has no EnvironmentSystem). Reflected on EnvironmentSettings with the
    // ReturnType-override so the declared script return IS EnvironmentSettings.
    [[nodiscard]] inline Variant EnvironmentSettingsOf(draconic::script::Scene sceneHandle)
    {
        return Variant::Resolving(&TypeOf<EnvironmentSettings>(),
                                  &settingsref::ResolveEnvironmentSettings,
                                  settingsref::SceneSettingsCtx{sceneHandle.scene});
    }
    // PostProcessSettings.of(scene): a re-resolving handle over the scene's live post settings.
    [[nodiscard]] inline Variant PostProcessSettingsOf(draconic::script::Scene sceneHandle)
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

        // Swap the entity's MeshComponent mesh to resource `id`, binding it through the run's
        // resource manager so the swap takes effect live (a bare VM with no manager sets the id
        // only, unbound). No-op if the scene is null or the entity has no MeshComponent.
        void setMesh(draconic::script::Entity entity, Guid id) const
        {
            MeshComponent* mesh = MeshOf(entity);
            if (mesh == nullptr)
            {
                return;
            }
            mesh->mesh.SetId(id);
            if (auto* resources = draconic::script::CurrentRunResources())
            {
                mesh->mesh.Bind(*resources);
            }
        }

        // Swap the entity's slot-0 material (single-material meshes / the whole-mesh slot) to
        // resource `id`, binding it through the run's resource manager.
        void setMaterial(draconic::script::Entity entity, Guid id) const
        {
            MeshComponent* mesh = MeshOf(entity);
            if (mesh == nullptr)
            {
                return;
            }
            if (mesh->materials.IsEmpty())
            {
                mesh->materials.PushBack(draconic::resource::Ref<materials::Material>{});
            }
            mesh->materials[0].SetId(id);
            if (auto* resources = draconic::script::CurrentRunResources())
            {
                mesh->materials[0].Bind(*resources);
            }
        }

        [[nodiscard]] static SceneRender of(draconic::script::Scene sceneHandle)
        {
            return SceneRender{sceneHandle.scene};
        }

    private:
        [[nodiscard]] MeshComponent* MeshOf(draconic::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            MeshComponentManager* meshes = scene->GetSystem<MeshComponentManager>();
            return (meshes != nullptr) ? meshes->Get(entity.Handle()) : nullptr;
        }
    };

} // namespace draconic::render (exported)

// Registers all render component/enum reflection (idempotent). Called by RenderSubsystem::OnInit
// so every app with a renderer gets reflected components for free. Non-inline: the body touches
// module-linkage registration functions, which an exported inline definition may not.
export namespace draconic::render
{
    void RegisterRenderComponentReflection();

    // Surfaces the render components to SCRIPT (Track A): registers MeshComponent/LightComponent
    // in the global type registry, seeds their Wren emission roots, and makes their class names
    // import-visible in behavior/Level preludes, so `MeshComponent.of(entity).visible = false` etc.
    // work on both backends. Called by the composition root (like RegisterPhysicsScriptFacade).
    void RegisterRenderScriptFacade();
}
