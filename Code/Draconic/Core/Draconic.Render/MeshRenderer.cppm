/// Draconic::Render - the `:mesh_renderer` partition.
///
/// `MeshRenderer` is the `Renderer` for the mesh categories (Opaque/Masked/Transparent). It
/// owns the built-in forward shader (two permutations), the GPU rings, and the mesh cache,
/// and records draws for the mesh `DrawItem`s it is handed.
///
/// HYBRID instancing (§8): a run of consecutive draws sharing one mesh + material is issued as
/// a single INSTANCED draw; a lone draw takes the simpler per-object path. Both read the
/// view-projection from a shared per-view UBO (set 0). The per-object path adds a per-object
/// UBO (set 1, world + tint, dynamic offset); the instanced path adds a per-instance
/// `StructuredBuffer<InstanceData>` (set 1) indexed by a uint4 DataOffsets vertex stream
/// (location 5, instance-stepped) - the portable base+offset addressing (NOT SV_InstanceID,
/// which differs between DX12 and Vulkan). Transparent draws are never instanced (back-to-front
/// order must dominate). Material set-2 binding + real lighting are later phases.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:mesh_renderer;

import draconic.foundation;
import draconic.rhi;
import draconic.geometry;
import draconic.shaders;
import draconic.shaders.system;
import draconic.materials;
import draconic.materials.pipelinecache;
import :data;
import :views;
import :pipeline;
import :cluster_system;
import :resources;
import :gpu_mesh;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    class MeshRenderer final : public Renderer
    {
    public:
        MeshRenderer(rhi::Device& device, shaders::ShaderSystem& shaderSystem,
                     materials::PipelineStateCache& psoCache,
                     materials::MaterialSystem& materialSystem, u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaderSystem), m_psoCache(&psoCache),
              m_materials(&materialSystem), m_meshes(device),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight),
              m_viewRing(device, framesInFlight, kViewDataSlot,
                         rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"mesh.view"),
              m_shadowViewRing(device, framesInFlight, kViewSlot,
                               rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
                               u8"mesh.shadowView"),
              m_objectRing(device, framesInFlight, kViewSlot,
                           rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"mesh.object"),
              m_instanceRing(device, framesInFlight, sizeof(InstanceData),
                             rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst,
                             u8"mesh.instances"),
              m_offsetsRing(device, framesInFlight, sizeof(DataOffsets),
                            rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst, u8"mesh.offsets"),
              m_lightRing(device, framesInFlight, sizeof(GpuLight),
                          rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst, u8"mesh.lights"),
              m_localShadowRing(device, framesInFlight, sizeof(GpuLocalShadow),
                                rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst,
                                u8"mesh.localShadows"),
              m_boneRing(device, framesInFlight, sizeof(Float4x4),
                         rhi::BufferUsage::CopySrc,
                         u8"mesh.bones.staging")
        {
        }

        ~MeshRenderer() override { Shutdown(); }

        MeshRenderer(const MeshRenderer&) = delete;
        MeshRenderer& operator=(const MeshRenderer&) = delete;

        // Registers the forward shader + creates the bind-group / pipeline layouts.
        Status Initialize();

        // The directional shadow map sampled in the forward shader this frame (the ShadowSystem's
        // texture when a caster exists, else null -> the 1x1 dummy). Set by RenderFrame before PrepareFrame.
        void SetShadowMap(rhi::TextureView* view, u64 generation) override;

        // The local-light (spot/point) shadow atlas sampled this frame (the ShadowSystem's atlas when any
        // local caster exists, else null -> the 1x1 dummy). Set by RenderFrame before PrepareFrame.
        void SetShadowAtlas(rhi::TextureView* view, u64 generation, u32 passCount) override;

        // Reflection-probe capture faces re-emit the draws (one forward pass each) - count them into the ring.
        void SetCaptureFacePasses(u32 passes) override { m_captureFacePasses = passes; }

        // This frame's active reflection probe (P2, single probe): the captured cube-ARRAY view (set-0 t8) +
        // the probe's box/slice/intensity/count for the forward's local-reflection path. null view => dummy
        // cube-array + count 0 (the forward keeps the global IBL reflection).
        void SetProbes(rhi::TextureView* cubeArray, rhi::Buffer* probeBuffer, u32 count) override;

        // This frame's IBL products (SH9 diffuse buffer + prefiltered specular cube + BRDF LUT), bound in
        // set 0. null views -> the neutral 1x1 dummies (zero SH + black cube => flat fallback ambient).
        // Upload this frame's local-shadow entries into the local-shadow ring (bound whole at set 0;
        // the shader reads LocalShadows[LocalShadowBase + shadowIndex]). Called once per frame (after
        // PrepareFrame, which begins the ring). The base is stamped into each view's ViewData in Resolve.
        void UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex) override;

        // ---- Renderer ----

        [[nodiscard]] Span<const RenderCategory> SupportedCategories() const override;

        // Size every ring for the whole frame's draws (once) + select this frame's region. The
        // per-object/instance rings are sized for 2x the camera draws so the shadow depth pass can
        // re-emit the same geometry into the same rings without starving the forward pass.
        void PrepareFrame(u32 maxDraws, u32 frameIndex) override;

        // Device-local mirror of the bone staging ring: vertex skinning reads bones across many passes
        // (forward + every cascade), so a CpuToGpu buffer would stream them over PCIe each read. One copy
        // per frame into VRAM (UploadSkinning) makes subsequent reads land at device bandwidth.
        bool EnsureBoneDevice();

        // Write every distinct skinned instance's matrices into the bone pool ONCE this frame (current
        // slab then previous slab, per Sedulous's [cur][prev] layout) + copy the populated range to the
        // device mirror. Builds m_boneStart: boneMatrices ptr -> { current base, prev base } in MATRIX
        // units, which Resolve uses for the per-draw bone base (DataOffsets.y/.z later). Called once per
        // frame before any pass; replaces the old per-pass MemCopy into the ring.
        void UploadSkinning(const ExtractedScene& scene, rhi::CommandEncoder& encoder) override;

        // Ensure every instanced-mesh SET in the snapshot has an up-to-date persistent GPU buffer, and grow
        // the shared DataOffsets ramp to the largest set. Called once per frame (from UploadSkinning). Static
        // sets fall through instantly after their first upload - that O(1)/frame path is the whole point.
        void UploadMultiMeshes(const ExtractedScene& scene);

        // Grow the shared DataOffsets ramp to at least `count` slots: [{0,0,0,0},{1,0,0,0},...]. Filled once
        // per (re)allocation (values are static per index - never rewritten). .x is each instance's index into
        // its set's own StructuredBuffer<InstanceData>; .y/.z (bone bases) stay 0 - MultiMesh v1 is non-skinned.
        bool EnsureRamp(u32 count);

        // Ensure this set's persistent InstanceData buffer + set-1 bind group exist and hold the current
        // instances. (Re)allocates + rebuilds the bind group on first sight or a grow (draining the GPU
        // first - a resize is rare); re-uploads InstanceData only when the component's version changed
        // (static sets upload exactly once). InstanceData.prevWorld = world (static content has no motion).
        void EnsureMultiMeshSet(const MultiMeshRenderData& mm);

        // Fill each skinned MultiMesh set's per-instance DataOffsets: .x = instance index into its InstanceData
        // buffer, .y/.z = the instance's pose base in the shared bone pool = poolBase + (Hash(i) % poseCount)*boneCount.
        // Runs after the bone upload (pool bases known). O(N) 16-byte writes, once per frame, shared across passes.
        void FillSkinnedMultiMeshOffsets(const ExtractedScene& scene);

        void Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                     Array<ResolvedDraw>& out) override;

        // Re-emit this view's draws as DEPTH-ONLY shadow casters (ctx.viewProj = light view-proj,
        // ctx.depthFormat = shadow map format). Mirrors Resolve's run batching but produces depth-only
        // ResolvedDraws (set 0 = light view, set 1 = object/instance; no material/cluster). Reuses the
        // object/instance rings + their bind groups (sized 2x in PrepareFrame).
        void ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                              Array<ResolvedDraw>& out) override;

        void FinishFrame() override;

        /// Wire the frames-in-flight retire queue (web-safe grows for the device buffers
        /// AND the staging rings). Null = drain-on-grow.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept
        {
            m_retire = retire;
            m_viewRing.SetRetireQueue(retire);
            m_shadowViewRing.SetRetireQueue(retire);
            m_objectRing.SetRetireQueue(retire);
            m_instanceRing.SetRetireQueue(retire);
            m_offsetsRing.SetRetireQueue(retire);
            m_lightRing.SetRetireQueue(retire);
            m_localShadowRing.SetRetireQueue(retire);
            m_boneRing.SetRetireQueue(retire);
        }

    private:
        void RetireOrDrainBuffer(rhi::Buffer*& buffer);
        GpuRetireQueue* m_retire = nullptr; // borrowed; null = WaitIdle on grow
        struct ViewData
        {                                // 528 (matches the View cbuffer)
            Float4x4 viewProj;           // 64
            Float4x4 view;               // 64  (view-space depth: cluster + cascade select)
            Float4x4 cascadeViewProj[4]; // 256 (CSM: world -> each cascade's light clip)
            Float3 cameraPos;
            f32 lightCount; // 16  (light count as float, mirrors HLSL)
            u32 lightOffset;
            i32 clusterViewportX, clusterViewportY;
            f32 iblMaxLod = -1.0f; // 16 (iblMaxLod<0 => no IBL)
            u32 clusterGridX = 0, clusterGridY = 0, clusterSliceCount = 0,
                clusterTileSize = 0;                                                      // 16
            f32 clusterNear = 0, clusterFar = 0, clusterLogScale = 0, clusterLogBias = 0; // 16
            Float3 ambient = Float3{0, 0, 0};
            f32 shadowCascadeCount = 0.0f;                // 16
            Float4 cascadeSplitFar = Float4{0, 0, 0, 0};  // 16
            Float4 cascadeTexelSize = Float4{0, 0, 0, 0}; // 16
            f32 shadowNormalBias = 0, shadowDepthBias = 0, cascadeLayerBase = 0;
            u32 localShadowBase = 0; // 16
            Float4x4 prevViewProj =
                Float4x4::Identity(); // 64  (motion vectors: last frame's world->clip)
            Float4 jitter =
                Float4{0, 0, 0, 0}; // 16  (xy = this frame's NDC jitter, zw = last frame's)
            Float4 probeCenter =
                Float4{0, 0, 0, 0}; // 16  (xyz = probe center, w = probe count [0 = none])
            Float4 probeBoxMin = Float4{0, 0, 0, 0}; // 16  (xyz = box min, w = probe cube slice)
            Float4 probeBoxMax = Float4{0, 0, 0, 0}; // 16  (xyz = box max, w = probe intensity)
            Float4 shadowParams =
                Float4{40.0f, 0, 0, 0}; // 16  (x = CSM far-fade width in world units; yzw spare)
        };
        struct ObjectData
        {
            Float4x4 world;
            Float4x4 prevWorld;
            Color tint;
            u32 boneBase = 0, prevBoneBase = 0, p1 = 0, p2 = 0;
        }; // 160 (cbuffer Object)
        struct InstanceData
        {
            Float4x4 world;
            Float4x4 prevWorld;
            Color tint;
        }; // 144 (StructuredBuffer element)
        struct DataOffsets
        {
            u32 x, y, z, w;
        }; // 16  (instance-stepped vertex attr)
        struct ShadowViewData
        {
            Float4x4 lightViewProj;
        }; // 64  (cbuffer ShadowView)
        // GPU-layout contract: these mirror HLSL cbuffer/StructuredBuffer elements and use the PACKED
        // Float4x4 (64B, tight). A stray SIMD Matrix4 (also 64B but 16-byte aligned) would still trip the
        // size math via padding shifts - the guards pin the exact byte layout the shaders expect.
        static_assert(sizeof(ObjectData) == 160, "cbuffer Object layout drift");
        static_assert(sizeof(InstanceData) == 144,
                      "StructuredBuffer<InstanceData> element layout drift");
        static_assert(sizeof(ShadowViewData) == 64, "cbuffer ShadowView layout drift");

        static constexpr u64 kViewSlot = 256; // dynamic UBO offset alignment (object/shadow-view)
        static constexpr u64 kViewDataSlot =
            1024;                              // view UBO slot (ViewData is 528B with CSM cascades)
        static constexpr u32 kMaxLights = 256; // per-view light budget (phase 4.1; clustered later)
        // shadow-view UBO slots per frame: one per (shadow pass × category run). Cascades (up to
        // kMaxShadowViews*kCount) + local-shadow atlas tiles (up to kMaxLocalShadows), each × a few
        // categories. Sized with headroom - a slot is tiny (256B).
        static constexpr u32 kMaxShadowPasses = 256;
        static constexpr u32 kMaxLocalShadows =
            kMaxLocalShadowEntries; // frame-global entry cap (shared with the pipeline)
        static constexpr u32 kMaxBoneMatrices =
            1u << 20; // GPU skinning bone-matrix pool slots per frame.
                      // The current arch re-uploads each caster's bones
                      // PER PASS (forward + 4 CSM cascades + local), so
                      // usage is ~N*bones*(5+); sized big so the stress
                      // test doesn't overflow (the persistent-buffer
                      // rewrite uploads once, shared across passes).

        void ResolveSingle(const RenderRecordContext& ctx, u32 viewOffset,
                           rhi::BindGroup* clusterBG, const MeshRenderData& md, const GpuMesh& mesh,
                           Array<ResolvedDraw>& out);

        void ResolveInstanced(const RenderRecordContext& ctx, u32 viewOffset,
                              rhi::BindGroup* clusterBG, Span<const DrawItem> items, usize first,
                              u32 count, const MeshRenderData& head, const GpuMesh& mesh,
                              Array<ResolvedDraw>& out);

        void ResolveDepthSingle(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                const MeshRenderData& md, const GpuMesh& mesh,
                                Array<ResolvedDraw>& out);

        void ResolveDepthInstanced(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                   Span<const DrawItem> items, usize first, u32 count,
                                   const GpuMesh& mesh, Array<ResolvedDraw>& out);

        // Forward draw for a MultiMesh: bind THIS set's persistent InstanceData (set 1) + the shared DataOffsets
        // ramp, one instanced draw per submesh material. NO fill loop - the buffer already holds the instances
        // (uploaded once in UploadMultiMeshes). The shader is the identical instanced path (Instances[DataOffsets.x]).
        void ResolveMultiMesh(const RenderRecordContext& ctx, u32 viewOffset,
                              rhi::BindGroup* clusterBG, const MultiMeshRenderData& mm,
                              const GpuMesh& mesh, Array<ResolvedDraw>& out);

        // Depth-only draw for a MultiMesh caster (camera prepass + every shadow cascade): the same persistent
        // buffer + shared ramp, the depth/shadow pipeline layout. Non-masked casters are one draw; masked
        // casters fold in the material set for the alpha test (like ResolveDepthInstanced).
        void ResolveMultiMeshDepth(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                   const MultiMeshRenderData& mm, const GpuMesh& mesh,
                                   Array<ResolvedDraw>& out);

        // Depth-only PSO config for the shadow pass. Back-face cull + a small depth bias/slope to push
        // shadow acne off lit surfaces (tuned on GPU; 5.2 refines with normal-offset bias in the shader).
        [[nodiscard]] static materials::PipelineConfig
        ShadowConfigFor(const RenderRecordContext& ctx, bool instanced, bool masked = false);

        [[nodiscard]] static materials::PipelineConfig
        ConfigFor(const MeshRenderData& md, const RenderRecordContext& ctx, bool instanced);

        bool MakeLayout(const rhi::BindGroupLayoutEntry& entry, rhi::BindGroupLayout*& out);

        bool MakePipelineLayout(rhi::BindGroupLayout* set0, rhi::BindGroupLayout* set1,
                                rhi::BindGroupLayout* set2, rhi::BindGroupLayout* set3,
                                rhi::PipelineLayout*& out);

        // Two-set pipeline layout (the depth-only shadow pipelines: light-view + object/instance).
        bool MakePipelineLayout(rhi::BindGroupLayout* set0, rhi::BindGroupLayout* set1,
                                rhi::PipelineLayout*& out);

        // Three-set pipeline layout (masked shadow: light-view + object/instance + material) - the material
        // set feeds the alpha-test fragment's albedo sample.
        bool MakePipelineLayout3(rhi::BindGroupLayout* set0, rhi::BindGroupLayout* set1,
                                 rhi::BindGroupLayout* set2, rhi::PipelineLayout*& out);

        // Masked-shadow pipeline layout for a material set-2 layout, cached by (set2, instanced): light-view
        // (0) + object/instance (1) + material (2). Retired with the renderer.
        [[nodiscard]] rhi::PipelineLayout* GetOrCreateShadowMaskedLayout(rhi::BindGroupLayout* set2,
                                                                         bool instanced);

        // The default material - a standard PBR material (materials::CreatePBR) used whenever a draw
        // carries no material. Its set-2 layout/bind group flow through the same data-driven path as any
        // other material, so there is nothing special about the "no material" case.
        Status CreateDefaultMaterial();

        // The MaterialInstance the renderer owns for `material` (one per material, created lazily). The
        // instance carries per-draw overrides (textures/uniforms) + caches its bind group in the system.
        [[nodiscard]] materials::MaterialInstance* InstanceFor(materials::Material* material);

        // The set-2 (material) bind group for `material`, built data-driven from its property list by the
        // material system (UBO + textures/samplers in declared order, with white/flat-normal/default-sampler
        // fallbacks for unset slots). `material` is non-null (callers substitute the default material).
        [[nodiscard]] rhi::BindGroup* MaterialBindGroup(materials::Material* material);

        // The forward pipeline layout for a given material set-2 layout, cached by (set2 layout, instanced).
        // set0 = view, set1 = object (single) / instances (instanced), set2 = material, set3 = clusters.
        [[nodiscard]] rhi::PipelineLayout* GetOrCreatePipelineLayout(rhi::BindGroupLayout* set2,
                                                                     bool instanced);

        // Per-frame bind groups (view/shadow-view/object/instance) are rebuilt when their inputs change -
        // e.g. the directional shadow view alternates per in-flight slot, so the view BG rebuilds every
        // frame. The OLD bind group may still be referenced by an in-flight command buffer, so it can't be
        // freed immediately (vkFreeDescriptorSets-00309): retire it and free after the frame ring cycles.
        void RetireBindGroup(rhi::BindGroup* bg);
        // Drop material instances whose material nobody else references (the resource layer
        // hot-reloaded it away): retire their bind group (may still be bound by in-flight
        // frames) and destroy the instance. Without this, stale descriptor sets keep destroyed
        // texture views referenced forever.
        void PruneStaleMaterialInstances();

        void TickRetiredBindGroups();

        void RetireBuffer(rhi::Buffer* buffer);

        // (Re)create a bind group over a ring's buffer when the ring (re)allocated. `whole` binds
        // the entire buffer (storage, indexed); otherwise a `bindSize` window (dynamic-offset UBO).
        bool EnsureBindGroup(DynamicUniformRing& ring, rhi::BindGroupLayout* layout, u64 bindSize,
                             rhi::BindGroup*& bg, u32& bgGen, bool whole);

        // The set-0 bind group spans two rings: the per-view UBO (dynamic-offset window of one
        // ViewData) + the light list (whole light buffer, read as Lights[lightOffset + i]). PER-VIEW
        // SLOTS: views of different scenes bind different IBL products in one frame (per-scene sky),
        // so each view index caches its own group - keyed by the ring generations, the shadow/atlas
        // generations, and the IBL context generation (unique across contexts, [[bind-group-cache-
        // versioning]]). Sets m_viewBG (the CURRENT view's group) for the Resolve* bodies.
        bool EnsureViewBindGroup(const RenderRecordContext& ctx);

        // The comparison sampler (hardware PCF) + a 1x1 dummy depth map bound into set 0 when no shadow
        // caster exists this frame (so the descriptor set is always complete).
        Status CreateShadowResources();

        // The set-0 bind group for the shadow depth pass: just the light-view UBO (dynamic-offset
        // window of one ShadowViewData). Rebuilt when the shadow-view ring (re)allocated.
        bool EnsureShadowViewBindGroup();

        // Tiny placeholder cluster buffers + a set-3 bind group over them, bound when clustering is
        // unavailable. The shader's ClusterGridX==0 path never reads them, but set 3 must be bound.
        Status CreateDummyClusters();

        // The set-3 bind group for a frame-in-flight slot, over the ClusterSystem's per-frame cluster
        // buffers. One per slot (the buffers alternate per frame) so a slot's group is stable.
        // Rebuild when the cluster buffers' VERSION changes (the ClusterSystem bumps it on realloc).
        // Pointer identity is NOT a safe "unchanged" test: a freed rhi::Buffer* address can be reused by
        // the new allocation, so a stale bind group would point at a destroyed VkBuffer (use-after-free).
        rhi::BindGroup* EnsureClusterBindGroup(u32 slot, rhi::Buffer* offsets, rhi::Buffer* indices,
                                               u32 version);

        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        materials::PipelineStateCache* m_psoCache;
        materials::MaterialSystem* m_materials;
        GpuMeshCache m_meshes;
        u32 m_framesInFlight = 2;
        u32 m_frameIndex = 0; // this frame's index (for FiF-slotting the offsets buffer)

        rhi::BindGroupLayout* m_viewLayout = nullptr;
        rhi::BindGroupLayout* m_objectLayout = nullptr;
        rhi::BindGroupLayout* m_instanceLayout = nullptr;
        rhi::BindGroupLayout* m_clusterLayout = nullptr;
        rhi::BindGroupLayout* m_shadowViewLayout =
            nullptr; // set 0 for the depth-only shadow pipeline
        rhi::PipelineLayout* m_shadowPipelineLayoutSingle = nullptr;
        rhi::PipelineLayout* m_shadowPipelineLayoutInstanced = nullptr;
        // Forward pipeline layouts, keyed by (material set-2 layout, instanced); built on demand so each
        // material's own set-2 layout drives the PSO (material-driven; supports custom shaders).
        HashMap<u64, rhi::PipelineLayout*> m_pipelineLayouts;
        HashMap<u64, rhi::PipelineLayout*>
            m_shadowMaskedLayouts; // masked-shadow 3-set layouts (by set-2, instanced)

        // Material set-2 resources. The default material (standard PBR) backs draws with no material;
        // instances carry per-material overrides and flow through the material system's data-driven BG path.
        RefPtr<materials::Material> m_defaultMaterial;
        HashMap<u64, materials::MaterialInstance*> m_instances;          // lookup by Material::uid
        Array<UniquePtr<materials::MaterialInstance>> m_instanceStorage; // ownership

        DynamicUniformRing m_viewRing;
        DynamicUniformRing m_shadowViewRing;
        DynamicUniformRing m_objectRing;
        DynamicUniformRing m_instanceRing;
        DynamicUniformRing m_offsetsRing;
        DynamicUniformRing m_lightRing;
        DynamicUniformRing m_localShadowRing; // per-frame GpuLocalShadow entries (spot/point atlas)
        DynamicUniformRing m_boneRing; // bone-matrix STAGING ring (CpuToGpu; written once/frame)
        rhi::Buffer* m_boneDevice = nullptr; // GpuOnly device mirror the VS reads (set-0 t4 SRV)
        u64 m_boneDeviceBytes = 0;
        u32 m_boneDeviceGen = 0; // bumps on (re)create -> invalidates set-0 bind groups
        // Per-frame map: a skinned instance's boneMatrices pointer -> its bases (matrix units) in the pool.
        struct BoneSlot
        {
            u32 base = 0;
            u32 prevBase = 0;
        };
        // cur/prev = palette pointers, count = matrices in `cur`. hasPrev: per-entity casters write a cur+prev
        // slab (motion vectors); a skinned-MultiMesh POSE POOL writes just its M*boneCount palettes (no prev).
        struct SkinnedRef
        {
            const Float4x4* cur;
            const Float4x4* prev;
            u32 count;
            bool hasPrev = true;
        };
        HashMap<const Float4x4*, BoneSlot> m_boneStart;
        Array<SkinnedRef> m_skinnedScratch;

        // Per-entity previous-frame world matrix, for rigid-object motion vectors. FLAT double-buffer indexed
        // by entity INDEX (entityId low 32 bits) - not a hashmap: direct O(1) index, no hashing/probing/rehash
        // (the per-instance Find was the motion-vector hot cost at scale). m_prevWorld holds LAST frame's worlds
        // (read by every view this frame); resolves write THIS frame's into m_curWorld; the two swap at
        // FinishFrame. Out-of-range / never-written -> prev == cur (no motion), so newly-visible objects don't
        // smear on their first frame.
        Array<Float4x4> m_prevWorld;
        Array<Float4x4> m_curWorld;

        // Record `cur` as this frame's world (idempotent across a frame's views - same value each time) and
        // return the entity's previous-frame world (or `cur` if unknown). Indexed by entity index so multiple
        // views resolving the same object read a STABLE prev (writing cur never clobbers prev - separate buffers).
        [[nodiscard]] Float4x4 PrevWorldFor(u64 entityId, const Float4x4& cur);

        // Camera depth-prepass -> forward instance sharing. The prepass fills the FULL InstanceData (world +
        // real prevWorld + tint) for each opaque (mesh,material) group once and records the group's DataOffsets
        // range here; the forward looks it up and REUSES that instance data instead of re-filling it (build once,
        // not twice). Keyed by (viewIndex, mesh, material); cleared each frame. Instanced path only.
        struct InstShare
        {
            u64 offsByteOffset = 0;
            u32 count = 0;
        };
        HashMap<u64, InstShare> m_instShareCache;
        // Keyed by the VIEW pointer (not viewIndex): the main view's prepass + forward share one RenderView, but
        // probe-capture forwards reuse viewIndex 0 with a different draw list - a distinct pointer avoids collision.
        static u64 InstShareKey(const void* view, const void* mesh, const void* mat) noexcept;

        // Bind groups retired this/prior frames but possibly still referenced by in-flight command
        // buffers; freed by TickRetiredBindGroups once the frame ring has cycled (framesLeft hits 0).
        struct RetiredBG
        {
            rhi::BindGroup* bg;
            u32 framesLeft;
        };
        Array<RetiredBG> m_retiredBGs;
        struct RetiredBuffer
        {
            rhi::Buffer* buffer;
            u32 framesLeft;
        };
        Array<RetiredBuffer> m_retiredBuffers; // material uniform buffers of pruned instances

        // Per-view set-0 slots (views of different scenes bind different IBL products) + the
        // CURRENT view's group (set by EnsureViewBindGroup; read by the Resolve* bodies).
        struct ViewBGSlot
        {
            rhi::BindGroup* bg = nullptr;
            u32 viewGen = 0, lightGen = 0, localGen = 0, boneGen = 0;
            rhi::TextureView* shadow = nullptr;
            u64 shadowGen = 0;
            rhi::TextureView* atlas = nullptr;
            u64 atlasGen = 0;
            u64 iblGen = 0;
            rhi::TextureView* prefilter = nullptr;
            rhi::TextureView* probeCube = nullptr;
            rhi::Buffer* probeBuf = nullptr;
        };
        Array<ViewBGSlot> m_viewBGs;
        rhi::BindGroup* m_viewBG = nullptr; // current view's (borrowed from its slot)
        rhi::BindGroup* m_shadowViewBG = nullptr;
        u32 m_shadowViewBGGen = 0;
        u32 m_shadowViewBGBoneGen = 0;
        rhi::BindGroup* m_objectBG = nullptr;
        rhi::BindGroup* m_instanceBG = nullptr;
        // The set-0 bind group spans the view UBO + light SB + shadow map + sampler; rebuild it when any
        // of those change (rings roll over, or the active shadow map view changes).
        // The set-0 shadow binding is cached by (view pointer, ShadowSystem generation): a freed shadow
        // texture's view address can be reused by a recreated one (5.2 atlas/resolution changes), so the
        // generation - not the pointer alone - is what reliably invalidates the bind group. See the
        // ClusterBinding::version + ShadowSystem::Generation() docs for the same rationale.
        // Shadow set-0 resources: the comparison sampler + a 1x1 dummy map; m_activeShadowView points at
        // the real ShadowSystem map (set each frame) or the dummy.
        rhi::Sampler* m_shadowSampler = nullptr;
        rhi::Texture* m_dummyShadowTex = nullptr;
        rhi::TextureView* m_dummyShadowView = nullptr;
        rhi::TextureView* m_activeShadowView = nullptr;
        // The dummy shadow/atlas depth textures are bound (and statically sampled by the mesh shader) on
        // frames with no real caster, but live OUTSIDE the render graph, so the barrier solver never
        // transitions them out of UNDEFINED. Transition them once to DepthStencilRead the first frame we
        // hold the command encoder, so the layout the descriptor expects always matches (VUID-09600).
        bool m_dummyDepthInit = false;
        u64 m_activeShadowGen = 0;
        // Local-light (spot/point) shadow atlas (t2) + per-light entries (t3) - 5.3. The atlas view + its
        // generation + the local-shadow ring generation extend the set-0 bind-group cache key.
        rhi::Texture* m_dummyAtlasTex = nullptr;
        rhi::TextureView* m_dummyAtlasView = nullptr;
        rhi::TextureView* m_activeAtlasView = nullptr;
        u64 m_activeAtlasGen = 0;
        u32 m_localShadowBase = 0;      // this frame's base into m_localShadowRing
        u32 m_localShadowPassCount = 0; // # atlas depth passes (caster re-emits) this frame
        u32 m_captureFacePasses = 0;    // # probe-capture face passes (caster re-emits) this frame
        u32 m_objectBGGen = 0, m_instanceBGGen = 0;

        // --- MultiMesh (instanced-mesh) persistent buffers (docs/design/instanced-mesh.md §5) ---
        // Each InstancedMeshComponent (a "set") owns a PERSISTENT InstanceData storage buffer + a set-1 bind
        // group over it, keyed by the component's stable key. Uploaded only when the set's version changes -
        // a static set uploads once, then costs nothing per frame (the whole point). A single shared
        // DataOffsets ramp [{0,..},{1,..},...] serves EVERY set (.x indexes each set's own buffer), grown to
        // the largest set. Bound identically for the depth prepass, forward, and all shadow cascades.
        static constexpr u32 kMultiMeshMaxFiF =
            4; // upper bound on frames-in-flight for the per-region arrays
        struct MultiMeshSet
        {
            // The InstanceData is N-buffered (one region per frame-in-flight) so a runtime position change can
            // rewrite this frame's region while the GPU reads the previous frame's - hazard-free even for
            // per-frame-dynamic crowds. Each region has its own set-1 bind group; the draw binds this frame's.
            rhi::Buffer* instanceBuf =
                nullptr; // Storage, CpuToGpu: framesInFlight x capacity x InstanceData
            rhi::BindGroup* instanceBG[kMultiMeshMaxFiF] =
                {}; // one set-1 bind group per FiF region
            rhi::BindGroup* activeInstanceBG =
                nullptr;      // this frame's region bind group (set in EnsureMultiMeshSet)
            u32 capacity = 0; // instances per region
            u32 count = 0;    // live instance count this frame
            u32 uploadedVersion =
                0; // last component version being written (0 = never; versions start at 1)
            u32 dirtyFrames = 0; // regions still to write after a version change (FiF countdown)
            u32 lastFrame = 0;   // last frame this set was extracted (for eviction)
            // Skinned crowds: a PER-SET DataOffsets buffer (dynamic, refilled each frame with per-instance bone
            // bases) - replaces the shared static ramp, whose .y is always 0. Only allocated for skinned sets.
            rhi::Buffer* offsetsBuf =
                nullptr;             // Vertex, CpuToGpu: framesInFlight x capacity x DataOffsets
            u32 offsetsCapacity = 0; // instances per frame region
            u32 offsetsByteOffset =
                0; // this frame's region byte offset (rewritten every frame -> FiF-slotted)
            bool skinned = false; // drew skinned this frame (posePool present)
        };
        HashMap<u64, MultiMeshSet> m_multiMeshSets;
        rhi::Buffer* m_rampBuffer = nullptr; // shared DataOffsets ramp (Vertex, CpuToGpu)
        u32 m_rampCapacity = 0;
        u32 m_multiMeshFrame = 0; // bumped each UploadMultiMeshes (eviction clock)

        // IBL (phase 6): SH9 diffuse buffer (t5) + prefiltered specular cube (t6) + BRDF LUT (t7) + a
        // linear env sampler (s1), all set 0. Neutral 1x1 dummies are bound when no environment is active.
        static constexpr u64 kShBytes = sizeof(f32) * 4 * 9; // 9 RGB SH coeffs as float4
        rhi::Sampler* m_envSampler = nullptr;
        rhi::Buffer* m_dummyShBuffer = nullptr;
        rhi::Texture* m_dummyCube = nullptr;
        rhi::TextureView* m_dummyCubeView = nullptr;
        rhi::Texture* m_dummyBrdf = nullptr;
        rhi::TextureView* m_dummyBrdfView = nullptr;

        // Reflection probes (P2-P4): prefiltered cube-ARRAY (t8) + probe-metadata SRV (t9) + probe count.
        static constexpr u64 kProbeBufferBytes =
            64u * 16u; // sizeof(GpuProbe)=64 * kMaxReflectionProbes=16
        rhi::Texture* m_dummyProbeCube = nullptr;
        rhi::TextureView* m_dummyProbeCubeView = nullptr;
        rhi::Buffer* m_dummyProbeBuffer = nullptr; // 1 zeroed record for the no-probe path
        rhi::TextureView* m_activeProbeCube = nullptr;
        rhi::Buffer* m_activeProbeBuffer = nullptr;
        u32 m_activeProbeCount = 0;

        // set 3 (clustered light lists). A dummy bound when clustering is off; otherwise one bind group
        // per (view, frame-in-flight) slot over the ClusterSystem's per-view cluster buffers.
        static constexpr u32 kMaxFramesInFlight = 8;
        static constexpr u32 kMaxViewsPerFrame = 8;
        static constexpr u32 kMaxClusterSlots = kMaxViewsPerFrame * kMaxFramesInFlight;
        rhi::Buffer* m_dummyClusterOffsets = nullptr;
        rhi::Buffer* m_dummyClusterIndices = nullptr;
        rhi::BindGroup* m_dummyClusterBG = nullptr;
        rhi::BindGroup* m_clusterBGs[kMaxClusterSlots] = {};
        rhi::Buffer* m_clusterBGOffsets[kMaxClusterSlots] = {};
        u32 m_clusterBGVersion[kMaxClusterSlots] = {};

        bool m_ready = false;
    };

} // namespace draconic::render
