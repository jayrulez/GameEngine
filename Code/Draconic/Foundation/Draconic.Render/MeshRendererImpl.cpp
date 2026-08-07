// Draconic Render - draconic.render:mesh_renderer implementation unit (sec 3.2 / sec 10.6).
module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

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

namespace draconic::render
{
    Status MeshRenderer::Initialize()
    {

        // set 0: per-view UBO (ViewProj + camera + light range), dynamic offset, Vertex|Fragment;
        // + the light list as a read-only StructuredBuffer (Fragment), bound whole.
        rhi::BindGroupLayoutEntry viewEntry = rhi::BindGroupLayoutEntry::UniformBuffer(
            0, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment);
        viewEntry.hasDynamicOffset = true;
        rhi::BindGroupLayoutEntry lightEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ sizeof(GpuLight)); // StructuredBuffer<GpuLight>
        // Directional shadow map (t1) + comparison sampler (s0) live in set 0 (the bind-group budget
        // is 4 SETS, not 4 bindings - shadows fold into the view set rather than needing a 5th set).
        rhi::BindGroupLayoutEntry shadowTexEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            1, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray);
        shadowTexEntry.textureSampleType = rhi::TextureSampleType::Depth; // SampleCmp source
        // Local-light (spot/point) shadow atlas (t2, Texture2D) + per-light shadow entries (t3, SRV).
        rhi::BindGroupLayoutEntry atlasTexEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            2, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2DArray);
        atlasTexEntry.textureSampleType = rhi::TextureSampleType::Depth; // SampleCmp source
        rhi::BindGroupLayoutEntry localShadowEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            3, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ sizeof(GpuLocalShadow)); // StructuredBuffer<GpuLocalShadow>
        rhi::BindGroupLayoutEntry shadowSampEntry{};
        shadowSampEntry.binding = 0;
        shadowSampEntry.visibility = rhi::ShaderStage::Fragment;
        shadowSampEntry.type = rhi::BindingType::ComparisonSampler;
        // t4: the GPU skinning bone-matrix pool (Vertex-visible SRV). Bound on every set-0 BG; the
        // forward VS only reads it under the SKINNED permutation.
        rhi::BindGroupLayoutEntry boneEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            4, rhi::ShaderStage::Vertex, /*readOnly*/ true,
            /*stride*/ sizeof(Float4x4)); // StructuredBuffer<BoneMatrix> (4x float4)
        // IBL (phase 6) folds into set 0 too: SH9 diffuse coeffs (t5, SRV), prefiltered specular cube
        // (t6), BRDF LUT (t7), + a linear-clamp env sampler (s1, distinct from the comparison sampler s0).
        rhi::BindGroupLayoutEntry iblShEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            5, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 16); // StructuredBuffer<float4> IblSH
        rhi::BindGroupLayoutEntry prefilterEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            6, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCube);
        rhi::BindGroupLayoutEntry brdfEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            7, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::Texture2D);
        rhi::BindGroupLayoutEntry envSampEntry =
            rhi::BindGroupLayoutEntry::Sampler(1, rhi::ShaderStage::Fragment);
        // Reflection probes: a cube-ARRAY of local probe radiance (t8) + a probe metadata buffer (t9, SRV).
        rhi::BindGroupLayoutEntry probeEntry = rhi::BindGroupLayoutEntry::SampledTexture(
            8, rhi::ShaderStage::Fragment, rhi::TextureViewDimension::TextureCubeArray);
        rhi::BindGroupLayoutEntry probeBufEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            9, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 64); // StructuredBuffer<GpuProbe> (4x float4, see ReflectionProbeSystem)
        rhi::BindGroupLayoutEntry set0[] = {
            viewEntry,       lightEntry, shadowTexEntry, atlasTexEntry,  localShadowEntry,
            shadowSampEntry, boneEntry,  iblShEntry,     prefilterEntry, brdfEntry,
            envSampEntry,    probeEntry, probeBufEntry};
        rhi::BindGroupLayoutDesc s0d{};
        s0d.entries = Span<const rhi::BindGroupLayoutEntry>{set0, 13};
        if (!m_device->CreateBindGroupLayout(s0d, m_viewLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // set 1 (non-instanced): per-object UBO (World + Tint), dynamic offset.
        rhi::BindGroupLayoutEntry objEntry =
            rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
        objEntry.hasDynamicOffset = true;
        if (!MakeLayout(objEntry, m_objectLayout))
        {
            return Status{ErrorCode::Unknown};
        }

        // set 1 (instanced): per-instance StructuredBuffer (read-only storage).
        rhi::BindGroupLayoutEntry instEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Vertex, /*readOnly*/ true,
            /*stride*/ sizeof(InstanceData)); // StructuredBuffer<InstanceData>
        if (!MakeLayout(instEntry, m_instanceLayout))
        {
            return Status{ErrorCode::Unknown};
        }

        // set 2 (material) is NOT a fixed renderer layout: it is derived per material from the
        // material's own property list (materials::MaterialSystem::GetOrCreateLayout) and the forward
        // pipeline layout is assembled per set-2 layout (GetOrCreatePipelineLayout). This is what makes
        // the renderer truly material-driven - a custom shader (e.g. toon) with a different property set
        // gets its own set-2 layout + PSO with zero renderer changes. The set-0/1/3 layouts below are
        // the stable "frame contract" every material plugs into.

        // set 3: clustered light lists - per-cluster (offset,count) SRV (t0) + flat index SRV (t1).
        rhi::BindGroupLayoutEntry clOffEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            0, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 8); // StructuredBuffer<uint2> ClusterOffsets
        rhi::BindGroupLayoutEntry clIdxEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            1, rhi::ShaderStage::Fragment, /*readOnly*/ true,
            /*stride*/ 4); // StructuredBuffer<uint> ClusterLightIndices
        rhi::BindGroupLayoutEntry set3[] = {clOffEntry, clIdxEntry};
        rhi::BindGroupLayoutDesc s3d{};
        s3d.entries = Span<const rhi::BindGroupLayoutEntry>{set3, 2};
        if (!m_device->CreateBindGroupLayout(s3d, m_clusterLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // Forward pipeline layouts (view + object/instance + material set-2 + cluster) are built
        // on demand per material set-2 layout and cached in GetOrCreatePipelineLayout.

        // Shadow depth-only path (phase 5): a vertex-only shader + a 2-set pipeline layout
        // (set 0 = light view UBO, set 1 = the SAME object/instance layouts as forward, so the
        // object/instance bind groups are reused). No material/cluster sets.
        rhi::BindGroupLayoutEntry shadowViewEntry =
            rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
        shadowViewEntry.hasDynamicOffset = true;
        // set 0 also carries the skinning bone-matrix pool (t4) so skinned casters deform their shadow.
        rhi::BindGroupLayoutEntry shadowBoneEntry = rhi::BindGroupLayoutEntry::StorageBuffer(
            4, rhi::ShaderStage::Vertex, /*readOnly*/ true,
            /*stride*/ sizeof(Float4x4)); // StructuredBuffer<BoneMatrix> (4x float4)
        rhi::BindGroupLayoutEntry shadowSet0[] = {shadowViewEntry, shadowBoneEntry};
        rhi::BindGroupLayoutDesc svd{};
        svd.entries = Span<const rhi::BindGroupLayoutEntry>{shadowSet0, 2};
        if (!m_device->CreateBindGroupLayout(svd, m_shadowViewLayout).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!MakePipelineLayout(m_shadowViewLayout, m_objectLayout, m_shadowPipelineLayoutSingle))
        {
            return Status{ErrorCode::Unknown};
        }
        if (!MakePipelineLayout(m_shadowViewLayout, m_instanceLayout,
                                m_shadowPipelineLayoutInstanced))
        {
            return Status{ErrorCode::Unknown};
        }

        if (!CreateDefaultMaterial().IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        if (!CreateShadowResources().IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return CreateDummyClusters();
    }

    void MeshRenderer::SetShadowMap(rhi::TextureView* view, u64 generation)
    {
        m_activeShadowView = (view != nullptr) ? view : m_dummyShadowView;
        m_activeShadowGen = (view != nullptr) ? generation : 0; // dummy never changes
    }

    void MeshRenderer::SetShadowAtlas(rhi::TextureView* view, u64 generation, u32 passCount)
    {
        m_activeAtlasView = (view != nullptr) ? view : m_dummyAtlasView;
        m_activeAtlasGen = (view != nullptr) ? generation : 0;
        m_localShadowPassCount =
            (view != nullptr) ? passCount : 0; // each tile re-emits the casters
    }

    void MeshRenderer::SetProbes(rhi::TextureView* cubeArray, rhi::Buffer* probeBuffer, u32 count)
    {
        const bool has = (cubeArray != nullptr && probeBuffer != nullptr && count > 0);
        m_activeProbeCube = has ? cubeArray : m_dummyProbeCubeView;
        m_activeProbeBuffer = has ? probeBuffer : m_dummyProbeBuffer;
        m_activeProbeCount =
            has ? count : 0; // -> ViewData.probeCenter.w (the forward's loop bound)
    }

    void MeshRenderer::UploadLocalShadows(Span<const GpuLocalShadow> shadows, u32 frameIndex)
    {
        (void)frameIndex;
        m_localShadowBase = 0;
        u32 n = static_cast<u32>(shadows.Size());
        if (n == 0 || !m_ready)
        {
            return;
        }
        if (n > kMaxLocalShadows)
        {
            n = kMaxLocalShadows;
        }
        const DynamicUniformRing::Range r = m_localShadowRing.AllocateRange(n);
        if (r.ok)
        {
            MemCopy(r.ptr, shadows.Data(), static_cast<usize>(n) * sizeof(GpuLocalShadow));
            m_localShadowBase = r.slotIndex;
        }
    }

    Span<const RenderCategory> MeshRenderer::SupportedCategories() const
    {
        static constexpr RenderCategory kCats[] = {
            RenderCategories::Opaque, RenderCategories::Masked, RenderCategories::Transparent};
        return Span<const RenderCategory>{kCats, 3};
    }

    void MeshRenderer::PrepareFrame(u32 maxDraws, u32 frameIndex)
    {
        m_ready = false;
        PruneStaleMaterialInstances();
        TickRetiredBindGroups();    // free per-frame bind groups retired long enough ago to be idle
        m_materials->TickRetired(); // same, for material set-2 groups replaced by rebuilds
        if (maxDraws == 0)
        {
            return;
        }
        // Per-frame ring capacity = draws x (passes that re-emit them): the depth PREPASS + the forward
        // (2 camera passes) + per-cascade CSM re-emit + per-spot-tile local-atlas re-emit. Under-counting
        // overflows the object/instance/offset rings at high draw counts -> Allocate() fails -> dropped
        // draws (was missing the prepass, so the stress tests lost their spheres).
        const u32 drawCap =
            maxDraws * (2u + ShadowCascades::kCount + m_localShadowPassCount + m_captureFacePasses);
        if (!m_viewRing.Reserve(maxDraws) || !m_shadowViewRing.Reserve(kMaxShadowPasses) ||
            !m_objectRing.Reserve(drawCap) || !m_instanceRing.Reserve(drawCap) ||
            !m_offsetsRing.Reserve(drawCap) || !m_lightRing.Reserve(kMaxLights) ||
            !m_localShadowRing.Reserve(kMaxLocalShadows) || !m_boneRing.Reserve(kMaxBoneMatrices))
        {
            return;
        }
        if (!EnsureBoneDevice())
        {
            return;
        } // device-local mirror of the bone staging ring (VS reads VRAM)
        if (!EnsureShadowViewBindGroup() ||
            !EnsureBindGroup(m_objectRing, m_objectLayout, sizeof(ObjectData), m_objectBG,
                             m_objectBGGen, /*whole*/ false) ||
            !EnsureBindGroup(m_instanceRing, m_instanceLayout, 0, m_instanceBG, m_instanceBGGen,
                             /*whole*/ true))
        {
            return;
        }
        m_frameIndex =
            frameIndex; // frames-in-flight slot for the per-set skinned MultiMesh offsets buffer
        m_viewRing.BeginFrame(frameIndex);
        m_shadowViewRing.BeginFrame(frameIndex);
        m_objectRing.BeginFrame(frameIndex);
        m_instanceRing.BeginFrame(frameIndex);
        m_offsetsRing.BeginFrame(frameIndex);
        m_lightRing.BeginFrame(frameIndex);
        m_localShadowRing.BeginFrame(frameIndex);
        m_boneRing.BeginFrame(frameIndex);
        m_instShareCache
            .Clear(); // per-frame: the camera prepass fills it, the forward reuses it (ring offsets are frame-scoped)
        m_ready = true;
    }

    bool MeshRenderer::EnsureBoneDevice()
    {
        const u64 want = m_boneRing.ByteCapacity();
        if (m_boneDevice != nullptr && m_boneDeviceBytes == want)
        {
            return true;
        }
        // Idle only when REPLACING a live buffer (first alloc has nothing to protect; a
        // mid-frame wait on web pumps the event loop and drops the frame's submit).
        if (m_boneDevice != nullptr)
        {
            RetireOrDrainBuffer(m_boneDevice);
        }
        rhi::BufferDesc bd{};
        bd.size = want;
        bd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
        bd.memory = rhi::MemoryLocation::GpuOnly;
        bd.label = u8"mesh.bones.device";
        if (!m_device->CreateBuffer(bd, m_boneDevice).IsOk())
        {
            m_boneDevice = nullptr;
            m_boneDeviceBytes = 0;
            return false;
        }
        m_boneDeviceBytes = want;
        ++m_boneDeviceGen; // invalidate set-0 bind groups that bind the device buffer
        return true;
    }

    void MeshRenderer::UploadSkinning(const ExtractedScene& scene, rhi::CommandEncoder& encoder)
    {
        m_boneStart.Clear();
        m_skinnedScratch.Clear();
        if (!m_ready)
        {
            return;
        }
        UploadMultiMeshes(
            scene); // ensure/upload the persistent instanced-mesh buffers (once per frame)
        // One-time: bring the out-of-graph dummy depth textures into the layout their descriptors
        // expect, so they're never sampled while UNDEFINED on caster-less frames (VUID-09600).
        if (!m_dummyDepthInit)
        {
            if (m_dummyShadowTex != nullptr)
            {
                encoder.TransitionTexture(m_dummyShadowTex, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::DepthStencilRead);
            }
            if (m_dummyAtlasTex != nullptr)
            {
                encoder.TransitionTexture(m_dummyAtlasTex, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::DepthStencilRead);
            }
            // IBL color fallbacks (cube + BRDF LUT) - also out-of-graph, sampled when no env is active.
            if (m_dummyCube != nullptr)
            {
                encoder.TransitionTexture(m_dummyCube, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::ShaderRead);
            }
            if (m_dummyBrdf != nullptr)
            {
                encoder.TransitionTexture(m_dummyBrdf, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::ShaderRead);
            }
            if (m_dummyProbeCube != nullptr)
            {
                encoder.TransitionTexture(m_dummyProbeCube, rhi::ResourceState::Undefined,
                                          rhi::ResourceState::ShaderRead);
            }
            m_dummyDepthInit = true;
        }
        // Pass 1: collect DISTINCT skinned instances (by boneMatrices ptr). Total matrices = sum of
        // 2*boneCount (current slab + previous slab). The map reserves the key so dups are skipped.
        u32 total = 0;
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            } // skip non-mesh (e.g. sprite) items
            const auto* md = static_cast<const MeshRenderData*>(data);
            if (md->boneMatrices == nullptr || md->boneCount == 0)
            {
                continue;
            }
            if (md->mesh == nullptr || !md->mesh->IsSkinned())
            {
                continue;
            }
            if (m_boneStart.Contains(md->boneMatrices))
            {
                continue;
            }
            m_boneStart.InsertOrAssign(md->boneMatrices,
                                       BoneSlot{}); // reserve; bases filled in pass 2
            m_skinnedScratch.PushBack(
                SkinnedRef{md->boneMatrices, md->prevBoneMatrices, md->boneCount});
            total += md->boneCount * 2u;
        }
        // Skinned-MultiMesh POSE POOLS: M palettes (poseCount * boneCount matrices) per crowd, uploaded once
        // (no prev slab - crowd motion vectors are v1-deferred). Each set's per-instance bone base is filled
        // into its DataOffsets buffer in FillSkinnedMultiMeshOffsets once the pool base is known.
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* mdb = static_cast<const MeshRenderData*>(data);
            if (!mdb->multiMesh)
            {
                continue;
            }
            const auto* mm = static_cast<const MultiMeshRenderData*>(mdb);
            if (mm->posePool == nullptr || mm->poseCount == 0 || mm->boneCount == 0)
            {
                continue;
            }
            if (mm->mesh == nullptr || !mm->mesh->IsSkinned())
            {
                continue;
            }
            if (m_boneStart.Contains(mm->posePool))
            {
                continue;
            }
            m_boneStart.InsertOrAssign(mm->posePool, BoneSlot{});
            const u32 poolCount = mm->poseCount * mm->boneCount;
            // cur + prev slab (per-bone motion vectors); prev == null falls back to cur (zero motion).
            m_skinnedScratch.PushBack(
                SkinnedRef{mm->posePool, mm->prevPosePool, poolCount, /*hasPrev*/ true});
            total += poolCount * 2u;
        }
        if (total == 0)
        {
            m_boneStart.Clear();
            return;
        }
        const DynamicUniformRing::Range block = m_boneRing.AllocateRange(total);
        if (!block.ok)
        {
            m_boneStart.Clear();
            return;
        }

        // Pass 2: write each distinct instance into the block (current then prev) + record its bases.
        u32 cursor = 0; // matrix-units offset within the block
        for (const SkinnedRef& r : m_skinnedScratch)
        {
            const u32 n = r.count;
            const u32 base = block.slotIndex + cursor;
            Float4x4* dst = static_cast<Float4x4*>(block.ptr) + cursor;
            MemCopy(dst, r.cur, static_cast<usize>(n) * sizeof(Float4x4));
            if (r.hasPrev)
            { // per-entity caster: also write the prev slab (motion vectors)
                MemCopy(dst + n, (r.prev != nullptr) ? r.prev : r.cur,
                        static_cast<usize>(n) * sizeof(Float4x4));
                if (BoneSlot* slot = m_boneStart.Find(r.cur))
                {
                    *slot = BoneSlot{base, base + n};
                }
                cursor += n * 2u;
            }
            else
            { // MultiMesh pose pool: just the M palettes, no prev slab
                if (BoneSlot* slot = m_boneStart.Find(r.cur))
                {
                    *slot = BoneSlot{base, base};
                }
                cursor += n;
            }
        }

        // Mirror the populated range to VRAM, then make it visible to vertex-shader reads.
        encoder.CopyBufferToBuffer(m_boneRing.Buffer(), block.byteOffset, m_boneDevice,
                                   block.byteOffset, static_cast<u64>(total) * sizeof(Float4x4));
        encoder.TransitionBuffer(m_boneDevice, rhi::ResourceState::CopyDst,
                                 rhi::ResourceState::ShaderRead);
        FillSkinnedMultiMeshOffsets(
            scene); // per-instance bone base into each skinned set's DataOffsets
    }

    void MeshRenderer::UploadMultiMeshes(const ExtractedScene& scene)
    {
        ++m_multiMeshFrame;
        u32 maxCount = 0;
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* md = static_cast<const MeshRenderData*>(data);
            if (!md->multiMesh)
            {
                continue;
            }
            const auto* mm = static_cast<const MultiMeshRenderData*>(md);
            if (mm->instanceCount > maxCount)
            {
                maxCount = mm->instanceCount;
            }
        }
        if (maxCount == 0)
        {
            return;
        }
        if (!EnsureRamp(maxCount))
        {
            return;
        }
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* md = static_cast<const MeshRenderData*>(data);
            if (!md->multiMesh)
            {
                continue;
            }
            EnsureMultiMeshSet(*static_cast<const MultiMeshRenderData*>(md));
        }
    }

    // Replace-path release: retire the buffer through the frames-in-flight queue when
    // wired (web-safe - no mid-frame WaitIdle pumping the browser event loop), else the
    // classic drain. Nulls the pointer either way.
    void MeshRenderer::RetireOrDrainBuffer(rhi::Buffer*& buffer)
    {
        if (buffer == nullptr)
        {
            return;
        }
        if (m_retire != nullptr)
        {
            m_retire->Retire(buffer);
        }
        else
        {
            m_device->WaitIdle();
            m_device->DestroyBuffer(buffer);
        }
        buffer = nullptr;
    }

    bool MeshRenderer::EnsureRamp(u32 count)
    {
        if (m_rampBuffer != nullptr && count <= m_rampCapacity)
        {
            return true;
        }
        // An in-flight frame may still reference the old ramp: retire when wired, drain else.
        if (m_rampBuffer != nullptr)
        {
            RetireOrDrainBuffer(m_rampBuffer);
        }
        rhi::BufferDesc bd{};
        bd.size = static_cast<u64>(count) * sizeof(DataOffsets);
        bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        bd.label = u8"mesh.multimesh.ramp";
        if (!m_device->CreateBuffer(bd, m_rampBuffer).IsOk())
        {
            m_rampBuffer = nullptr;
            m_rampCapacity = 0;
            return false;
        }
        if (auto* p = static_cast<DataOffsets*>(m_rampBuffer->Map()))
        {
            for (u32 i = 0; i < count; ++i)
            {
                p[i] = DataOffsets{i, 0, 0, 0};
            }
            m_rampBuffer->Unmap();
        }
        m_rampCapacity = count;
        return true;
    }

    void MeshRenderer::EnsureMultiMeshSet(const MultiMeshRenderData& mm)
    {
        MultiMeshSet* set = m_multiMeshSets.Find(mm.key);
        if (set == nullptr)
        {
            set = &m_multiMeshSets.InsertOrAssign(mm.key, MultiMeshSet{});
        }
        set->lastFrame = m_multiMeshFrame;

        const u32 fif = Min(m_framesInFlight, kMultiMeshMaxFiF);
        const u32 region = m_frameIndex % fif; // this frame's InstanceData region

        if (set->instanceBuf == nullptr || mm.instanceCount > set->capacity)
        {
            // An in-flight frame may still reference the old buffer/bind groups: retire
            // when wired (web-safe), else drain once for the whole replacement.
            if (m_retire == nullptr &&
                (set->instanceBuf != nullptr || set->instanceBG[0] != nullptr))
            {
                m_device->WaitIdle();
            }
            for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
            {
                if (set->instanceBG[r] != nullptr)
                {
                    if (m_retire != nullptr)
                    {
                        m_retire->Retire(set->instanceBG[r]);
                    }
                    else
                    {
                        m_device->DestroyBindGroup(set->instanceBG[r]);
                    }
                    set->instanceBG[r] = nullptr;
                }
            }
            if (set->instanceBuf != nullptr)
            {
                if (m_retire != nullptr)
                {
                    m_retire->Retire(set->instanceBuf);
                }
                else
                {
                    m_device->DestroyBuffer(set->instanceBuf);
                }
                set->instanceBuf = nullptr;
            }
            rhi::BufferDesc bd{};
            const u64 regionBytes = static_cast<u64>(mm.instanceCount) * sizeof(InstanceData);
            bd.size = static_cast<u64>(fif) * regionBytes; // one region per frame-in-flight
            bd.usage = rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"mesh.multimesh.instances";
            if (!m_device->CreateBuffer(bd, set->instanceBuf).IsOk())
            {
                set->instanceBuf = nullptr;
                set->capacity = 0;
                return;
            }
            // A set-1 bind group per region (each over its own slice of the buffer, so the shader's
            // Instances[i] indexes within the bound region - the shared static ramp / skinned offsets stay 0-based).
            bool ok = true;
            for (u32 r = 0; r < fif; ++r)
            {
                rhi::BindGroupEntry be = rhi::BindGroupEntry::BufferEntry(
                    set->instanceBuf, static_cast<u64>(r) * regionBytes, regionBytes);
                rhi::BindGroupDesc bgd{};
                bgd.layout = m_instanceLayout;
                bgd.entries = Span<const rhi::BindGroupEntry>{&be, 1};
                if (!m_device->CreateBindGroup(bgd, set->instanceBG[r]).IsOk())
                {
                    set->instanceBG[r] = nullptr;
                    ok = false;
                    break;
                }
            }
            if (!ok)
            {
                for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
                {
                    if (set->instanceBG[r] != nullptr)
                    {
                        m_device->DestroyBindGroup(set->instanceBG[r]);
                        set->instanceBG[r] = nullptr;
                    }
                }
                m_device->DestroyBuffer(set->instanceBuf);
                set->instanceBuf = nullptr;
                set->capacity = 0;
                return;
            }
            set->capacity = mm.instanceCount;
            set->uploadedVersion = 0; // force a re-upload after (re)allocation
            set->dirtyFrames = fif;   // write every region
        }
        set->count = mm.instanceCount;
        set->activeInstanceBG = set->instanceBG[region];

        // A version change re-uploads for `fif` frames so every region ends up current, then stops - static
        // sets write only once (fif frames), per-frame-dynamic sets write every frame, both hazard-free
        // (each frame writes ONLY its own region while the GPU reads the previous one).
        if (set->uploadedVersion != mm.version)
        {
            set->uploadedVersion = mm.version;
            set->dirtyFrames = fif;
        }
        if (set->dirtyFrames > 0 && set->instanceBuf != nullptr && mm.transforms != nullptr)
        {
            if (auto* dst = static_cast<InstanceData*>(set->instanceBuf->Map()))
            {
                InstanceData* r = dst + static_cast<usize>(region) * set->capacity;
                for (u32 i = 0; i < mm.instanceCount; ++i)
                {
                    const Color tint =
                        (mm.tints != nullptr) ? mm.tints[i] : mm.color; // per-instance or shared
                    r[i] = InstanceData{mm.transforms[i], mm.transforms[i], tint};
                }
                set->instanceBuf->Unmap();
            }
            --set->dirtyFrames;
        }

        // Skinned crowds need a per-set DataOffsets buffer (dynamic per-instance bone bases). Allocate/grow
        // it here; it's FILLED in FillSkinnedMultiMeshOffsets once the pose pool's bone base is known.
        set->skinned = (mm.posePool != nullptr && mm.poseCount > 0 && mm.boneCount > 0);
        if (set->skinned && (set->offsetsBuf == nullptr || mm.instanceCount > set->offsetsCapacity))
        {
            if (set->offsetsBuf != nullptr)
            {
                RetireOrDrainBuffer(set->offsetsBuf);
            }
            rhi::BufferDesc od{};
            // framesInFlight regions: the offsets are rewritten EVERY frame (dynamic bone bases), so each
            // frame writes its own region while the GPU reads the previous frame's - never the same bytes.
            od.size = static_cast<u64>(m_framesInFlight) * static_cast<u64>(mm.instanceCount) *
                      sizeof(DataOffsets);
            od.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::CopyDst;
            od.memory = rhi::MemoryLocation::CpuToGpu;
            od.label = u8"mesh.multimesh.offsets";
            if (!m_device->CreateBuffer(od, set->offsetsBuf).IsOk())
            {
                set->offsetsBuf = nullptr;
                set->offsetsCapacity = 0;
                return;
            }
            set->offsetsCapacity = mm.instanceCount;
        }
    }

    void MeshRenderer::FillSkinnedMultiMeshOffsets(const ExtractedScene& scene)
    {
        const u32 region = m_frameIndex % m_framesInFlight; // this frame's FiF slot
        for (RenderData* data : scene.Items())
        {
            if (data == nullptr || data->rendererId != RendererId())
            {
                continue;
            }
            const auto* mdb = static_cast<const MeshRenderData*>(data);
            if (!mdb->multiMesh)
            {
                continue;
            }
            const auto* mm = static_cast<const MultiMeshRenderData*>(mdb);
            if (mm->posePool == nullptr || mm->poseCount == 0 || mm->boneCount == 0)
            {
                continue;
            }
            MultiMeshSet* set = m_multiMeshSets.Find(mm->key);
            if (set == nullptr || set->offsetsBuf == nullptr || set->offsetsCapacity == 0)
            {
                continue;
            }
            const BoneSlot* pool = m_boneStart.Find(mm->posePool);
            if (pool == nullptr)
            {
                continue;
            }
            const u32 base =
                region *
                set->offsetsCapacity; // write ONLY this frame's region (GPU reads the prev one)
            auto* od = static_cast<DataOffsets*>(set->offsetsBuf->Map());
            if (od == nullptr)
            {
                continue;
            }
            const u32 M = mm->poseCount;
            for (u32 i = 0; i < mm->instanceCount; ++i)
            {
                // Pick this instance's pose out of the M shared palettes per the caller's policy (pure,
                // unit-tested in SelectPose). Exactly M palette evaluations regardless; the slot is stable
                // per i across frames, so motion blur (prev pose) is unaffected.
                const u32 pose = SelectPose(mm->poseAssignment, i, M, mm->poseIndices);
                const u32 bucket = pose * mm->boneCount;     // matrix-unit offset within a palette
                const u32 curBase = pool->base + bucket;     // current pose
                const u32 prvBase = pool->prevBase + bucket; // last frame's pose (motion vectors)
                od[base + i] = DataOffsets{
                    i, curBase, prvBase, 0}; // .x = Instances[] idx, .y = cur bone base, .z = prev
            }
            set->offsetsBuf->Unmap();
            set->offsetsByteOffset = base * static_cast<u32>(sizeof(DataOffsets));
        }
    }

    void MeshRenderer::Resolve(const RenderRecordContext& ctx, Span<const DrawItem> items,
                               Array<ResolvedDraw>& out)
    {
        if (!m_ready || items.IsEmpty())
        {
            return;
        }
        // This VIEW's set-0 group (its scene's IBL products; per-view slots - see the Ensure doc).
        if (!EnsureViewBindGroup(ctx))
        {
            return;
        }

        // Upload this view's lights into the light ring (bound whole at set 0; the shader reads
        // Lights[lightOffset + i]). Clamp to the per-frame capacity.
        u32 lightCount = static_cast<u32>(ctx.lights.Size());
        if (lightCount > kMaxLights)
        {
            lightCount = kMaxLights;
        }
        u32 lightOffset = 0;
        if (lightCount > 0)
        {
            const DynamicUniformRing::Range lr = m_lightRing.AllocateRange(lightCount);
            if (lr.ok)
            {
                MemCopy(lr.ptr, ctx.lights.Data(),
                        static_cast<usize>(lightCount) * sizeof(GpuLight));
                lightOffset = lr.slotIndex;
            }
            else
            {
                lightCount = 0;
            }
        }

        // Per-view UBO (shared by every draw in this call): ViewProj + camera + light range. The
        // two pipeline layouts share set 0 (m_viewLayout), so the resolved binding is the same.
        // The clustered light lists for this view (set 3). Empty binding -> dummy + ClusterGridX=0,
        // which makes the shader fall back to looping all lights.
        rhi::BindGroup* clusterBG = m_dummyClusterBG;
        if (ctx.cluster.Valid())
        {
            // Per-(view, frame) slot - two views in one frame have different cluster buffers and
            // must not share a bind group (else it thrashes / frees a set still in flight).
            const u32 clusterSlot =
                ctx.viewIndex * m_framesInFlight + (ctx.frameIndex % m_framesInFlight);
            clusterBG = EnsureClusterBindGroup(clusterSlot, ctx.cluster.offsets,
                                               ctx.cluster.lightIndices, ctx.cluster.version);
        }

        const DynamicUniformRing::Range view = m_viewRing.Allocate();
        if (!view.ok)
        {
            return;
        }
        ViewData vd{};
        vd.viewProj = ctx.viewProj;
        vd.prevViewProj = ctx.prevViewProj; // motion vectors (camera)
        vd.jitter = Float4{ctx.jitter.x, ctx.jitter.y, ctx.prevJitter.x, ctx.prevJitter.y};
        vd.view = ctx.viewMatrix;
        vd.cameraPos = ctx.cameraPos;
        vd.ambient = ctx.ambient;
        const bool iblActive = ctx.ibl.valid && ctx.ibl.shBuffer != nullptr &&
                               ctx.ibl.prefilterView != nullptr && ctx.ibl.brdfView != nullptr;
        vd.iblMaxLod = iblActive ? ctx.ibl.maxLod : -1.0f; // <0 => forward uses flat ambient
        // Probes disabled during probe capture (ctx.probesEnabled=false) -> count 0 so metallics reflect the
        // sky (global IBL), not the not-yet-built probe (which would bake them black - self-reflection).
        // ProbeCenter.x = this view's SCENE's base into the Probes SRV (scenes' records are concatenated
        // per frame), .w = its count (the forward's loop bound); box/slice/etc. per probe live in the buffer.
        vd.probeCenter = Float4{static_cast<f32>(ctx.probeBase), 0, 0,
                                ctx.probesEnabled ? static_cast<f32>(ctx.probeCount) : 0.0f};

        vd.lightCount = static_cast<f32>(lightCount);
        vd.lightOffset = lightOffset;
        // CSM cascade data for THIS view (per-view fit, carried in ctx).
        if (ctx.cascades.valid)
        {
            for (u32 c = 0; c < ShadowCascades::kCount; ++c)
            {
                vd.cascadeViewProj[c] = ctx.cascades.viewProj[c];
            }
            vd.cascadeSplitFar = Float4{ctx.cascades.splitFar[0], ctx.cascades.splitFar[1],
                                        ctx.cascades.splitFar[2], ctx.cascades.splitFar[3]};
            vd.cascadeTexelSize =
                Float4{ctx.cascades.texelWorldSize[0], ctx.cascades.texelWorldSize[1],
                       ctx.cascades.texelWorldSize[2], ctx.cascades.texelWorldSize[3]};
            vd.shadowCascadeCount = static_cast<f32>(ShadowCascades::kCount);
            vd.cascadeLayerBase =
                static_cast<f32>(ctx.cascadeLayerBase); // this view's first array layer
            // Normal-offset is in TEXELS (scaled by the cascade's world texel size in the shader).
            // Keep it tiny (Sedulous uses 0.02) - at large values it shifts the receiver enough to eat
            // the light-facing side of a contact shadow, worse the bigger the cascade's texelWorld grows.
            // Acne is carried by the hardware depth bias (ShadowConfigFor: 50 / 1.5), not this.
            vd.shadowNormalBias = 0.02f;
            vd.shadowDepthBias = 0.0009f;
            vd.shadowParams.x = ctx.shadowFarFade; // CSM far-fade band (runtime-tunable)
        }
        // y = shadow-map sample uv.y sign. The shadow map rasterizes the same as the main color
        // target, so on Y-flip targets (WebGPU, positive viewport) uv.y = +ndc.y*0.5+0.5; on
        // Vulkan (negative-height viewport) it stays -ndc.y*0.5+0.5. Mirrors SkyPass's SkyFlags.
        // OUTSIDE the cascades.valid block: SampleLocalShadow (spot/point tiles) uses this sign
        // too, and local shadows exist without a valid directional CSM - the struct default (0)
        // would collapse every local lookup onto one atlas row.
        vd.shadowParams.y = m_device->NeedsClipSpaceYFlip() ? 1.0f : -1.0f;
        // Ring base + THIS view's scene's entry base (scenes' entries are concatenated per frame;
        // lights carry scene-relative shadowIndex values).
        vd.localShadowBase = m_localShadowBase + ctx.localShadowEntryBase;
        if (ctx.cluster.Valid())
        {
            vd.clusterGridX = ctx.cluster.gridX;
            vd.clusterGridY = ctx.cluster.gridY;
            vd.clusterSliceCount = ctx.cluster.sliceCount;
            vd.clusterTileSize = ctx.cluster.tileSize;
            vd.clusterViewportX = ctx.cluster.viewportX;
            vd.clusterViewportY = ctx.cluster.viewportY;
            vd.clusterNear = ctx.cluster.nearZ;
            vd.clusterFar = ctx.cluster.farZ;
            vd.clusterLogScale = ctx.cluster.logScale;
            vd.clusterLogBias = ctx.cluster.logBias;
        }
        *static_cast<ViewData*>(view.ptr) = vd;
        const u32 viewOffset = view.byteOffset;

        const bool allowInstancing = (items[0].data->category != RenderCategories::Transparent);

        usize i = 0;
        while (i < items.Size())
        {
            const auto* head = static_cast<const MeshRenderData*>(items[i].data);
            // An instanced-mesh SET (MultiMesh): one item drawn N times from its persistent buffer, no
            // per-frame fill. Handled standalone (never merged into a neighbouring run).
            if (head->multiMesh)
            {
                const GpuMesh* mmMesh = m_meshes.GetOrUpload(head->mesh);
                if (mmMesh != nullptr)
                {
                    ResolveMultiMesh(ctx, viewOffset, clusterBG,
                                     *static_cast<const MultiMeshRenderData*>(head), *mmMesh, out);
                }
                ++i;
                continue;
            }
            // Skinned meshes carry per-instance bones via DataOffsets.y now, so they batch like static
            // meshes - identical (mesh, material) skinned instances collapse into one instanced draw.
            const bool headSkinned =
                head->mesh != nullptr && head->mesh->IsSkinned() && head->boneMatrices != nullptr;
            // Extend the run while mesh + material match (a batchable group).
            usize j = i + 1;
            if (allowInstancing)
            {
                while (j < items.Size())
                {
                    const auto* nd = static_cast<const MeshRenderData*>(items[j].data);
                    if (nd->multiMesh || nd->mesh != head->mesh || nd->material != head->material)
                    {
                        break;
                    }
                    ++j;
                }
            }
            const u32 runLen = static_cast<u32>(j - i);

            const GpuMesh* mesh = m_meshes.GetOrUpload(head->mesh);
            if (mesh != nullptr)
            {
                // Skinned always uses the instanced path (even count 1) - the single path has no bone base.
                if (runLen >= 2 || headSkinned)
                {
                    ResolveInstanced(ctx, viewOffset, clusterBG, items, i, runLen, *head, *mesh,
                                     out);
                }
                else
                {
                    ResolveSingle(ctx, viewOffset, clusterBG, *head, *mesh, out);
                }
            }
            i = j;
        }
    }

    void MeshRenderer::ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem> items,
                                        Array<ResolvedDraw>& out)
    {
        if (!m_ready || items.IsEmpty())
        {
            return;
        }

        const DynamicUniformRing::Range sv = m_shadowViewRing.Allocate();
        if (!sv.ok)
        {
            return;
        }
        *static_cast<ShadowViewData*>(sv.ptr) = ShadowViewData{ctx.viewProj};
        const u32 shadowViewOffset = sv.byteOffset;

        usize i = 0;
        while (i < items.Size())
        {
            const auto* head = static_cast<const MeshRenderData*>(items[i].data);
            // MultiMesh caster: one set, drawn N times from its persistent buffer + the shared ramp,
            // shared across the depth prepass and every shadow cascade (no per-cascade fill).
            if (head->multiMesh)
            {
                const GpuMesh* mmMesh = m_meshes.GetOrUpload(head->mesh);
                if (mmMesh != nullptr)
                {
                    ResolveMultiMeshDepth(ctx, shadowViewOffset,
                                          *static_cast<const MultiMeshRenderData*>(head), *mmMesh,
                                          out);
                }
                ++i;
                continue;
            }
            // Skinned casters batch like static ones now (per-instance bone base via DataOffsets.y).
            const bool headSkinned =
                head->mesh != nullptr && head->mesh->IsSkinned() && head->boneMatrices != nullptr;
            usize j = i + 1;
            while (j < items.Size())
            {
                const auto* nd = static_cast<const MeshRenderData*>(items[j].data);
                if (nd->multiMesh || nd->mesh != head->mesh || nd->material != head->material)
                {
                    break;
                }
                ++j;
            }
            const u32 runLen = static_cast<u32>(j - i);
            const GpuMesh* mesh = m_meshes.GetOrUpload(head->mesh);
            if (mesh != nullptr)
            {
                if (runLen >= 2 || headSkinned)
                {
                    ResolveDepthInstanced(ctx, shadowViewOffset, items, i, runLen, *mesh, out);
                }
                else
                {
                    ResolveDepthSingle(ctx, shadowViewOffset, *head, *mesh, out);
                }
            }
            i = j;
        }
    }

    void MeshRenderer::FinishFrame()
    {
        m_viewRing.EndFrame();
        m_shadowViewRing.EndFrame();
        m_objectRing.EndFrame();
        m_instanceRing.EndFrame();
        m_boneRing.EndFrame();
        m_offsetsRing.EndFrame();
        // This frame's world matrices become next frame's "previous" (motion vectors). Flat swap: O(1)
        // pointer moves, no clear - each visible entity overwrites its own slot when resolved, and slots
        // for now-invisible entities are never read (their draws don't resolve). Both buffers keep their
        // capacity, so steady state does zero allocation.
        Array<Float4x4> recycled = Move(m_prevWorld);
        m_prevWorld = Move(m_curWorld);
        m_curWorld = Move(recycled);
        m_ready = false;
    }

    void MeshRenderer::ResolveSingle(const RenderRecordContext& ctx, u32 viewOffset,
                                     rhi::BindGroup* clusterBG, const MeshRenderData& md,
                                     const GpuMesh& mesh, Array<ResolvedDraw>& out)
    {
        materials::Material* mat = (md.material != nullptr) ? md.material : m_defaultMaterial.Get();
        materials::PipelineConfig config = ConfigFor(md, ctx, /*instanced*/ false);

        // GPU skinning: a skinned mesh draws the SKINNED + SkinnedMesh-layout permutation, binds the
        // skin stream (buffer 1), and reads its bones from the shared device pool at boneBase (matrix
        // units, computed once this frame by UploadSkinning). No per-pass upload here.
        u32 boneBase = 0;
        bool skinned = md.boneMatrices != nullptr && md.boneCount > 0 && md.mesh != nullptr &&
                       md.mesh->IsSkinned() && mesh.skinBuffer != nullptr;
        if (skinned)
        {
            const BoneSlot* s = m_boneStart.Find(md.boneMatrices);
            if (s != nullptr)
            {
                boneBase = s->base;
                config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
                config.shaderFlags |= shaders::ShaderFlags::Skinned;
            }
            else
            {
                skinned =
                    false; // not uploaded (pool overflow) -> draw bind pose rather than garbage
            }
        }

        const DynamicUniformRing::Range obj = m_objectRing.Allocate();
        if (!obj.ok)
        {
            return;
        }
        ObjectData od{};
        od.world = md.world;
        od.prevWorld = ctx.needsMotion ? PrevWorldFor(md.entityId, md.world) : md.world;
        od.tint = md.color;
        od.boneBase = boneBase;
        od.prevBoneBase = boneBase;
        *static_cast<ObjectData*>(obj.ptr) = od;

        // Shared per-submesh draw state (view/object/cluster sets + vertex/index buffers + skinning).
        ResolvedDraw base{};
        base.viewSet = m_viewBG;
        base.viewOffset = viewOffset;
        base.viewDynamic = true; // set 0
        base.drawSet = m_objectBG;
        base.drawOffset = obj.byteOffset;
        base.drawDynamic = true;     // set 1
        base.clusterSet = clusterBG; // set 3
        base.vertexBuffer0 = mesh.vertexBuffer;
        base.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            base.vertexBuffer1 = mesh.skinBuffer;
            base.vertexOffset1 = mesh.skinOffset;
        }
        base.indexBuffer = mesh.indexBuffer;
        base.indexFormat = mesh.indexFormat;
        base.instanceCount = 1;

        // Emit one draw for an index sub-range with `m`'s material (set 2 = its data-driven bind group).
        const auto emit = [&](materials::Material* m, u64 indexOffset, u32 indexCount)
        {
            materials::Material* use = (m != nullptr) ? m : mat;
            rhi::BindGroupLayout* set2 = m_materials->GetOrCreateLayout(*use);
            rhi::PipelineLayout* plLayout = GetOrCreatePipelineLayout(set2, /*instanced*/ false);
            if (plLayout == nullptr)
            {
                return;
            }
            rhi::RenderPipeline* pso = m_psoCache->GetPipeline(config, plLayout, ctx.colorFormat);
            if (pso == nullptr)
            {
                return;
            }
            ResolvedDraw d = base;
            d.pso = pso;
            d.materialSet = m_materials->PrepareInstance(*InstanceFor(use), set2);
            d.indexOffset = indexOffset;
            d.indexCount = indexCount;
            out.PushBack(d);
        };

        // Multi-material: draw each submesh with its own material; else one draw for the whole mesh.
        if (md.submeshMaterialCount > 0 && md.mesh != nullptr && !md.mesh->subMeshes.IsEmpty())
        {
            const u64 stride = (mesh.indexFormat == rhi::IndexFormat::UInt16) ? 2u : 4u;
            for (const geometry::SubMesh& sub : md.mesh->subMeshes)
            {
                materials::Material* m =
                    (sub.materialIndex >= 0 &&
                     static_cast<u32>(sub.materialIndex) < md.submeshMaterialCount)
                        ? md.submeshMaterials[sub.materialIndex].Get()
                        : nullptr;
                if (m == nullptr)
                {
                    m = md.material;
                } // OOB/unresolved slot -> slot 0
                emit(m, mesh.indexOffset + static_cast<u64>(sub.startIndex) * stride,
                     static_cast<u32>(sub.indexCount));
            }
        }
        else
        {
            emit(mat, mesh.indexOffset, mesh.indexCount);
        }
    }

    void MeshRenderer::ResolveInstanced(const RenderRecordContext& ctx, u32 viewOffset,
                                        rhi::BindGroup* clusterBG, Span<const DrawItem> items,
                                        usize first, u32 count, const MeshRenderData& head,
                                        const GpuMesh& mesh, Array<ResolvedDraw>& out)
    {
        materials::Material* mat =
            (head.material != nullptr) ? head.material : m_defaultMaterial.Get();
        materials::PipelineConfig config = ConfigFor(head, ctx, /*instanced*/ true);
        // Skinned instanced draw: the shared skin stream (joints/weights) is per-mesh; each instance's
        // bone base rides in DataOffsets.y (current) / .z (prev) so one draw skins N characters.
        const bool skinned = head.boneMatrices != nullptr && head.mesh != nullptr &&
                             head.mesh->IsSkinned() && mesh.skinBuffer != nullptr;
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }
        // The camera depth prepass already filled this group's InstanceData + DataOffsets (identical objects,
        // same order) and cached the range - REUSE it instead of allocating + re-filling (build once). Falls
        // back to a fresh fill on a miss (no prepass, count mismatch, or ring exhausted).
        u64 offsByteOffset;
        const InstShare* shared =
            m_instShareCache.Find(InstShareKey(ctx.view, head.mesh, head.material));
        if (shared != nullptr && shared->count == count)
        {
            offsByteOffset = shared->offsByteOffset;
        }
        else
        {
            const DynamicUniformRing::Range inst = m_instanceRing.AllocateRange(count);
            const DynamicUniformRing::Range offs = m_offsetsRing.AllocateRange(count);
            if (!inst.ok || !offs.ok)
            {
                return;
            }
            InstanceData* id = static_cast<InstanceData*>(inst.ptr);
            DataOffsets* od = static_cast<DataOffsets*>(offs.ptr);
            for (u32 k = 0; k < count; ++k)
            {
                const auto* md = static_cast<const MeshRenderData*>(items[first + k].data);
                id[k] = InstanceData{
                    md->world, ctx.needsMotion ? PrevWorldFor(md->entityId, md->world) : md->world,
                    md->color};
                u32 boneBase = 0, prevBase = 0;
                if (skinned)
                {
                    if (const BoneSlot* s = m_boneStart.Find(md->boneMatrices))
                    {
                        boneBase = s->base;
                        prevBase = s->prevBase;
                    }
                }
                od[k] = DataOffsets{inst.slotIndex + k, boneBase, prevBase,
                                    0}; // .x=Instances[] idx, .y/.z=bone bases
            }
            offsByteOffset = offs.byteOffset;
        }

        // Shared per-submesh draw state (view/instances/cluster sets + vertex/index buffers + skinning).
        ResolvedDraw base{};
        base.viewSet = m_viewBG;
        base.viewOffset = viewOffset;
        base.viewDynamic = true; // set 0: view
        base.drawSet = m_instanceBG;
        base.drawDynamic = false;    // set 1: instances (whole)
        base.clusterSet = clusterBG; // set 3: cluster lists
        base.vertexBuffer0 = mesh.vertexBuffer;
        base.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            base.vertexBuffer1 = mesh.skinBuffer;
            base.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream (6/7)
            base.vertexBuffer2 = m_offsetsRing.Buffer();
            base.vertexOffset2 = offsByteOffset; // slot 2: DataOffsets (5)
        }
        else
        {
            base.vertexBuffer1 = m_offsetsRing.Buffer();
            base.vertexOffset1 = offsByteOffset; // slot 1: DataOffsets (5)
        }
        base.indexBuffer = mesh.indexBuffer;
        base.indexFormat = mesh.indexFormat;
        base.instanceCount = count;

        // Emit one instanced draw for an index sub-range with `m`'s material (set 2 = its bind group).
        const auto emit = [&](materials::Material* m, u64 indexOffset, u32 indexCount)
        {
            materials::Material* use = (m != nullptr) ? m : mat;
            rhi::BindGroupLayout* set2 = m_materials->GetOrCreateLayout(*use);
            rhi::PipelineLayout* plLayout = GetOrCreatePipelineLayout(set2, /*instanced*/ true);
            if (plLayout == nullptr)
            {
                return;
            }
            rhi::RenderPipeline* pso = m_psoCache->GetPipeline(config, plLayout, ctx.colorFormat);
            if (pso == nullptr)
            {
                return;
            }
            ResolvedDraw d = base;
            d.pso = pso;
            d.materialSet = m_materials->PrepareInstance(*InstanceFor(use), set2);
            d.indexOffset = indexOffset;
            d.indexCount = indexCount;
            out.PushBack(d);
        };

        // Multi-material: one instanced draw per submesh (its own material + index range); else one draw.
        if (head.submeshMaterialCount > 0 && head.mesh != nullptr &&
            !head.mesh->subMeshes.IsEmpty())
        {
            const u64 stride = (mesh.indexFormat == rhi::IndexFormat::UInt16) ? 2u : 4u;
            for (const geometry::SubMesh& sub : head.mesh->subMeshes)
            {
                materials::Material* m =
                    (sub.materialIndex >= 0 &&
                     static_cast<u32>(sub.materialIndex) < head.submeshMaterialCount)
                        ? head.submeshMaterials[sub.materialIndex].Get()
                        : nullptr;
                if (m == nullptr)
                {
                    m = head.material;
                } // OOB/unresolved slot -> slot 0
                emit(m, mesh.indexOffset + static_cast<u64>(sub.startIndex) * stride,
                     static_cast<u32>(sub.indexCount));
            }
        }
        else
        {
            emit(mat, mesh.indexOffset, mesh.indexCount);
        }
    }

    void MeshRenderer::ResolveDepthSingle(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                          const MeshRenderData& md, const GpuMesh& mesh,
                                          Array<ResolvedDraw>& out)
    {
        u32 boneBase = 0;
        bool skinned = md.boneMatrices != nullptr && md.boneCount > 0 && md.mesh != nullptr &&
                       md.mesh->IsSkinned() && mesh.skinBuffer != nullptr;
        // Masked casters cast holey shadows via the alpha-test fragment (needs the material set 2).
        const bool masked = md.material != nullptr &&
                            md.material->pipeline.blendMode == materials::BlendMode::Masked;
        materials::PipelineConfig config = ShadowConfigFor(ctx, /*instanced*/ false, masked);
        if (skinned)
        {
            const BoneSlot* s = m_boneStart.Find(md.boneMatrices);
            if (s != nullptr)
            {
                boneBase = s->base; // shared device pool (uploaded once this frame)
                config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
                config.shaderFlags |= shaders::ShaderFlags::Skinned;
            }
            else
            {
                skinned = false;
            }
        }
        rhi::BindGroup* matSet = nullptr;
        rhi::PipelineLayout* layout = m_shadowPipelineLayoutSingle;
        if (masked)
        {
            rhi::BindGroupLayout* set2 = m_materials->GetOrCreateLayout(*md.material);
            layout = GetOrCreateShadowMaskedLayout(set2, /*instanced*/ false);
            matSet = m_materials->PrepareInstance(*InstanceFor(md.material), set2);
            if (layout == nullptr)
            {
                layout = m_shadowPipelineLayoutSingle;
                matSet = nullptr;
                config = ShadowConfigFor(ctx, false, false);
            }
        }
        rhi::RenderPipeline* pso =
            m_psoCache->GetPipeline(config, layout, rhi::TextureFormat::Undefined);
        if (pso == nullptr)
        {
            return;
        }
        const DynamicUniformRing::Range obj = m_objectRing.Allocate();
        if (!obj.ok)
        {
            return;
        }
        ObjectData od{};
        od.world = md.world;
        od.prevWorld = md.world; // depth pass ignores prevWorld
        od.tint = md.color;
        od.boneBase = boneBase;
        *static_cast<ObjectData*>(obj.ptr) = od;

        ResolvedDraw d{};
        d.pso = pso;
        d.viewSet = m_shadowViewBG;
        d.viewOffset = shadowViewOffset;
        d.viewDynamic = true; // set 0: light view
        d.drawSet = m_objectBG;
        d.drawOffset = obj.byteOffset;
        d.drawDynamic = true;   // set 1: object UBO
        d.materialSet = matSet; // set 2: material (masked casters only - for the alpha-test sample)
        d.vertexBuffer0 = mesh.vertexBuffer;
        d.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            d.vertexBuffer1 = mesh.skinBuffer;
            d.vertexOffset1 = mesh.skinOffset;
        } // buffer 1: skin stream
        d.indexBuffer = mesh.indexBuffer;
        d.indexOffset = mesh.indexOffset;
        d.indexFormat = mesh.indexFormat;
        d.indexCount = mesh.indexCount;
        d.instanceCount = 1;
        out.PushBack(d);
    }

    void MeshRenderer::ResolveDepthInstanced(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                             Span<const DrawItem> items, usize first, u32 count,
                                             const GpuMesh& mesh, Array<ResolvedDraw>& out)
    {
        const auto& head = *static_cast<const MeshRenderData*>(items[first].data);
        const bool skinned = head.boneMatrices != nullptr && head.mesh != nullptr &&
                             head.mesh->IsSkinned() && mesh.skinBuffer != nullptr;
        const bool masked = head.material != nullptr &&
                            head.material->pipeline.blendMode == materials::BlendMode::Masked;
        materials::PipelineConfig config = ShadowConfigFor(ctx, /*instanced*/ true, masked);
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }
        rhi::BindGroup* matSet = nullptr;
        rhi::PipelineLayout* layout = m_shadowPipelineLayoutInstanced;
        if (masked)
        {
            rhi::BindGroupLayout* set2 = m_materials->GetOrCreateLayout(*head.material);
            layout = GetOrCreateShadowMaskedLayout(set2, /*instanced*/ true);
            matSet = m_materials->PrepareInstance(*InstanceFor(head.material), set2);
            if (layout == nullptr)
            {
                layout = m_shadowPipelineLayoutInstanced;
                matSet = nullptr;
                config = ShadowConfigFor(ctx, true, false);
            }
        }
        rhi::RenderPipeline* pso =
            m_psoCache->GetPipeline(config, layout, rhi::TextureFormat::Undefined);
        if (pso == nullptr)
        {
            return;
        }
        const DynamicUniformRing::Range inst = m_instanceRing.AllocateRange(count);
        const DynamicUniformRing::Range offs = m_offsetsRing.AllocateRange(count);
        if (!inst.ok || !offs.ok)
        {
            return;
        }

        InstanceData* id = static_cast<InstanceData*>(inst.ptr);
        DataOffsets* od = static_cast<DataOffsets*>(offs.ptr);
        // When this prepass feeds the forward (camera depth prepass, fillInstanceCache), build the FULL
        // InstanceData incl. REAL prevWorld so the forward can reuse it for motion vectors. Shadow casters
        // leave prevWorld = world (the depth/shadow shaders ignore it - no extra prev-world lookup).
        const bool feedsForward = ctx.fillInstanceCache;
        for (u32 k = 0; k < count; ++k)
        {
            const auto* md = static_cast<const MeshRenderData*>(items[first + k].data);
            const Float4x4 prev = (feedsForward && ctx.needsMotion)
                                      ? PrevWorldFor(md->entityId, md->world)
                                      : md->world;
            id[k] = InstanceData{md->world, prev, md->color};
            u32 boneBase = 0, prevBase = 0;
            if (skinned)
            {
                if (const BoneSlot* s = m_boneStart.Find(md->boneMatrices))
                {
                    boneBase = s->base;
                    prevBase = s->prevBase;
                }
            }
            od[k] = DataOffsets{inst.slotIndex + k, boneBase, prevBase, 0};
        }
        if (feedsForward)
        { // record this group's DataOffsets range for the forward to reuse
            m_instShareCache.InsertOrAssign(InstShareKey(ctx.view, head.mesh, head.material),
                                            InstShare{offs.byteOffset, count});
        }

        ResolvedDraw d{};
        d.pso = pso;
        d.viewSet = m_shadowViewBG;
        d.viewOffset = shadowViewOffset;
        d.viewDynamic = true; // set 0: light view
        d.drawSet = m_instanceBG;
        d.drawDynamic = false;  // set 1: instances (whole)
        d.materialSet = matSet; // set 2: material (masked casters only - alpha-test sample)
        d.vertexBuffer0 = mesh.vertexBuffer;
        d.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            d.vertexBuffer1 = mesh.skinBuffer;
            d.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream
            d.vertexBuffer2 = m_offsetsRing.Buffer();
            d.vertexOffset2 = offs.byteOffset; // slot 2: DataOffsets
        }
        else
        {
            d.vertexBuffer1 = m_offsetsRing.Buffer();
            d.vertexOffset1 = offs.byteOffset; // slot 1: DataOffsets
        }
        d.indexBuffer = mesh.indexBuffer;
        d.indexOffset = mesh.indexOffset;
        d.indexFormat = mesh.indexFormat;
        d.indexCount = mesh.indexCount;
        d.instanceCount = count;
        out.PushBack(d);
    }

    void MeshRenderer::ResolveMultiMesh(const RenderRecordContext& ctx, u32 viewOffset,
                                        rhi::BindGroup* clusterBG, const MultiMeshRenderData& mm,
                                        const GpuMesh& mesh, Array<ResolvedDraw>& out)
    {
        const MultiMeshSet* set = m_multiMeshSets.Find(mm.key);
        if (set == nullptr || set->activeInstanceBG == nullptr || set->count == 0)
        {
            return;
        }
        // Skinned crowd: per-instance bone base in the set's own DataOffsets buffer (+ the skin stream);
        // static: the shared [i,0,0,0] ramp. A skinned set falls back to static if its skin data is missing.
        const bool skinned =
            set->skinned && set->offsetsBuf != nullptr && mesh.skinBuffer != nullptr;
        if (!skinned && m_rampBuffer == nullptr)
        {
            return;
        }
        materials::Material* mat = (mm.material != nullptr) ? mm.material : m_defaultMaterial.Get();
        materials::PipelineConfig config = ConfigFor(mm, ctx, /*instanced*/ true);
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }

        ResolvedDraw base{};
        base.viewSet = m_viewBG;
        base.viewOffset = viewOffset;
        base.viewDynamic = true; // set 0: view
        base.drawSet = set->activeInstanceBG;
        base.drawDynamic = false;    // set 1: this set's persistent instances
        base.clusterSet = clusterBG; // set 3: cluster lists
        base.vertexBuffer0 = mesh.vertexBuffer;
        base.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            base.vertexBuffer1 = mesh.skinBuffer;
            base.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream
            base.vertexBuffer2 = set->offsetsBuf;
            base.vertexOffset2 =
                set->offsetsByteOffset; // slot 2: per-set DataOffsets (this frame's region)
        }
        else
        {
            base.vertexBuffer1 = m_rampBuffer;
            base.vertexOffset1 = 0; // slot 1: shared DataOffsets ramp
        }
        base.indexBuffer = mesh.indexBuffer;
        base.indexFormat = mesh.indexFormat;
        base.instanceCount = set->count;

        const auto emit = [&](materials::Material* m, u64 indexOffset, u32 indexCount)
        {
            materials::Material* use = (m != nullptr) ? m : mat;
            rhi::BindGroupLayout* set2 = m_materials->GetOrCreateLayout(*use);
            rhi::PipelineLayout* plLayout = GetOrCreatePipelineLayout(set2, /*instanced*/ true);
            if (plLayout == nullptr)
            {
                return;
            }
            rhi::RenderPipeline* pso = m_psoCache->GetPipeline(config, plLayout, ctx.colorFormat);
            if (pso == nullptr)
            {
                return;
            }
            ResolvedDraw d = base;
            d.pso = pso;
            d.materialSet = m_materials->PrepareInstance(*InstanceFor(use), set2);
            d.indexOffset = indexOffset;
            d.indexCount = indexCount;
            out.PushBack(d);
        };

        if (mm.submeshMaterialCount > 0 && mm.mesh != nullptr && !mm.mesh->subMeshes.IsEmpty())
        {
            const u64 stride = (mesh.indexFormat == rhi::IndexFormat::UInt16) ? 2u : 4u;
            for (const geometry::SubMesh& sub : mm.mesh->subMeshes)
            {
                materials::Material* m =
                    (sub.materialIndex >= 0 &&
                     static_cast<u32>(sub.materialIndex) < mm.submeshMaterialCount)
                        ? mm.submeshMaterials[sub.materialIndex].Get()
                        : nullptr;
                if (m == nullptr)
                {
                    m = mm.material;
                } // OOB/unresolved slot -> slot 0
                emit(m, mesh.indexOffset + static_cast<u64>(sub.startIndex) * stride,
                     static_cast<u32>(sub.indexCount));
            }
        }
        else
        {
            emit(mat, mesh.indexOffset, mesh.indexCount);
        }
    }

    void MeshRenderer::ResolveMultiMeshDepth(const RenderRecordContext& ctx, u32 shadowViewOffset,
                                             const MultiMeshRenderData& mm, const GpuMesh& mesh,
                                             Array<ResolvedDraw>& out)
    {
        const MultiMeshSet* set = m_multiMeshSets.Find(mm.key);
        if (set == nullptr || set->activeInstanceBG == nullptr || set->count == 0)
        {
            return;
        }
        const bool skinned =
            set->skinned && set->offsetsBuf != nullptr && mesh.skinBuffer != nullptr;
        if (!skinned && m_rampBuffer == nullptr)
        {
            return;
        }
        const bool masked = mm.material != nullptr &&
                            mm.material->pipeline.blendMode == materials::BlendMode::Masked;
        materials::PipelineConfig config = ShadowConfigFor(ctx, /*instanced*/ true, masked);
        if (skinned)
        {
            config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
            config.shaderFlags |= shaders::ShaderFlags::Skinned;
        }
        rhi::BindGroup* matSet = nullptr;
        rhi::PipelineLayout* layout = m_shadowPipelineLayoutInstanced;
        if (masked)
        {
            rhi::BindGroupLayout* set2 = m_materials->GetOrCreateLayout(*mm.material);
            layout = GetOrCreateShadowMaskedLayout(set2, /*instanced*/ true);
            matSet = m_materials->PrepareInstance(*InstanceFor(mm.material), set2);
            if (layout == nullptr)
            {
                layout = m_shadowPipelineLayoutInstanced;
                matSet = nullptr;
                config = ShadowConfigFor(ctx, true, false);
                if (skinned)
                {
                    config.vertexLayout = materials::VertexLayoutType::SkinnedMesh;
                    config.shaderFlags |= shaders::ShaderFlags::Skinned;
                }
            }
        }
        rhi::RenderPipeline* pso =
            m_psoCache->GetPipeline(config, layout, rhi::TextureFormat::Undefined);
        if (pso == nullptr)
        {
            return;
        }
        ResolvedDraw d{};
        d.pso = pso;
        d.viewSet = m_shadowViewBG;
        d.viewOffset = shadowViewOffset;
        d.viewDynamic = true; // set 0: light view
        d.drawSet = set->activeInstanceBG;
        d.drawDynamic = false; // set 1: this set's instances
        d.materialSet = matSet;
        d.vertexBuffer0 = mesh.vertexBuffer;
        d.vertexOffset0 = mesh.vertexOffset;
        if (skinned)
        {
            d.vertexBuffer1 = mesh.skinBuffer;
            d.vertexOffset1 = mesh.skinOffset; // slot 1: skin stream
            d.vertexBuffer2 = set->offsetsBuf;
            d.vertexOffset2 =
                set->offsetsByteOffset; // slot 2: per-set DataOffsets (this frame's region)
        }
        else
        {
            d.vertexBuffer1 = m_rampBuffer;
            d.vertexOffset1 = 0; // slot 1: shared DataOffsets ramp
        }
        d.indexBuffer = mesh.indexBuffer;
        d.indexOffset = mesh.indexOffset;
        d.indexFormat = mesh.indexFormat;
        d.indexCount = mesh.indexCount;
        d.instanceCount = set->count;
        out.PushBack(d);
    }

    materials::PipelineConfig MeshRenderer::ShadowConfigFor(const RenderRecordContext& ctx,
                                                            bool instanced, bool masked)
    {
        materials::PipelineConfig c{};
        c.shaderName = u8"shadow_depth";
        c.vertexLayout = materials::VertexLayoutType::Mesh;
        c.instanced = instanced;
        if (instanced)
        {
            c.shaderFlags |= shaders::ShaderFlags::Instanced;
        }
        // Masked casters run the alpha-test fragment (samples cutout alpha -> discard) so their shadows
        // have holes; opaque casters stay vertex-only (depthOnly, no fragment).
        c.depthOnly = !masked;
        c.colorTargetCount = 0;
        if (masked)
        {
            c.shaderFlags |= shaders::ShaderFlags::AlphaTest;
        }
        c.depthFormat = ctx.depthFormat;
        c.depthMode = materials::DepthMode::ReadWrite;
        c.depthCompare = rhi::CompareFunction::Less;
        // Render FRONT faces into the shadow map (cull back) - matches Sedulous (ShadowPipeline: .Back)
        // and is the conventional default: flat/architectural casters get tight contacts. CURVED casters
        // (spheres) keep a small grazing-contact gap inherent to shadow maps; the general fix is a later
        // contact/screen-space shadow pass, not a cull-mode or bias change (back-face culling only trades
        // the gap onto flat casters, which are far more common).
        c.cullMode = materials::CullModeConfig::Back;
        // Hardware depth bias (ported from Sedulous): a constant offset + a slope-scaled term, applied
        // in shadow-map depth space (so it adds little visible spatial gap, unlike the normal-offset).
        // Pairs with the receiver-side (1 - NdotL) normal-offset bias in forward.frag for acne control.
        // The camera depth PREPASS must NOT bias - its depth has to equal the forward pass's exactly so
        // the LessEqual early-Z accepts the re-drawn opaque fragments (bias would z-fight / reject them).
        if (!ctx.depthPrepass)
        {
            c.depthBias = 50;
            c.depthBiasSlopeScale = 1.5f;
        }
        return c;
    }

    materials::PipelineConfig MeshRenderer::ConfigFor(const MeshRenderData& md,
                                                      const RenderRecordContext& ctx,
                                                      bool instanced)
    {
        materials::PipelineConfig config =
            (md.material != nullptr) ? md.material->pipeline
                                     : materials::PipelineConfig::ForOpaqueMesh(u8"forward");
        config.depthFormat = ctx.depthFormat;
        config.instanced = instanced;
        if (instanced)
        {
            config.shaderFlags |= shaders::ShaderFlags::Instanced;
        }
        // Opaque + masked render the MRT G-buffer pass: target 0 = shaded color (format overridden
        // per-view at build); targets 1/2 = view-space normal + motion vector (the GBUFFER permutation
        // writes them). Transparent renders a separate color-only pass, so it stays single-target.
        const bool gbuffer = (config.blendMode == materials::BlendMode::Opaque ||
                              config.blendMode == materials::BlendMode::Masked);
        if (gbuffer)
        {
            config.colorTargetCount = 4;
            config.colorFormats[1] = kGNormalFormat;
            config.colorFormats[2] = kGVelocityFormat;
            config.colorFormats[3] = kGMaterialFormat; // SSR: roughness/metallic
            config.shaderFlags |= shaders::ShaderFlags::GBuffer;
            // Equal-depth fragments from the depth prepass must pass (early-Z shades each opaque pixel once).
            config.depthCompare = rhi::CompareFunction::LessEqual;
            // Masked: enable the alpha-test discard permutation (opaque-like, but cuts sub-cutoff pixels).
            if (config.blendMode == materials::BlendMode::Masked)
            {
                config.shaderFlags |= shaders::ShaderFlags::AlphaTest;
            }
        }
        else
        {
            config.colorTargetCount = 1; // transparent: color-only
        }
        return config;
    }

    bool MeshRenderer::MakeLayout(const rhi::BindGroupLayoutEntry& entry,
                                  rhi::BindGroupLayout*& out)
    {
        rhi::BindGroupLayoutDesc ld{};
        ld.entries = Span<const rhi::BindGroupLayoutEntry>{&entry, 1};
        return m_device->CreateBindGroupLayout(ld, out).IsOk();
    }

    bool MeshRenderer::MakePipelineLayout(rhi::BindGroupLayout* set0, rhi::BindGroupLayout* set1,
                                          rhi::BindGroupLayout* set2, rhi::BindGroupLayout* set3,
                                          rhi::PipelineLayout*& out)
    {
        rhi::BindGroupLayout* layouts[] = {set0, set1, set2, set3};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 4};
        return m_device->CreatePipelineLayout(pld, out).IsOk();
    }

    bool MeshRenderer::MakePipelineLayout(rhi::BindGroupLayout* set0, rhi::BindGroupLayout* set1,
                                          rhi::PipelineLayout*& out)
    {
        rhi::BindGroupLayout* layouts[] = {set0, set1};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 2};
        return m_device->CreatePipelineLayout(pld, out).IsOk();
    }

    bool MeshRenderer::MakePipelineLayout3(rhi::BindGroupLayout* set0, rhi::BindGroupLayout* set1,
                                           rhi::BindGroupLayout* set2, rhi::PipelineLayout*& out)
    {
        rhi::BindGroupLayout* layouts[] = {set0, set1, set2};
        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 3};
        return m_device->CreatePipelineLayout(pld, out).IsOk();
    }

    rhi::PipelineLayout* MeshRenderer::GetOrCreateShadowMaskedLayout(rhi::BindGroupLayout* set2,
                                                                     bool instanced)
    {
        const u64 key = (reinterpret_cast<u64>(set2) * 2u) + (instanced ? 1u : 0u);
        if (rhi::PipelineLayout** cached = m_shadowMaskedLayouts.Find(key))
        {
            return *cached;
        }
        rhi::BindGroupLayout* set1 = instanced ? m_instanceLayout : m_objectLayout;
        rhi::PipelineLayout* layout = nullptr;
        if (!MakePipelineLayout3(m_shadowViewLayout, set1, set2, layout))
        {
            return nullptr;
        }
        m_shadowMaskedLayouts.InsertOrAssign(key, layout);
        return layout;
    }

    Status MeshRenderer::CreateDefaultMaterial()
    {
        m_defaultMaterial = materials::CreatePBR(u8"__default_pbr");
        return (m_defaultMaterial.Get() != nullptr) ? Status{} : Status{ErrorCode::Unknown};
    }

    materials::MaterialInstance* MeshRenderer::InstanceFor(materials::Material* material)
    {
        // Keyed by the material's UID, never its pointer: a hot-reloaded material can
        // reallocate at the freed address and would silently reuse the STALE instance
        // (whose bind group references destroyed texture views).
        if (materials::MaterialInstance** found = m_instances.Find(material->uid))
        {
            return *found;
        }
        UniquePtr<materials::MaterialInstance> created =
            MakeUnique<materials::MaterialInstance>(DefaultAllocator(), material);
        materials::MaterialInstance* inst = created.Get();
        m_instanceStorage.PushBack(Move(created));       // owns the instance
        m_instances.InsertOrAssign(material->uid, inst); // lookup (storage owns the instance)
        return inst;
    }

    rhi::BindGroup* MeshRenderer::MaterialBindGroup(materials::Material* material)
    {
        rhi::BindGroupLayout* layout = m_materials->GetOrCreateLayout(*material);
        return m_materials->PrepareInstance(*InstanceFor(material), layout);
    }

    rhi::PipelineLayout* MeshRenderer::GetOrCreatePipelineLayout(rhi::BindGroupLayout* set2,
                                                                 bool instanced)
    {
        const u64 key = (reinterpret_cast<u64>(set2) * 2u) + (instanced ? 1u : 0u);
        if (rhi::PipelineLayout** cached = m_pipelineLayouts.Find(key))
        {
            return *cached;
        }
        rhi::BindGroupLayout* set1 = instanced ? m_instanceLayout : m_objectLayout;
        rhi::PipelineLayout* layout = nullptr;
        if (!MakePipelineLayout(m_viewLayout, set1, set2, m_clusterLayout, layout))
        {
            return nullptr;
        }
        m_pipelineLayouts.InsertOrAssign(key, layout);
        return layout;
    }

    void MeshRenderer::RetireBindGroup(rhi::BindGroup* bg)
    {
        if (bg != nullptr)
        {
            m_retiredBGs.PushBack(RetiredBG{bg, m_framesInFlight});
        }
    }

    void MeshRenderer::PruneStaleMaterialInstances()
    {
        usize w = 0;
        for (usize i = 0; i < m_instanceStorage.Size(); ++i)
        {
            materials::MaterialInstance* inst = m_instanceStorage[i].Get();
            materials::Material* material = inst->GetMaterial();
            if (material != nullptr && material->RefCount() <= 1)
            {
                RetireBindGroup(m_materials->DetachBindGroup(inst));
                RetireBuffer(m_materials->DetachUniformBuffer(inst));
                m_instances.Remove(material->uid);
                continue; // UniquePtr slot dropped -> instance destroyed (bg already detached)
            }
            if (w != i)
            {
                m_instanceStorage[w] = Move(m_instanceStorage[i]);
            }
            ++w;
        }
        m_instanceStorage.Resize(w);
    }

    void MeshRenderer::TickRetiredBindGroups()
    {
        usize w = 0;
        for (usize i = 0; i < m_retiredBGs.Size(); ++i)
        {
            RetiredBG r = m_retiredBGs[i];
            if (r.framesLeft <= 1)
            {
                m_device->DestroyBindGroup(r.bg);
            }
            else
            {
                r.framesLeft -= 1;
                m_retiredBGs[w++] = r;
            }
        }
        m_retiredBGs.Resize(w);
        w = 0;
        for (usize i = 0; i < m_retiredBuffers.Size(); ++i)
        {
            RetiredBuffer r = m_retiredBuffers[i];
            if (r.framesLeft <= 1)
            {
                m_device->DestroyBuffer(r.buffer);
            }
            else
            {
                r.framesLeft -= 1;
                m_retiredBuffers[w++] = r;
            }
        }
        m_retiredBuffers.Resize(w);
    }

    void MeshRenderer::RetireBuffer(rhi::Buffer* buffer)
    {
        if (buffer != nullptr)
        {
            m_retiredBuffers.PushBack(RetiredBuffer{buffer, m_framesInFlight});
        }
    }

    bool MeshRenderer::EnsureBindGroup(DynamicUniformRing& ring, rhi::BindGroupLayout* layout,
                                       u64 bindSize, rhi::BindGroup*& bg, u32& bgGen, bool whole)
    {
        if (bg != nullptr && bgGen == ring.Generation())
        {
            return true;
        }
        RetireBindGroup(bg);
        bg = nullptr;
        rhi::Buffer* buffer = ring.Buffer();
        if (buffer == nullptr)
        {
            return false;
        }
        rhi::BindGroupEntry be =
            rhi::BindGroupEntry::BufferEntry(buffer, 0, whole ? ring.ByteCapacity() : bindSize);
        rhi::BindGroupDesc bgd{};
        bgd.layout = layout;
        bgd.entries = Span<const rhi::BindGroupEntry>{&be, 1};
        if (!m_device->CreateBindGroup(bgd, bg).IsOk())
        {
            bg = nullptr;
            return false;
        }
        bgGen = ring.Generation();
        return true;
    }

    bool MeshRenderer::EnsureViewBindGroup(const RenderRecordContext& ctx)
    {
        if (m_activeShadowView == nullptr)
        {
            m_activeShadowView = m_dummyShadowView;
        }
        if (m_activeAtlasView == nullptr)
        {
            m_activeAtlasView = m_dummyAtlasView;
        }
        if (m_activeProbeCube == nullptr)
        {
            m_activeProbeCube = m_dummyProbeCubeView;
        }
        if (m_activeProbeBuffer == nullptr)
        {
            m_activeProbeBuffer = m_dummyProbeBuffer;
        }
        // This view's SCENE's IBL products (dummies when IBL is off/unavailable).
        const bool iblActive = ctx.ibl.valid && ctx.ibl.shBuffer != nullptr &&
                               ctx.ibl.prefilterView != nullptr && ctx.ibl.brdfView != nullptr;
        rhi::Buffer* sh = iblActive ? ctx.ibl.shBuffer : m_dummyShBuffer;
        rhi::TextureView* prefilter = iblActive ? ctx.ibl.prefilterView : m_dummyCubeView;
        rhi::TextureView* brdf = iblActive ? ctx.ibl.brdfView : m_dummyBrdfView;
        const u64 iblGen = iblActive ? ctx.ibl.generation : 0;

        if (m_viewBGs.Size() <= ctx.viewIndex)
        {
            m_viewBGs.Resize(ctx.viewIndex + 1u);
        }
        ViewBGSlot& slot = m_viewBGs[ctx.viewIndex];
        if (slot.bg != nullptr && slot.viewGen == m_viewRing.Generation() &&
            slot.lightGen == m_lightRing.Generation() && slot.shadow == m_activeShadowView &&
            slot.shadowGen == m_activeShadowGen && slot.atlas == m_activeAtlasView &&
            slot.atlasGen == m_activeAtlasGen && slot.localGen == m_localShadowRing.Generation() &&
            slot.boneGen == m_boneDeviceGen && slot.iblGen == iblGen &&
            slot.prefilter == prefilter && slot.probeCube == m_activeProbeCube &&
            slot.probeBuf == m_activeProbeBuffer)
        {
            m_viewBG = slot.bg;
            return true;
        }
        RetireBindGroup(slot.bg);
        slot.bg = nullptr;
        m_viewBG = nullptr;
        rhi::Buffer* viewBuf = m_viewRing.Buffer();
        rhi::Buffer* lightBuf = m_lightRing.Buffer();
        rhi::Buffer* localBuf = m_localShadowRing.Buffer();
        rhi::Buffer* boneBuf = m_boneDevice; // VS reads the device mirror, not the staging ring
        if (viewBuf == nullptr || lightBuf == nullptr || localBuf == nullptr ||
            boneBuf == nullptr || m_activeShadowView == nullptr || m_activeAtlasView == nullptr ||
            m_shadowSampler == nullptr || sh == nullptr || prefilter == nullptr ||
            brdf == nullptr || m_envSampler == nullptr)
        {
            return false;
        }
        // Order must match the set-0 layout: view UBO, lights, cascade map (t1), local atlas (t2),
        // local-shadow entries (t3), comparison sampler (s0), bone-matrix pool (t4), IBL SH9 (t5),
        // prefilter cube (t6), BRDF LUT (t7), env sampler (s1). Buffers bound whole.
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(viewBuf, 0, sizeof(ViewData)),
            rhi::BindGroupEntry::BufferEntry(lightBuf, 0, m_lightRing.ByteCapacity()),
            rhi::BindGroupEntry::TextureEntry(m_activeShadowView),
            rhi::BindGroupEntry::TextureEntry(m_activeAtlasView),
            rhi::BindGroupEntry::BufferEntry(localBuf, 0, m_localShadowRing.ByteCapacity()),
            rhi::BindGroupEntry::SamplerEntry(m_shadowSampler),
            rhi::BindGroupEntry::BufferEntry(boneBuf, 0, m_boneDeviceBytes),
            rhi::BindGroupEntry::BufferEntry(sh, 0, kShBytes),
            rhi::BindGroupEntry::TextureEntry(prefilter),
            rhi::BindGroupEntry::TextureEntry(brdf),
            rhi::BindGroupEntry::SamplerEntry(m_envSampler),
            rhi::BindGroupEntry::TextureEntry(m_activeProbeCube),
            rhi::BindGroupEntry::BufferEntry(m_activeProbeBuffer, 0, kProbeBufferBytes),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_viewLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 13};
        if (!m_device->CreateBindGroup(bgd, slot.bg).IsOk())
        {
            slot.bg = nullptr;
            return false;
        }
        slot.viewGen = m_viewRing.Generation();
        slot.lightGen = m_lightRing.Generation();
        slot.shadow = m_activeShadowView;
        slot.shadowGen = m_activeShadowGen;
        slot.atlas = m_activeAtlasView;
        slot.atlasGen = m_activeAtlasGen;
        slot.localGen = m_localShadowRing.Generation();
        slot.boneGen = m_boneDeviceGen;
        slot.iblGen = iblGen;
        slot.prefilter = prefilter;
        slot.probeCube = m_activeProbeCube;
        slot.probeBuf = m_activeProbeBuffer;
        m_viewBG = slot.bg;
        return true;
    }

    Status MeshRenderer::CreateShadowResources()
    {
        rhi::SamplerDesc sd{};
        sd.minFilter = rhi::FilterMode::Linear;
        sd.magFilter = rhi::FilterMode::Linear;
        sd.mipmapFilter = rhi::MipmapFilterMode::Nearest;
        sd.addressU = rhi::AddressMode::ClampToEdge;
        sd.addressV = rhi::AddressMode::ClampToEdge;
        sd.addressW = rhi::AddressMode::ClampToEdge;
        sd.compare = rhi::CompareFunction::LessEqual; // lit when fragment depth <= stored depth
        sd.label = u8"mesh.shadowSampler";
        if (!m_device->CreateSampler(sd, m_shadowSampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::TextureDesc td{};
        td.format = rhi::TextureFormat::Depth32Float;
        td.width = 1;
        td.height = 1;
        td.arrayLayerCount = 1;
        td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        td.label = u8"mesh.dummyShadow";
        if (!m_device->CreateTexture(td, m_dummyShadowTex).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // A Texture2DArray view (1 layer) so it matches the shader's Texture2DArray shadow binding.
        rhi::TextureViewDesc vd{};
        vd.format = rhi::TextureFormat::Depth32Float;
        vd.aspect = rhi::TextureAspect::DepthOnly;
        vd.dimension = rhi::TextureViewDimension::Texture2DArray;
        vd.arrayLayerCount = 1;
        if (!m_device->CreateTextureView(m_dummyShadowTex, vd, m_dummyShadowView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        m_activeShadowView = m_dummyShadowView;

        // A 1x1 Texture2D dummy for the local-shadow atlas (t2) + a 1-element dummy data buffer (t3),
        // bound when no local shadow caster exists this frame (the descriptor set stays complete).
        rhi::TextureDesc atd{};
        atd.format = rhi::TextureFormat::Depth32Float;
        atd.width = 1;
        atd.height = 1;
        atd.arrayLayerCount = 2;
        atd.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        atd.label = u8"mesh.dummyAtlas";
        if (!m_device->CreateTexture(atd, m_dummyAtlasTex).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc avd{};
        avd.format = rhi::TextureFormat::Depth32Float;
        avd.aspect = rhi::TextureAspect::DepthOnly;
        avd.dimension = rhi::TextureViewDimension::Texture2DArray;
        avd.arrayLayerCount = 2;
        if (!m_device->CreateTextureView(m_dummyAtlasTex, avd, m_dummyAtlasView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        m_activeAtlasView = m_dummyAtlasView;

        // IBL fallbacks (bound when no environment is active): a zero-filled SH9 buffer (=> no diffuse
        // ambient), a 1x1x6 black cube (prefilter), a 1x1 black BRDF LUT, and a linear-clamp env
        // sampler. The color dummies transition UNDEFINED->ShaderRead once (with the depth dummies).
        rhi::SamplerDesc es{};
        es.minFilter = rhi::FilterMode::Linear;
        es.magFilter = rhi::FilterMode::Linear;
        es.mipmapFilter = rhi::MipmapFilterMode::Linear;
        es.addressU = rhi::AddressMode::ClampToEdge;
        es.addressV = rhi::AddressMode::ClampToEdge;
        es.addressW = rhi::AddressMode::ClampToEdge;
        es.label = u8"mesh.envSampler";
        if (!m_device->CreateSampler(es, m_envSampler).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::BufferDesc shd{};
        shd.size = kShBytes;
        shd.usage = rhi::BufferUsage::Storage;
        shd.memory = rhi::MemoryLocation::GpuOnly;
        shd.label = u8"mesh.dummySH";
        if (!m_device->CreateBuffer(shd, m_dummyShBuffer).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::TextureDesc cd{};
        cd.format = rhi::TextureFormat::RGBA16Float;
        cd.width = 1;
        cd.height = 1;
        cd.arrayLayerCount = 6;
        cd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        cd.label = u8"mesh.dummyCube";
        if (!m_device->CreateTexture(cd, m_dummyCube).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc cvd{};
        cvd.format = rhi::TextureFormat::RGBA16Float;
        cvd.dimension = rhi::TextureViewDimension::TextureCube;
        cvd.arrayLayerCount = 6;
        if (!m_device->CreateTextureView(m_dummyCube, cvd, m_dummyCubeView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        rhi::TextureDesc ld{};
        ld.format = rhi::TextureFormat::RG16Float;
        ld.width = 1;
        ld.height = 1;
        ld.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        ld.label = u8"mesh.dummyBRDF";
        if (!m_device->CreateTexture(ld, m_dummyBrdf).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc lvd{};
        lvd.format = rhi::TextureFormat::RG16Float;
        lvd.dimension = rhi::TextureViewDimension::Texture2D;
        if (!m_device->CreateTextureView(m_dummyBrdf, lvd, m_dummyBrdfView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        // Dummy probe cube-ARRAY (6 layers = 1 cube) bound when no probe is active - count 0 keeps the
        // forward on the global IBL reflection, so the content is irrelevant.
        rhi::TextureDesc pcd{};
        pcd.format = rhi::TextureFormat::RGBA16Float;
        pcd.width = 1;
        pcd.height = 1;
        pcd.arrayLayerCount = 6;
        pcd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        pcd.label = u8"mesh.dummyProbeCube";
        if (!m_device->CreateTexture(pcd, m_dummyProbeCube).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::TextureViewDesc pcv{};
        pcv.format = rhi::TextureFormat::RGBA16Float;
        pcv.dimension = rhi::TextureViewDimension::TextureCubeArray;
        pcv.arrayLayerCount = 6;
        if (!m_device->CreateTextureView(m_dummyProbeCube, pcv, m_dummyProbeCubeView).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // Dummy probe-metadata buffer (t9) for the no-probe path - never read (count 0), just a valid SRV.
        rhi::BufferDesc pbd{};
        pbd.size = kProbeBufferBytes;
        pbd.usage = rhi::BufferUsage::Storage;
        pbd.memory = rhi::MemoryLocation::GpuOnly;
        pbd.label = u8"mesh.dummyProbeBuf";
        if (!m_device->CreateBuffer(pbd, m_dummyProbeBuffer).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }

        m_activeProbeCube = m_dummyProbeCubeView;
        m_activeProbeBuffer = m_dummyProbeBuffer;
        return Status{};
    }

    bool MeshRenderer::EnsureShadowViewBindGroup()
    {
        if (m_shadowViewBG != nullptr && m_shadowViewBGGen == m_shadowViewRing.Generation() &&
            m_shadowViewBGBoneGen == m_boneDeviceGen)
        {
            return true;
        }
        RetireBindGroup(m_shadowViewBG);
        m_shadowViewBG = nullptr;
        rhi::Buffer* buf = m_shadowViewRing.Buffer();
        rhi::Buffer* boneBuf = m_boneDevice; // shared device mirror (same as the forward set 0)
        if (buf == nullptr || boneBuf == nullptr)
        {
            return false;
        }
        rhi::BindGroupEntry be[] = {
            rhi::BindGroupEntry::BufferEntry(buf, 0, sizeof(ShadowViewData)),
            rhi::BindGroupEntry::BufferEntry(boneBuf, 0,
                                             m_boneDeviceBytes), // t4: skinning pool
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_shadowViewLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{be, 2};
        if (!m_device->CreateBindGroup(bgd, m_shadowViewBG).IsOk())
        {
            m_shadowViewBG = nullptr;
            return false;
        }
        m_shadowViewBGGen = m_shadowViewRing.Generation();
        m_shadowViewBGBoneGen = m_boneDeviceGen;
        return true;
    }

    Status MeshRenderer::CreateDummyClusters()
    {
        rhi::BufferDesc obd{};
        obd.size = sizeof(u32) * 2;
        obd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
        obd.memory = rhi::MemoryLocation::GpuOnly;
        obd.label = u8"cluster.dummyOffsets";
        if (!m_device->CreateBuffer(obd, m_dummyClusterOffsets).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        rhi::BufferDesc ibd{};
        ibd.size = sizeof(u32);
        ibd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst;
        ibd.memory = rhi::MemoryLocation::GpuOnly;
        ibd.label = u8"cluster.dummyIndices";
        if (!m_device->CreateBuffer(ibd, m_dummyClusterIndices).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        // Zero-initialize dummy cluster buffers. DX12 default-heap buffers have undefined
        // contents; the forward shader reads ClusterOffsets[].y as a loop count, so garbage
        // data causes an infinite GPU loop (TDR). Vulkan zero-initializes by luck.
        {
            rhi::Queue* q = m_device->GetQueue(rhi::QueueType::Graphics);
            rhi::TransferBatch* batch = nullptr;
            if (q != nullptr && q->CreateTransferBatch(batch).IsOk() && batch != nullptr)
            {
                u32 zeros[2] = {0, 0};
                batch->WriteBuffer(m_dummyClusterOffsets, 0,
                                   Span<const u8>(reinterpret_cast<const u8*>(zeros), sizeof(zeros)));
                u32 zero = 0;
                batch->WriteBuffer(m_dummyClusterIndices, 0,
                                   Span<const u8>(reinterpret_cast<const u8*>(&zero), sizeof(zero)));
                batch->Submit();
                q->DestroyTransferBatch(batch);
            }
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(m_dummyClusterOffsets, 0, sizeof(u32) * 2),
            rhi::BindGroupEntry::BufferEntry(m_dummyClusterIndices, 0, sizeof(u32)),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_clusterLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
        if (!m_device->CreateBindGroup(bgd, m_dummyClusterBG).IsOk())
        {
            return Status{ErrorCode::Unknown};
        }
        return Status{};
    }

    rhi::BindGroup* MeshRenderer::EnsureClusterBindGroup(u32 slot, rhi::Buffer* offsets,
                                                         rhi::Buffer* indices, u32 version)
    {
        if (slot >= kMaxClusterSlots || offsets == nullptr || indices == nullptr)
        {
            return m_dummyClusterBG;
        }
        if (m_clusterBGs[slot] != nullptr && m_clusterBGOffsets[slot] == offsets &&
            m_clusterBGVersion[slot] == version)
        {
            return m_clusterBGs[slot];
        }
        if (m_clusterBGs[slot] != nullptr)
        {
            m_device->DestroyBindGroup(m_clusterBGs[slot]);
            m_clusterBGs[slot] = nullptr;
        }
        rhi::BindGroupEntry entries[] = {
            rhi::BindGroupEntry::BufferEntry(offsets, 0, offsets->desc.size),
            rhi::BindGroupEntry::BufferEntry(indices, 0, indices->desc.size),
        };
        rhi::BindGroupDesc bgd{};
        bgd.layout = m_clusterLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
        if (!m_device->CreateBindGroup(bgd, m_clusterBGs[slot]).IsOk())
        {
            m_clusterBGs[slot] = nullptr;
            return m_dummyClusterBG;
        }
        m_clusterBGOffsets[slot] = offsets;
        m_clusterBGVersion[slot] = version;
        return m_clusterBGs[slot];
    }

    void MeshRenderer::Shutdown()
    {
        // Release material instances first (their dtors notify the still-live MaterialSystem, which
        // owns + destroys their data-driven bind groups). The default material's RefPtr then drops.
        m_instances.Clear();
        m_instanceStorage.Clear();
        m_defaultMaterial.Reset();
        m_meshes.Clear();
        for (RetiredBG& r : m_retiredBGs)
        {
            m_device->DestroyBindGroup(r.bg);
        }
        m_retiredBGs.Clear();
        for (RetiredBuffer& r : m_retiredBuffers)
        {
            m_device->DestroyBuffer(r.buffer);
        }
        m_retiredBuffers.Clear();
        for (ViewBGSlot& slot : m_viewBGs)
        {
            if (slot.bg != nullptr)
            {
                m_device->DestroyBindGroup(slot.bg);
                slot.bg = nullptr;
            }
        }
        m_viewBGs.Clear();
        m_viewBG = nullptr; // borrowed from a slot - already destroyed above
        if (m_shadowViewBG)
        {
            m_device->DestroyBindGroup(m_shadowViewBG);
            m_shadowViewBG = nullptr;
        }
        if (m_boneDevice)
        {
            m_device->DestroyBuffer(m_boneDevice);
            m_boneDevice = nullptr;
            m_boneDeviceBytes = 0;
        }
        if (m_dummyShadowView)
        {
            m_device->DestroyTextureView(m_dummyShadowView);
            m_dummyShadowView = nullptr;
        }
        if (m_dummyShadowTex)
        {
            m_device->DestroyTexture(m_dummyShadowTex);
            m_dummyShadowTex = nullptr;
        }
        if (m_dummyAtlasView)
        {
            m_device->DestroyTextureView(m_dummyAtlasView);
            m_dummyAtlasView = nullptr;
        }
        if (m_dummyAtlasTex)
        {
            m_device->DestroyTexture(m_dummyAtlasTex);
            m_dummyAtlasTex = nullptr;
        }
        if (m_shadowSampler)
        {
            m_device->DestroySampler(m_shadowSampler);
            m_shadowSampler = nullptr;
        }
        if (m_dummyCubeView)
        {
            m_device->DestroyTextureView(m_dummyCubeView);
            m_dummyCubeView = nullptr;
        }
        if (m_dummyCube)
        {
            m_device->DestroyTexture(m_dummyCube);
            m_dummyCube = nullptr;
        }
        if (m_dummyBrdfView)
        {
            m_device->DestroyTextureView(m_dummyBrdfView);
            m_dummyBrdfView = nullptr;
        }
        if (m_dummyBrdf)
        {
            m_device->DestroyTexture(m_dummyBrdf);
            m_dummyBrdf = nullptr;
        }
        if (m_dummyProbeCubeView)
        {
            m_device->DestroyTextureView(m_dummyProbeCubeView);
            m_dummyProbeCubeView = nullptr;
        }
        if (m_dummyProbeCube)
        {
            m_device->DestroyTexture(m_dummyProbeCube);
            m_dummyProbeCube = nullptr;
        }
        if (m_dummyProbeBuffer)
        {
            m_device->DestroyBuffer(m_dummyProbeBuffer);
            m_dummyProbeBuffer = nullptr;
        }
        if (m_dummyShBuffer)
        {
            m_device->DestroyBuffer(m_dummyShBuffer);
            m_dummyShBuffer = nullptr;
        }
        if (m_envSampler)
        {
            m_device->DestroySampler(m_envSampler);
            m_envSampler = nullptr;
        }
        if (m_objectBG)
        {
            m_device->DestroyBindGroup(m_objectBG);
            m_objectBG = nullptr;
        }
        if (m_instanceBG)
        {
            m_device->DestroyBindGroup(m_instanceBG);
            m_instanceBG = nullptr;
        }
        // MultiMesh persistent per-set buffers/bind groups + the shared ramp.
        for (auto& kv : m_multiMeshSets)
        {
            for (u32 r = 0; r < kMultiMeshMaxFiF; ++r)
            {
                if (kv.value.instanceBG[r] != nullptr)
                {
                    m_device->DestroyBindGroup(kv.value.instanceBG[r]);
                }
            }
            if (kv.value.instanceBuf != nullptr)
            {
                m_device->DestroyBuffer(kv.value.instanceBuf);
            }
            if (kv.value.offsetsBuf != nullptr)
            {
                m_device->DestroyBuffer(kv.value.offsetsBuf);
            }
        }
        m_multiMeshSets.Clear();
        if (m_rampBuffer)
        {
            m_device->DestroyBuffer(m_rampBuffer);
            m_rampBuffer = nullptr;
            m_rampCapacity = 0;
        }
        for (u32 i = 0; i < kMaxClusterSlots; ++i)
        {
            if (m_clusterBGs[i] != nullptr)
            {
                m_device->DestroyBindGroup(m_clusterBGs[i]);
                m_clusterBGs[i] = nullptr;
            }
        }
        if (m_dummyClusterBG)
        {
            m_device->DestroyBindGroup(m_dummyClusterBG);
            m_dummyClusterBG = nullptr;
        }
        if (m_dummyClusterOffsets)
        {
            m_device->DestroyBuffer(m_dummyClusterOffsets);
            m_dummyClusterOffsets = nullptr;
        }
        if (m_dummyClusterIndices)
        {
            m_device->DestroyBuffer(m_dummyClusterIndices);
            m_dummyClusterIndices = nullptr;
        }
        for (auto& kv : m_pipelineLayouts)
        {
            if (kv.value != nullptr)
            {
                m_device->DestroyPipelineLayout(kv.value);
            }
        }
        m_pipelineLayouts.Clear();
        for (auto& kv : m_shadowMaskedLayouts)
        {
            if (kv.value != nullptr)
            {
                m_device->DestroyPipelineLayout(kv.value);
            }
        }
        m_shadowMaskedLayouts.Clear();
        if (m_shadowPipelineLayoutSingle)
        {
            m_device->DestroyPipelineLayout(m_shadowPipelineLayoutSingle);
            m_shadowPipelineLayoutSingle = nullptr;
        }
        if (m_shadowPipelineLayoutInstanced)
        {
            m_device->DestroyPipelineLayout(m_shadowPipelineLayoutInstanced);
            m_shadowPipelineLayoutInstanced = nullptr;
        }
        if (m_viewLayout)
        {
            m_device->DestroyBindGroupLayout(m_viewLayout);
            m_viewLayout = nullptr;
        }
        if (m_objectLayout)
        {
            m_device->DestroyBindGroupLayout(m_objectLayout);
            m_objectLayout = nullptr;
        }
        if (m_instanceLayout)
        {
            m_device->DestroyBindGroupLayout(m_instanceLayout);
            m_instanceLayout = nullptr;
        }
        if (m_clusterLayout)
        {
            m_device->DestroyBindGroupLayout(m_clusterLayout);
            m_clusterLayout = nullptr;
        }
        if (m_shadowViewLayout)
        {
            m_device->DestroyBindGroupLayout(m_shadowViewLayout);
            m_shadowViewLayout = nullptr;
        }
        // rings free their buffers in their destructors (m_device still valid after this).
    }

    Float4x4 MeshRenderer::PrevWorldFor(u64 entityId, const Float4x4& cur)
    {
        const u32 idx = static_cast<u32>(entityId); // entityId = (generation << 32) | index
        if (idx >= m_curWorld.Size())
        {
            m_curWorld.Resize(idx + 1u);
        } // grows toward the max live index, then stable
        m_curWorld[idx] = cur;
        return (idx < m_prevWorld.Size()) ? m_prevWorld[idx] : cur;
    }

    u64 MeshRenderer::InstShareKey(const void* view, const void* mesh, const void* mat) noexcept
    {
        u64 k = 1469598103934665603ull;
        k = (k ^ static_cast<u64>(reinterpret_cast<usize>(view))) * 1099511628211ull;
        k = (k ^ static_cast<u64>(reinterpret_cast<usize>(mesh))) * 1099511628211ull;
        k = (k ^ static_cast<u64>(reinterpret_cast<usize>(mat))) * 1099511628211ull;
        return k;
    }
}
