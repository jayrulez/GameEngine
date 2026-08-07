/// Validation wrapper for Device. Tracks all live resources for leak
/// detection, validates create/destroy parameters.
/// Ported from Sedulous.RHI.Validation/ValidatedDevice.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_device;

import draconic.foundation;
import draconic.rhi;
import :validated_fence;
import :validated_swap_chain;
import :validated_command_pool;
import :validated_queue;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    class ValidatedDevice : public Device
    {
    public:
        explicit ValidatedDevice(Device* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
            type = inner->type;
            features = inner->features;
            shaderGroupHandleSize = inner->shaderGroupHandleSize;
            shaderGroupHandleAlignment = inner->shaderGroupHandleAlignment;
            shaderGroupBaseAlignment = inner->shaderGroupBaseAlignment;
        }

        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            return m_inner->PreferredShaderFormat();
        }

        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override
        {
            return m_inner->NeedsClipSpaceYFlip();
        }

        // ---- Queues ----
        Queue* GetQueue(QueueType t, u32 index) override
        {
            // Wrap on first access.
            Queue* raw = m_inner->GetQueue(t, index);
            if (!raw)
                return nullptr;
            for (auto& w : m_queueWrappers)
                if (w.raw == raw)
                    return w.validated;
            auto* vq = m_allocator.New<ValidatedQueue>(raw, m_allocator);
            m_queueWrappers.PushBack({raw, vq});
            return vq;
        }
        u32 GetQueueCount(QueueType t) override { return m_inner->GetQueueCount(t); }
        FormatSupport GetFormatSupport(TextureFormat f) override
        {
            return m_inner->GetFormatSupport(f);
        }

        // ---- Create methods (with validation + tracking) ----

        Status CreateBuffer(const BufferDesc& d, Buffer*& out) override
        {
            if (!checkAlive("CreateBuffer", out)) return ErrorCode::Unknown;
            if (d.size == 0) { LogError("[Validation] CreateBuffer: size is 0"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.memory == MemoryLocation::CpuToGpu && (static_cast<u32>(d.usage) & static_cast<u32>(BufferUsage::Storage)))
            {
                LogError("[Validation] CreateBuffer: Storage usage is not compatible with CpuToGpu memory. "
                         "DX12 UPLOAD heaps cannot have ALLOW_UNORDERED_ACCESS. "
                         "Use StorageRead for read-only structured buffers, or GpuOnly memory with a staging copy pattern.");
                out = nullptr; return ErrorCode::InvalidArgument;
            }
            if (d.memory == MemoryLocation::GpuToCpu && (static_cast<u32>(d.usage) & static_cast<u32>(BufferUsage::Storage)))
            {
                LogError("[Validation] CreateBuffer: Storage usage is not compatible with GpuToCpu memory. "
                         "DX12 READBACK heaps cannot have ALLOW_UNORDERED_ACCESS.");
                out = nullptr; return ErrorCode::InvalidArgument;
            }
            Status r = m_inner->CreateBuffer(d, out);
            if (r == ErrorCode::Ok && out) m_liveBuffers.PushBack(out);
            return r;
        }

        Status CreateTexture(const TextureDesc& d, Texture*& out) override
        {
            if (!checkAlive("CreateTexture", out)) return ErrorCode::Unknown;
            if (d.width == 0 || d.height == 0) { LogError("[Validation] CreateTexture: width or height is 0"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateTexture(d, out);
            if (r == ErrorCode::Ok && out) m_liveTextures.PushBack(out);
            return r;
        }

        Status CreateSampler(const SamplerDesc& d, Sampler*& out) override
        {
            if (!checkAlive("CreateSampler", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreateSampler(d, out);
            if (r == ErrorCode::Ok && out) m_liveSamplers.PushBack(out);
            return r;
        }

        Status CreateShaderModule(const ShaderModuleDesc& d, ShaderModule*& out) override
        {
            if (!checkAlive("CreateShaderModule", out)) return ErrorCode::Unknown;
            if (d.code.Size() == 0) { LogError("[Validation] CreateShaderModule: code is empty"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateShaderModule(d, out);
            if (r == ErrorCode::Ok && out) m_liveShaderModules.PushBack(out);
            return r;
        }

        Status CreateBindGroupLayout(const BindGroupLayoutDesc& d, BindGroupLayout*& out) override
        {
            if (!checkAlive("CreateBindGroupLayout", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreateBindGroupLayout(d, out);
            if (r == ErrorCode::Ok && out) m_liveBindGroupLayouts.PushBack(out);
            return r;
        }

        Status CreateBindGroup(const BindGroupDesc& d, BindGroup*& out) override
        {
            if (!checkAlive("CreateBindGroup", out)) return ErrorCode::Unknown;
            if (d.layout == nullptr) { LogError("[Validation] CreateBindGroup: layout is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            // Count non-bindless layout entries; positional entries must match.
            {
                auto layoutEntries = d.layout->Entries();
                u32 regularCount = 0;
                for (usize i = 0; i < layoutEntries.Size(); ++i)
                {
                    const auto& le = layoutEntries[i];
                    if (le.type != BindingType::BindlessTextures && le.type != BindingType::BindlessSamplers &&
                        le.type != BindingType::BindlessStorageBuffers && le.type != BindingType::BindlessStorageTextures)
                        ++regularCount;
                }
                if (d.entries.Size() != regularCount)
                {
                    LogErrorf("[Validation] CreateBindGroup: entry count (%u) does not match non-bindless layout entry count (%u)",
                              static_cast<unsigned>(d.entries.Size()), static_cast<unsigned>(regularCount));
                    out = nullptr; return ErrorCode::InvalidArgument;
                }
                // Per-entry resource type validation.
                u32 entryIdx = 0;
                for (usize i = 0; i < layoutEntries.Size() && entryIdx < d.entries.Size(); ++i)
                {
                    const auto& le = layoutEntries[i];
                    if (le.type == BindingType::BindlessTextures || le.type == BindingType::BindlessSamplers ||
                        le.type == BindingType::BindlessStorageBuffers || le.type == BindingType::BindlessStorageTextures)
                        continue;
                    const auto& entry = d.entries[entryIdx];
                    switch (le.type)
                    {
                    case BindingType::UniformBuffer:
                    case BindingType::StorageBufferReadOnly:
                    case BindingType::StorageBufferReadWrite:
                        if (entry.buffer == nullptr)
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] expects a buffer but Buffer is null", entryIdx);
                        break;
                    case BindingType::SampledTexture:
                    case BindingType::StorageTextureReadOnly:
                    case BindingType::StorageTextureReadWrite:
                        if (entry.textureView == nullptr)
                            {
                                const String label8 = String(d.label);
                                LogErrorf("[Validation] CreateBindGroup('%s'): entry [%u] "
                                          "expects a texture view but TextureView is null",
                                          reinterpret_cast<const char*>(label8.CStr()),
                                          entryIdx);
                            }
                        break;
                    case BindingType::Sampler:
                    case BindingType::ComparisonSampler:
                        if (entry.sampler == nullptr)
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] expects a sampler but Sampler is null", entryIdx);
                        break;
                    case BindingType::AccelerationStructure:
                        if (entry.accelStruct == nullptr)
                            LogErrorf("[Validation] CreateBindGroup: entry [%u] expects an acceleration structure but AccelStruct is null", entryIdx);
                        break;
                    default: break;
                    }
                    ++entryIdx;
                }
            }
            Status r = m_inner->CreateBindGroup(d, out);
            if (r == ErrorCode::Ok && out) m_liveBindGroups.PushBack(out);
            return r;
        }

        Status CreatePipelineLayout(const PipelineLayoutDesc& d, PipelineLayout*& out) override
        {
            if (!checkAlive("CreatePipelineLayout", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreatePipelineLayout(d, out);
            if (r == ErrorCode::Ok && out) m_livePipelineLayouts.PushBack(out);
            return r;
        }

        Status CreatePipelineCache(const PipelineCacheDesc& d, PipelineCache*& out) override
        {
            if (!checkAlive("CreatePipelineCache", out)) return ErrorCode::Unknown;
            Status r = m_inner->CreatePipelineCache(d, out);
            if (r == ErrorCode::Ok && out) m_livePipelineCaches.PushBack(out);
            return r;
        }

        Status CreateRenderPipeline(const RenderPipelineDesc& d, RenderPipeline*& out) override
        {
            if (!checkAlive("CreateRenderPipeline", out)) return ErrorCode::Unknown;
            if (d.layout == nullptr) { LogError("[Validation] CreateRenderPipeline: layout is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.vertex.shader.module == nullptr) { LogError("[Validation] CreateRenderPipeline: vertex shader module is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateRenderPipeline(d, out);
            if (r == ErrorCode::Ok && out) m_liveRenderPipelines.PushBack(out);
            return r;
        }

        Status CreateComputePipeline(const ComputePipelineDesc& d, ComputePipeline*& out) override
        {
            if (!checkAlive("CreateComputePipeline", out)) return ErrorCode::Unknown;
            if (d.layout == nullptr) { LogError("[Validation] CreateComputePipeline: layout is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.compute.module == nullptr) { LogError("[Validation] CreateComputePipeline: compute shader module is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateComputePipeline(d, out);
            if (r == ErrorCode::Ok && out) m_liveComputePipelines.PushBack(out);
            return r;
        }

        Status CreateQuerySet(const QuerySetDesc& d, QuerySet*& out) override
        {
            if (!checkAlive("CreateQuerySet", out)) return ErrorCode::Unknown;
            if (d.count == 0) { LogError("[Validation] CreateQuerySet: count is 0"); out = nullptr; return ErrorCode::InvalidArgument; }
            Status r = m_inner->CreateQuerySet(d, out);
            if (r == ErrorCode::Ok && out) m_liveQuerySets.PushBack(out);
            return r;
        }

        Status CreateTextureView(Texture* tex, const TextureViewDesc& d, TextureView*& out) override
        {
            if (!checkAlive("CreateTextureView", out)) return ErrorCode::Unknown;
            if (!tex)
            {
                LogError("[Validation] CreateTextureView: texture is null");
                out = nullptr;
                return ErrorCode::InvalidArgument;
            }
            // Use-after-destroy detection.
            {
                bool tracked = false;
                for (usize i = 0; i < m_liveTextures.Size(); ++i) { if (m_liveTextures[i] == tex) { tracked = true; break; } }
                if (!tracked) LogError("[Validation] CreateTextureView: texture has been destroyed or was not created by this device");
            }
            Status r = m_inner->CreateTextureView(tex, d, out);
            if (r == ErrorCode::Ok && out)
                m_liveTextureViews.PushBack(out);
            return r;
        }

        Status CreateCommandPool(QueueType qt, CommandPool*& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] createCommandPool: device destroyed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            CommandPool* innerPool = nullptr;
            Status r = m_inner->CreateCommandPool(qt, innerPool);
            if (r != ErrorCode::Ok || !innerPool)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedCommandPool>(innerPool, m_allocator);
            m_liveCommandPools.PushBack(out);
            return ErrorCode::Ok;
        }

        Status CreateFence(u64 initialValue, Fence*& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] createFence: device destroyed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Fence* innerFence = nullptr;
            Status r = m_inner->CreateFence(initialValue, innerFence);
            if (r != ErrorCode::Ok || !innerFence)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedFence>(innerFence);
            m_liveFences.PushBack(out);
            return ErrorCode::Ok;
        }

        Status CreateSwapChain(Surface* surface, const SwapChainDesc& d, SwapChain*& out) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] CreateSwapChain: device destroyed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            if (surface == nullptr) { LogError("[Validation] CreateSwapChain: surface is null"); out = nullptr; return ErrorCode::InvalidArgument; }
            if (d.width == 0 || d.height == 0) { LogError("[Validation] CreateSwapChain: width or height is 0"); out = nullptr; return ErrorCode::InvalidArgument; }
            SwapChain* innerSc = nullptr;
            Status r = m_inner->CreateSwapChain(surface, d, innerSc);
            if (r != ErrorCode::Ok || !innerSc)
            {
                out = nullptr;
                return r;
            }
            out = m_allocator.New<ValidatedSwapChain>(innerSc);
            m_liveSwapChains.PushBack(out);
            return ErrorCode::Ok;
        }

        // ---- Mesh/RT (forwarded, validated for destroyed state) ----
        Status CreateMeshPipeline(const MeshPipelineDesc& d, MeshPipeline*& out) override
        {
            if (m_destroyed)
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Status r = m_inner->CreateMeshPipeline(d, out);
            if (r == ErrorCode::Ok && out)
                m_liveMeshPipelines.PushBack(out);
            return r;
        }
        void DestroyMeshPipeline(MeshPipeline*& p) override
        {
            if (!p) return;
            if (!removeFromList(m_liveMeshPipelines, p))
                LogWarning("[Validation] DestroyMeshPipeline: resource was not tracked (double-destroy or wrong device?)");
            m_inner->DestroyMeshPipeline(p);
            p = nullptr;
        }

        Status CreateAccelStruct(const AccelStructDesc& d, AccelStruct*& out) override
        {
            if (m_destroyed)
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Status r = m_inner->CreateAccelStruct(d, out);
            if (r == ErrorCode::Ok && out)
                m_liveAccelStructs.PushBack(out);
            return r;
        }
        void DestroyAccelStruct(AccelStruct*& a) override
        {
            if (!a) return;
            if (!removeFromList(m_liveAccelStructs, a))
                LogWarning("[Validation] DestroyAccelStruct: resource was not tracked (double-destroy or wrong device?)");
            m_inner->DestroyAccelStruct(a);
            a = nullptr;
        }

        Status CreateRayTracingPipeline(const RayTracingPipelineDesc& d,
                                        RayTracingPipeline*& out) override
        {
            if (m_destroyed)
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }
            Status r = m_inner->CreateRayTracingPipeline(d, out);
            if (r == ErrorCode::Ok && out)
                m_liveRtPipelines.PushBack(out);
            return r;
        }
        void DestroyRayTracingPipeline(RayTracingPipeline*& p) override
        {
            if (!p) return;
            if (!removeFromList(m_liveRtPipelines, p))
                LogWarning("[Validation] DestroyRayTracingPipeline: resource was not tracked (double-destroy or wrong device?)");
            m_inner->DestroyRayTracingPipeline(p);
            p = nullptr;
        }

        Status GetShaderGroupHandles(RayTracingPipeline* p, u32 first, u32 count,
                                     Span<u8> out) override
        {
            return m_inner->GetShaderGroupHandles(p, first, count, out);
        }

        // ---- Destroy methods (with tracking removal + not-tracked warning) ----
#define V_DESTROY(Type, method, list)                                                              \
    void method(Type*& x) override                                                                 \
    {                                                                                              \
        if (!x) return;                                                                            \
        if (!removeFromList(list, x))                                                              \
            LogWarning("[Validation] " #method ": resource was not tracked (double-destroy or wrong device?)"); \
        m_inner->method(x);                                                                        \
        x = nullptr;                                                                               \
    }

        V_DESTROY(Buffer, DestroyBuffer, m_liveBuffers)
        V_DESTROY(Texture, DestroyTexture, m_liveTextures)
        V_DESTROY(TextureView, DestroyTextureView, m_liveTextureViews)
        V_DESTROY(Sampler, DestroySampler, m_liveSamplers)
        V_DESTROY(ShaderModule, DestroyShaderModule, m_liveShaderModules)
        V_DESTROY(BindGroupLayout, DestroyBindGroupLayout, m_liveBindGroupLayouts)
        V_DESTROY(BindGroup, DestroyBindGroup, m_liveBindGroups)
        V_DESTROY(PipelineLayout, DestroyPipelineLayout, m_livePipelineLayouts)
        V_DESTROY(PipelineCache, DestroyPipelineCache, m_livePipelineCaches)
        V_DESTROY(RenderPipeline, DestroyRenderPipeline, m_liveRenderPipelines)
        V_DESTROY(ComputePipeline, DestroyComputePipeline, m_liveComputePipelines)
        V_DESTROY(QuerySet, DestroyQuerySet, m_liveQuerySets)
#undef V_DESTROY

        void DestroyCommandPool(CommandPool*& pool) override
        {
            if (!pool)
                return;
            removeFromList(m_liveCommandPools, pool);
            auto* vp = static_cast<ValidatedCommandPool*>(pool);
            if (vp)
            {
                CommandPool* innerPool = vp->inner();
                m_inner->DestroyCommandPool(innerPool);
                m_allocator.Delete(vp);
            }
            else
                m_inner->DestroyCommandPool(pool);
            pool = nullptr;
        }

        void DestroyFence(Fence*& fence) override
        {
            if (!fence)
                return;
            removeFromList(m_liveFences, fence);
            auto* vf = static_cast<ValidatedFence*>(fence);
            if (vf)
            {
                Fence* innerFence = vf->inner();
                m_inner->DestroyFence(innerFence);
                m_allocator.Delete(vf);
            }
            else
                m_inner->DestroyFence(fence);
            fence = nullptr;
        }

        void DestroySwapChain(SwapChain*& sc) override
        {
            if (!sc)
                return;
            removeFromList(m_liveSwapChains, sc);
            auto* vs = static_cast<ValidatedSwapChain*>(sc);
            if (vs)
            {
                SwapChain* innerSc = vs->inner();
                m_inner->DestroySwapChain(innerSc);
                m_allocator.Delete(vs);
            }
            else
                m_inner->DestroySwapChain(sc);
            sc = nullptr;
        }

        void DestroySurface(Surface*& s) override { m_inner->DestroySurface(s); }

        bool IsLost() override { return m_inner->IsLost(); }

        void WaitIdle() override { m_inner->WaitIdle(); }

        void Destroy() override
        {
            if (m_destroyed)
            {
                LogError("[Validation] Device::destroy: already destroyed");
                return;
            }
            m_destroyed = true;
            reportLeaks();
            for (auto& w : m_queueWrappers)
                m_allocator.Delete(w.validated);
            m_queueWrappers.Clear();
            m_inner->Destroy();
            IAllocator& alloc = m_allocator;
            this->~ValidatedDevice();
            alloc.Free(this);
        }

    private:
        template <typename T>
        bool checkAlive(const char* method, T*& out)
        {
            if (m_destroyed)
            {
                LogErrorf("[Validation] %s: device destroyed", method);
                out = nullptr;
                return false;
            }
            return true;
        }

        template <typename T>
        bool removeFromList(Array<T*>& list, T* item)
        {
            for (usize i = 0; i < list.Size(); ++i)
            {
                if (list[i] == item)
                {
                    list.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }

        void reportLeaks()
        {
            auto report = [](const char* name, usize count)
            {
                if (count > 0)
                    LogWarningf("[Validation] Device destroyed with %zu live %s(s)", count, name);
            };
            report("Buffer", m_liveBuffers.Size());
            report("Texture", m_liveTextures.Size());
            report("TextureView", m_liveTextureViews.Size());
            report("Sampler", m_liveSamplers.Size());
            report("ShaderModule", m_liveShaderModules.Size());
            report("BindGroupLayout", m_liveBindGroupLayouts.Size());
            report("BindGroup", m_liveBindGroups.Size());
            report("PipelineLayout", m_livePipelineLayouts.Size());
            report("PipelineCache", m_livePipelineCaches.Size());
            report("RenderPipeline", m_liveRenderPipelines.Size());
            report("ComputePipeline", m_liveComputePipelines.Size());
            report("MeshPipeline", m_liveMeshPipelines.Size());
            report("AccelStruct", m_liveAccelStructs.Size());
            report("RayTracingPipeline", m_liveRtPipelines.Size());
            report("CommandPool", m_liveCommandPools.Size());
            report("Fence", m_liveFences.Size());
            report("SwapChain", m_liveSwapChains.Size());
            report("QuerySet", m_liveQuerySets.Size());
        }

        Device* m_inner;
        IAllocator& m_allocator;
        bool m_destroyed = false;

        struct QueueWrap
        {
            Queue* raw;
            ValidatedQueue* validated;
        };
        Array<QueueWrap> m_queueWrappers;

        Array<Buffer*> m_liveBuffers;
        Array<Texture*> m_liveTextures;
        Array<TextureView*> m_liveTextureViews;
        Array<Sampler*> m_liveSamplers;
        Array<ShaderModule*> m_liveShaderModules;
        Array<BindGroupLayout*> m_liveBindGroupLayouts;
        Array<BindGroup*> m_liveBindGroups;
        Array<PipelineLayout*> m_livePipelineLayouts;
        Array<PipelineCache*> m_livePipelineCaches;
        Array<RenderPipeline*> m_liveRenderPipelines;
        Array<ComputePipeline*> m_liveComputePipelines;
        Array<MeshPipeline*> m_liveMeshPipelines;
        Array<AccelStruct*> m_liveAccelStructs;
        Array<RayTracingPipeline*> m_liveRtPipelines;
        Array<CommandPool*> m_liveCommandPools;
        Array<Fence*> m_liveFences;
        Array<SwapChain*> m_liveSwapChains;
        Array<QuerySet*> m_liveQuerySets;
    };

} // namespace draconic::rhi::validation
