/// DX12 implementation of Device.
/// Creates ID3D12Device, manages descriptor heaps, queues, command signatures,
/// and an internal blit pipeline for texture copy / mipmap generation.
/// Ported from Sedulous.RHI.DX12/DX12Device.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "DxIncludes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

export module draconic.rhi.dx12:device;

import draconic.foundation;
import draconic.rhi;
import :conversions;
import :adapter;
import :surface;
import :descriptor_heap;
import :gpu_descriptor_heap;
import :buffer;
import :texture;
import :texture_view;
import :sampler;
import :shader_module;
import :fence;
import :query_set;
import :bind_group_layout;
import :bind_group;
import :pipeline_layout;
import :pipeline_cache;
import :render_pipeline;
import :compute_pipeline;
import :mesh_pipeline;
import :accel_struct;
import :ray_tracing_pipeline;
import :command_pool;
import :command_encoder;
import :render_bundle_encoder;
import :render_pass_encoder;
import :compute_pass_encoder;
import :queue;
import :swap_chain;

using namespace draconic::foundation;

export namespace draconic::rhi::dx12
{

    class DxDeviceImpl : public Device
    {
    public:
        explicit DxDeviceImpl(IAllocator& allocator) noexcept : m_allocator(allocator) {}

        Status init(DxAdapterImpl* adapter, const DeviceDesc& desc)
        {
            m_adapter = adapter;

            // Create device at feature level 12.0.
            HRESULT hr = D3D12CreateDevice(adapter->handle(), D3D_FEATURE_LEVEL_12_0,
                                           IID_PPV_ARGS(&m_device));
            if (FAILED(hr))
            {
                LogErrorf("DxDevice: D3D12CreateDevice failed (0x%08X)", static_cast<unsigned>(hr));
                return ErrorCode::Unknown;
            }

            // Suppress noisy debug layer warnings.
            {
                ComPtr<ID3D12InfoQueue> infoQueue;
                if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
                {
                    D3D12_MESSAGE_ID suppressIds[] = {
                        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                    };
                    D3D12_INFO_QUEUE_FILTER filter{};
                    filter.DenyList.NumIDs = static_cast<UINT>(std::size(suppressIds));
                    filter.DenyList.pIDList = suppressIds;
                    infoQueue->AddStorageFilterEntries(&filter);
                    m_infoQueue = infoQueue;
                }
            }

            // --- Descriptor heap allocators (CPU-side, for staging) ---
            m_rtvHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256);
            m_dsvHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64);
            m_srvHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
            m_samplerHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);

            // --- GPU-visible descriptor heaps (shader-visible) ---
            // SRV heap sized to the Tier-1 guarantee (1M): per-pool descriptor staging blocks
            // (framesInFlight x jobSlots pools x 1024+) exhausted a 64K heap under multi-view
            // loads - allocate() then fails, SetBindGroup goes stale, and draws sample garbage.
            // The sampler heap is capped at 2048 by D3D12; staging spends it frugally instead.
            m_gpuSrvHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000,
                              true);
            m_gpuSamplerHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, true);

            // --- CPU-visible descriptor heaps (non-shader-visible, bind groups write here) ---
            m_cpuSrvHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, false);
            m_cpuSamplerHeap.init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, false);

            // --- Create queues ---
            u32 graphicsCount = std::max(desc.graphicsQueueCount, 1u);
            for (u32 i = 0; i < graphicsCount; ++i)
            {
                auto* q = m_allocator.New<DxQueueImpl>(m_allocator);
                if (q->init(m_device.Get(), QueueType::Graphics, this) != ErrorCode::Ok)
                {
                    m_allocator.Delete(q);
                    break;
                }
                m_graphicsQueues.PushBack(q);
            }
            for (u32 i = 0; i < desc.computeQueueCount; ++i)
            {
                auto* q = m_allocator.New<DxQueueImpl>(m_allocator);
                if (q->init(m_device.Get(), QueueType::Compute, this) != ErrorCode::Ok)
                {
                    m_allocator.Delete(q);
                    break;
                }
                m_computeQueues.PushBack(q);
            }
            for (u32 i = 0; i < desc.transferQueueCount; ++i)
            {
                auto* q = m_allocator.New<DxQueueImpl>(m_allocator);
                if (q->init(m_device.Get(), QueueType::Transfer, this) != ErrorCode::Ok)
                {
                    m_allocator.Delete(q);
                    break;
                }
                m_transferQueues.PushBack(q);
            }

            // --- Cached command signatures for indirect execution ---
            createIndirectCommandSignatures();

            // --- Internal blit pipeline ---
            createBlitPipeline();

            // --- Detect mesh shader & ray tracing support ---
            detectExtensionSupport();

            // --- Populate features ---
            type = DeviceType::DX12;
            features = adapter->buildFeatures();

            // --- RT handle properties (DX12 constants) ---
            if (m_rtEnabled)
            {
                shaderGroupHandleSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;              // 32
                shaderGroupHandleAlignment = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32
                shaderGroupBaseAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;    // 64
            }

            return ErrorCode::Ok;
        }

        // ==================================================================
        // Device interface -- Queues
        // ==================================================================

        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            return ShaderFormat::DXIL;
        }

        // DX12 is the engine's reference coordinate system (VkRenderPassEncoder flips Y to "match DX12"),
        // so reconstruction passes need no compensation. NB: DX12 uses a POSITIVE viewport, so if the sky/
        // shadows ever render flipped on DX12 (untested on the reconstruction path), this becomes true.
        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override { return false; }

        Queue* GetQueue(QueueType t, u32 index) override
        {
            switch (t)
            {
            case QueueType::Graphics:
                return index < m_graphicsQueues.Size() ? m_graphicsQueues[index] : nullptr;
            case QueueType::Compute:
                return index < m_computeQueues.Size() ? m_computeQueues[index] : nullptr;
            case QueueType::Transfer:
                return index < m_transferQueues.Size() ? m_transferQueues[index] : nullptr;
            }
            return nullptr;
        }

        u32 GetQueueCount(QueueType t) override
        {
            switch (t)
            {
            case QueueType::Graphics:
                return static_cast<u32>(m_graphicsQueues.Size());
            case QueueType::Compute:
                return static_cast<u32>(m_computeQueues.Size());
            case QueueType::Transfer:
                return static_cast<u32>(m_transferQueues.Size());
            }
            return 0;
        }

        FormatSupport GetFormatSupport(TextureFormat /*format*/) override
        {
            // DX12 supports D24_S8 on all hardware and most formats broadly.
            // A full implementation would call CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT).
            return FormatSupport::Texture | FormatSupport::ColorAttachment |
                   FormatSupport::DepthStencil | FormatSupport::Buffer |
                   FormatSupport::VertexBuffer | FormatSupport::BlendableColor |
                   FormatSupport::LinearFilter;
        }

        // ==================================================================
        // Device interface -- Resource creation
        // ==================================================================

        Status CreateBuffer(const BufferDesc& d, Buffer*& out) override
        {
            auto* b = m_allocator.New<DxBufferImpl>();
            if (b->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(b);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(b->handle(), d.label);
            out = b;
            return ErrorCode::Ok;
        }

        Status CreateTexture(const TextureDesc& d, Texture*& out) override
        {
            auto* t = m_allocator.New<DxTextureImpl>();
            if (t->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(t);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(t->handle(), d.label);
            out = t;
            return ErrorCode::Ok;
        }

        Status CreateTextureView(Texture* tex, const TextureViewDesc& d, TextureView*& out) override
        {
            auto* dxTex = static_cast<DxTextureImpl*>(tex);
            if (!dxTex)
            {
                LogError("DxDevice: cast to DxTextureImpl failed");
                out = nullptr;
                return ErrorCode::Unknown;
            }
            auto* v = m_allocator.New<DxTextureViewImpl>();
            if (v->init(m_device.Get(), dxTex, d, &m_srvHeap, &m_rtvHeap, &m_dsvHeap) !=
                ErrorCode::Ok)
            {
                m_allocator.Delete(v);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = v;
            return ErrorCode::Ok;
        }

        Status CreateSampler(const SamplerDesc& d, Sampler*& out) override
        {
            auto* s = m_allocator.New<DxSamplerImpl>();
            if (s->init(m_device.Get(), d, &m_samplerHeap) != ErrorCode::Ok)
            {
                m_allocator.Delete(s);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = s;
            return ErrorCode::Ok;
        }

        Status CreateShaderModule(const ShaderModuleDesc& d, ShaderModule*& out) override
        {
            auto* m = m_allocator.New<DxShaderModuleImpl>();
            if (m->init(d) != ErrorCode::Ok)
            {
                m_allocator.Delete(m);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = m;
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Binding & Pipelines
        // ==================================================================

        Status CreateBindGroupLayout(const BindGroupLayoutDesc& d, BindGroupLayout*& out) override
        {
            auto* l = m_allocator.New<DxBindGroupLayoutImpl>();
            if (l->init(d) != ErrorCode::Ok)
            {
                m_allocator.Delete(l);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = l;
            return ErrorCode::Ok;
        }

        Status CreateBindGroup(const BindGroupDesc& d, BindGroup*& out) override
        {
            auto* g = m_allocator.New<DxBindGroupImpl>();
            if (g->init(m_device.Get(), d, &m_cpuSrvHeap, &m_cpuSamplerHeap, &m_gpuSamplerHeap) !=
                ErrorCode::Ok)
            {
                m_allocator.Delete(g);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = g;
            return ErrorCode::Ok;
        }

        Status CreatePipelineLayout(const PipelineLayoutDesc& d, PipelineLayout*& out) override
        {
            auto* l = m_allocator.New<DxPipelineLayoutImpl>();
            if (l->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                LogError("DxDevice: createPipelineLayout failed");
                m_allocator.Delete(l);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(l->handle(), d.label);
            out = l;
            return ErrorCode::Ok;
        }

        Status CreatePipelineCache(const PipelineCacheDesc& d, PipelineCache*& out) override
        {
            auto* c = m_allocator.New<DxPipelineCacheImpl>();
            if (c->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(c);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            if (c->handle())
                setDebugName(c->handle(), d.label);
            out = c;
            return ErrorCode::Ok;
        }

        Status CreateRenderPipeline(const RenderPipelineDesc& d, RenderPipeline*& out) override
        {
            auto* p = m_allocator.New<DxRenderPipelineImpl>();
            if (p->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(p->handle(), d.label);
            out = p;
            return ErrorCode::Ok;
        }

        Status CreateComputePipeline(const ComputePipelineDesc& d, ComputePipeline*& out) override
        {
            auto* p = m_allocator.New<DxComputePipelineImpl>();
            if (p->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(p->handle(), d.label);
            out = p;
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Mesh shader (folded in)
        // ==================================================================

        Status CreateMeshPipeline(const MeshPipelineDesc& d, MeshPipeline*& out) override
        {
            if (!m_meshEnabled)
            {
                out = nullptr;
                return ErrorCode::NotSupported;
            }
            auto* p = m_allocator.New<DxMeshPipelineImpl>();
            if (p->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(p->handle(), d.label);
            out = p;
            return ErrorCode::Ok;
        }

        void DestroyMeshPipeline(MeshPipeline*& p) override
        {
            if (p)
            {
                static_cast<DxMeshPipelineImpl*>(p)->cleanup();
                m_allocator.Delete(static_cast<DxMeshPipelineImpl*>(p));
                p = nullptr;
            }
        }

        // ==================================================================
        // Ray tracing (folded in)
        // ==================================================================

        Status CreateAccelStruct(const AccelStructDesc& d, AccelStruct*& out) override
        {
            if (!m_rtEnabled)
            {
                out = nullptr;
                return ErrorCode::NotSupported;
            }
            auto* a = m_allocator.New<DxAccelStructImpl>();
            if (a->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(a);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = a;
            return ErrorCode::Ok;
        }

        void DestroyAccelStruct(AccelStruct*& a) override
        {
            if (a)
            {
                static_cast<DxAccelStructImpl*>(a)->cleanup();
                m_allocator.Delete(static_cast<DxAccelStructImpl*>(a));
                a = nullptr;
            }
        }

        Status CreateRayTracingPipeline(const RayTracingPipelineDesc& d,
                                        RayTracingPipeline*& out) override
        {
            if (!m_rtEnabled)
            {
                out = nullptr;
                return ErrorCode::NotSupported;
            }
            auto* p = m_allocator.New<DxRayTracingPipelineImpl>();
            if (p->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = p;
            return ErrorCode::Ok;
        }

        void DestroyRayTracingPipeline(RayTracingPipeline*& p) override
        {
            if (p)
            {
                static_cast<DxRayTracingPipelineImpl*>(p)->cleanup();
                m_allocator.Delete(static_cast<DxRayTracingPipelineImpl*>(p));
                p = nullptr;
            }
        }

        Status GetShaderGroupHandles(RayTracingPipeline* pipeline, u32 firstGroup, u32 groupCount,
                                     Span<u8> outData) override
        {
            if (!m_rtEnabled)
                return ErrorCode::NotSupported;
            auto* dxPipeline = static_cast<DxRayTracingPipelineImpl*>(pipeline);
            if (!dxPipeline || !dxPipeline->properties())
            {
                LogError("DxDevice: pipeline or properties is null");
                return ErrorCode::Unknown;
            }

            constexpr u32 handleSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
            if (outData.Size() < static_cast<usize>(groupCount * handleSize))
            {
                LogError("DxDevice: output buffer too small for shader group handles");
                return ErrorCode::Unknown;
            }

            auto exportNames = dxPipeline->groupExportNames();
            for (u32 i = 0; i < groupCount; ++i)
            {
                u32 groupIdx = firstGroup + i;
                if (groupIdx >= exportNames.Size())
                {
                    LogError("DxDevice: shader group index out of range");
                    return ErrorCode::Unknown;
                }
                const auto& exportName = exportNames[groupIdx];
                void* identifier =
                    dxPipeline->properties()->GetShaderIdentifier(exportName.c_str());
                if (!identifier)
                {
                    LogError("DxDevice: GetShaderIdentifier returned null");
                    return ErrorCode::Unknown;
                }
                std::memcpy(outData.Data() + (i * handleSize), identifier, handleSize);
            }
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Commands
        // ==================================================================

        Status CreateCommandPool(QueueType qt, CommandPool*& out) override
        {
            auto* p = m_allocator.New<DxCommandPoolImpl>();
            if (p->init(this, m_device.Get(), qt, &m_cpuSrvHeap, &m_gpuSrvHeap, &m_cpuSamplerHeap,
                        &m_gpuSamplerHeap, m_allocator) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = p;
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Synchronization
        // ==================================================================

        Status CreateFence(u64 initialValue, Fence*& out) override
        {
            auto* f = m_allocator.New<DxFenceImpl>();
            if (f->init(m_device.Get(), initialValue) != ErrorCode::Ok)
            {
                m_allocator.Delete(f);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = f;
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Queries
        // ==================================================================

        Status CreateQuerySet(const QuerySetDesc& d, QuerySet*& out) override
        {
            auto* q = m_allocator.New<DxQuerySetImpl>();
            if (q->init(m_device.Get(), d) != ErrorCode::Ok)
            {
                m_allocator.Delete(q);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            setDebugName(q->handle(), d.label);
            out = q;
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Presentation
        // ==================================================================

        Status CreateSwapChain(Surface* surface, const SwapChainDesc& d, SwapChain*& out) override
        {
            auto* dxSurface = static_cast<DxSurfaceImpl*>(surface);
            if (!dxSurface)
            {
                LogError("DxDevice: cast to DxSurfaceImpl failed");
                out = nullptr;
                return ErrorCode::Unknown;
            }

            // Need a graphics queue for swap chain.
            if (m_graphicsQueues.IsEmpty())
            {
                out = nullptr;
                return ErrorCode::Unknown;
            }

            auto* sc = m_allocator.New<DxSwapChainImpl>();
            if (sc->init(m_device.Get(), m_adapter->factory(), m_graphicsQueues[0]->handle(),
                         dxSurface, d, &m_srvHeap, &m_rtvHeap, &m_dsvHeap,
                         m_allocator) != ErrorCode::Ok)
            {
                m_allocator.Delete(sc);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = sc;
            return ErrorCode::Ok;
        }

        // ==================================================================
        // Resource destruction
        // ==================================================================

        void DestroyBuffer(Buffer*& b) override
        {
            if (b)
            {
                static_cast<DxBufferImpl*>(b)->cleanup();
                m_allocator.Delete(static_cast<DxBufferImpl*>(b));
                b = nullptr;
            }
        }
        void DestroyTexture(Texture*& t) override
        {
            if (t)
            {
                static_cast<DxTextureImpl*>(t)->cleanup();
                m_allocator.Delete(static_cast<DxTextureImpl*>(t));
                t = nullptr;
            }
        }
        void DestroyTextureView(TextureView*& v) override
        {
            if (v)
            {
                static_cast<DxTextureViewImpl*>(v)->cleanup();
                m_allocator.Delete(static_cast<DxTextureViewImpl*>(v));
                v = nullptr;
            }
        }
        void DestroySampler(Sampler*& s) override
        {
            if (s)
            {
                static_cast<DxSamplerImpl*>(s)->cleanup();
                m_allocator.Delete(static_cast<DxSamplerImpl*>(s));
                s = nullptr;
            }
        }
        void DestroyShaderModule(ShaderModule*& m) override
        {
            if (m)
            {
                static_cast<DxShaderModuleImpl*>(m)->cleanup();
                m_allocator.Delete(static_cast<DxShaderModuleImpl*>(m));
                m = nullptr;
            }
        }
        void DestroyBindGroupLayout(BindGroupLayout*& l) override
        {
            if (l)
            {
                m_allocator.Delete(static_cast<DxBindGroupLayoutImpl*>(l));
                l = nullptr;
            }
        }
        void DestroyBindGroup(BindGroup*& g) override
        {
            if (g)
            {
                static_cast<DxBindGroupImpl*>(g)->cleanup();
                m_allocator.Delete(static_cast<DxBindGroupImpl*>(g));
                g = nullptr;
            }
        }
        void DestroyPipelineLayout(PipelineLayout*& l) override
        {
            if (l)
            {
                static_cast<DxPipelineLayoutImpl*>(l)->cleanup();
                m_allocator.Delete(static_cast<DxPipelineLayoutImpl*>(l));
                l = nullptr;
            }
        }
        void DestroyPipelineCache(PipelineCache*& c) override
        {
            if (c)
            {
                static_cast<DxPipelineCacheImpl*>(c)->cleanup();
                m_allocator.Delete(static_cast<DxPipelineCacheImpl*>(c));
                c = nullptr;
            }
        }
        void DestroyRenderPipeline(RenderPipeline*& p) override
        {
            if (p)
            {
                static_cast<DxRenderPipelineImpl*>(p)->cleanup();
                m_allocator.Delete(static_cast<DxRenderPipelineImpl*>(p));
                p = nullptr;
            }
        }
        void DestroyComputePipeline(ComputePipeline*& p) override
        {
            if (p)
            {
                static_cast<DxComputePipelineImpl*>(p)->cleanup();
                m_allocator.Delete(static_cast<DxComputePipelineImpl*>(p));
                p = nullptr;
            }
        }
        void DestroyCommandPool(CommandPool*& p) override
        {
            if (p)
            {
                static_cast<DxCommandPoolImpl*>(p)->cleanup();
                m_allocator.Delete(static_cast<DxCommandPoolImpl*>(p));
                p = nullptr;
            }
        }
        void DestroyFence(Fence*& f) override
        {
            if (f)
            {
                static_cast<DxFenceImpl*>(f)->cleanup();
                m_allocator.Delete(static_cast<DxFenceImpl*>(f));
                f = nullptr;
            }
        }
        void DestroyQuerySet(QuerySet*& q) override
        {
            if (q)
            {
                static_cast<DxQuerySetImpl*>(q)->cleanup();
                m_allocator.Delete(static_cast<DxQuerySetImpl*>(q));
                q = nullptr;
            }
        }
        void DestroySwapChain(SwapChain*& sc) override
        {
            if (sc)
            {
                static_cast<DxSwapChainImpl*>(sc)->cleanup();
                m_allocator.Delete(static_cast<DxSwapChainImpl*>(sc));
                sc = nullptr;
            }
        }
        void DestroySurface(Surface*& s) override
        {
            if (s)
            {
                m_allocator.Delete(static_cast<DxSurfaceImpl*>(s));
                s = nullptr;
            }
        }

        // ==================================================================
        // Lifecycle
        // ==================================================================

        bool IsLost() override
        {
            // The D3D12 device is the authority: SUCCEEDED means alive, any
            // failure (DXGI_ERROR_DEVICE_HUNG/REMOVED/RESET) means lost.
            return m_device && FAILED(m_device->GetDeviceRemovedReason());
        }

        void WaitIdle() override
        {
            for (auto* q : m_graphicsQueues)
                q->WaitIdle();
            for (auto* q : m_computeQueues)
                q->WaitIdle();
            for (auto* q : m_transferQueues)
                q->WaitIdle();
            drainDebugMessages();
        }

        void drainDebugMessages()
        {
            if (!m_infoQueue)
                return;
            UINT64 count = m_infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < count; ++i)
            {
                SIZE_T len = 0;
                m_infoQueue->GetMessage(i, nullptr, &len);
                if (len == 0)
                    continue;
                auto* msg = static_cast<D3D12_MESSAGE*>(std::malloc(len));
                if (m_infoQueue->GetMessage(i, msg, &len) == S_OK)
                {
                    if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                        std::fprintf(stderr, "[DX12 %s] %.*s\n",
                                     msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR     ? "ERROR"
                                     : msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "WARN"
                                                                                       : "CORRUPT",
                                     static_cast<int>(msg->DescriptionByteLength),
                                     msg->pDescription);
                }
                std::free(msg);
            }
            m_infoQueue->ClearStoredMessages();
        }

        void Destroy() override
        {
            WaitIdle();

            // Queues.
            for (auto* q : m_graphicsQueues)
            {
                q->cleanup();
                m_allocator.Delete(q);
            }
            for (auto* q : m_computeQueues)
            {
                q->cleanup();
                m_allocator.Delete(q);
            }
            for (auto* q : m_transferQueues)
            {
                q->cleanup();
                m_allocator.Delete(q);
            }
            m_graphicsQueues.Clear();
            m_computeQueues.Clear();
            m_transferQueues.Clear();

            // Blit pipeline.
            for (auto& [fmt, pso] : m_blitPsoCache)
                pso.Reset();
            m_blitPsoCache.clear();
            m_blitVsBlob.Reset();
            m_blitPsBlob.Reset();
            m_blitRootSignature.Reset();

            // Command signatures.
            m_drawSignature.Reset();
            m_drawIndexedSignature.Reset();
            m_dispatchSignature.Reset();
            m_dispatchMeshSignature.Reset();

            // Descriptor heaps.
            m_cpuSrvHeap.Destroy();
            m_cpuSamplerHeap.Destroy();
            m_gpuSrvHeap.Destroy();
            m_gpuSamplerHeap.Destroy();
            m_rtvHeap.Destroy();
            m_dsvHeap.Destroy();
            m_srvHeap.Destroy();
            m_samplerHeap.Destroy();

            // Report live objects in debug builds.
#ifdef _DEBUG
            {
                ComPtr<ID3D12DebugDevice> debugDevice;
                if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
                {
                    debugDevice->ReportLiveDeviceObjects(static_cast<D3D12_RLDO_FLAGS>(
                        D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL));
                }
            }
#endif

            m_device.Reset();
            IAllocator& alloc = m_allocator;
            this->~DxDeviceImpl();
            alloc.Free(this);
        }

        // ==================================================================
        // Internal accessors (encoders, swap chain, etc. need these)
        // ==================================================================

        [[nodiscard]] ID3D12Device* handle() const { return m_device.Get(); }
        [[nodiscard]] DxAdapterImpl* adapter() const { return m_adapter; }

        [[nodiscard]] DxDescriptorHeapAllocator* rtvHeap() { return &m_rtvHeap; }
        [[nodiscard]] DxDescriptorHeapAllocator* dsvHeap() { return &m_dsvHeap; }
        [[nodiscard]] DxDescriptorHeapAllocator* srvHeap() { return &m_srvHeap; }
        [[nodiscard]] DxDescriptorHeapAllocator* samplerHeap() { return &m_samplerHeap; }

        [[nodiscard]] DxGpuDescriptorHeap* gpuSrvHeap() { return &m_gpuSrvHeap; }
        [[nodiscard]] DxGpuDescriptorHeap* gpuSamplerHeap() { return &m_gpuSamplerHeap; }
        [[nodiscard]] DxGpuDescriptorHeap* cpuSrvHeap() { return &m_cpuSrvHeap; }
        [[nodiscard]] DxGpuDescriptorHeap* cpuSamplerHeap() { return &m_cpuSamplerHeap; }

        [[nodiscard]] ID3D12CommandSignature* drawSignature() const
        {
            return m_drawSignature.Get();
        }
        [[nodiscard]] ID3D12CommandSignature* drawIndexedSignature() const
        {
            return m_drawIndexedSignature.Get();
        }
        [[nodiscard]] ID3D12CommandSignature* dispatchSignature() const
        {
            return m_dispatchSignature.Get();
        }
        [[nodiscard]] ID3D12CommandSignature* dispatchMeshSignature() const
        {
            return m_dispatchMeshSignature.Get();
        }
        [[nodiscard]] ID3D12RootSignature* blitRootSignature() const
        {
            return m_blitRootSignature.Get();
        }

        [[nodiscard]] bool meshEnabled() const { return m_meshEnabled; }
        [[nodiscard]] bool rtEnabled() const { return m_rtEnabled; }

        /// Gets or creates a blit PSO for the given render target format.
        ID3D12PipelineState* getOrCreateBlitPSO(DXGI_FORMAT format)
        {
            if (!m_blitRootSignature)
                return nullptr;

            std::lock_guard lock(m_blitMutex);

            auto it = m_blitPsoCache.find(format);
            if (it != m_blitPsoCache.end())
                return it->second.Get();

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psd{};
            psd.pRootSignature = m_blitRootSignature.Get();
            psd.VS = m_blitVsBytecode;
            psd.PS = m_blitPsBytecode;
            psd.InputLayout = {nullptr, 0};
            psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            psd.RasterizerState.DepthClipEnable = FALSE;
            psd.BlendState.RenderTarget[0].BlendEnable = FALSE;
            psd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x0F;
            psd.DepthStencilState.DepthEnable = FALSE;
            psd.DepthStencilState.StencilEnable = FALSE;
            psd.DSVFormat = DXGI_FORMAT_UNKNOWN;
            psd.NumRenderTargets = 1;
            psd.RTVFormats[0] = format;
            psd.SampleDesc.Count = 1;
            psd.SampleMask = UINT_MAX;

            ComPtr<ID3D12PipelineState> newPso;
            if (SUCCEEDED(m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&newPso))))
            {
                auto* raw = newPso.Get();
                m_blitPsoCache[format] = std::move(newPso);
                return raw;
            }
            return nullptr;
        }

        /// Sets a debug name on a DX12 object (visible in PIX, VS Graphics Debugger, etc.).
        /// Works with any type that inherits from ID3D12Object (Resource, PSO, QueryHeap, etc.).
        template <typename T>
        static void setDebugName(T* obj, StringView name)
        {
            if (!obj || name.IsEmpty())
                return;
            // Convert narrow to wide.
            std::wstring wide;
            wide.reserve(name.Size());
            for (usize i = 0; i < name.Size(); ++i)
                wide.push_back(static_cast<wchar_t>(name[i]));
            obj->SetName(wide.c_str());
        }

    private:
        // ------------------------------------------------------------------
        // Indirect command signatures
        // ------------------------------------------------------------------

        void createIndirectCommandSignatures()
        {
            D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
            D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
            sigDesc.NumArgumentDescs = 1;
            sigDesc.pArgumentDescs = &argDesc;
            sigDesc.NodeMask = 0;

            // Draw.
            argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            sigDesc.ByteStride = 16; // sizeof(D3D12_DRAW_ARGUMENTS): 4 x uint32
            m_device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&m_drawSignature));

            // DrawIndexed.
            argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            sigDesc.ByteStride = 20; // sizeof(D3D12_DRAW_INDEXED_ARGUMENTS): 5 x uint32
            m_device->CreateCommandSignature(&sigDesc, nullptr,
                                             IID_PPV_ARGS(&m_drawIndexedSignature));

            // Dispatch.
            argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            sigDesc.ByteStride = 12; // sizeof(D3D12_DISPATCH_ARGUMENTS): 3 x uint32
            m_device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&m_dispatchSignature));
        }

        // ------------------------------------------------------------------
        // Extension support detection (mesh shader, ray tracing)
        // ------------------------------------------------------------------

        void detectExtensionSupport()
        {
            // Mesh shaders -- requires D3D12_OPTIONS7.
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
            HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                                       sizeof(options7));
            if (SUCCEEDED(hr) && options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
            {
                m_meshEnabled = true;

                // DispatchMesh command signature.
                D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
                argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
                D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
                sigDesc.ByteStride = 12; // sizeof(D3D12_DISPATCH_MESH_ARGUMENTS): 3 x uint32
                sigDesc.NumArgumentDescs = 1;
                sigDesc.pArgumentDescs = &argDesc;
                sigDesc.NodeMask = 0;
                m_device->CreateCommandSignature(&sigDesc, nullptr,
                                                 IID_PPV_ARGS(&m_dispatchMeshSignature));
            }

            // Ray tracing -- requires D3D12_OPTIONS5.
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
            hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                               sizeof(options5));
            if (SUCCEEDED(hr) && options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
            {
                m_rtEnabled = true;
            }
        }

        // ------------------------------------------------------------------
        // Internal blit pipeline (fullscreen triangle VS + texture sample PS)
        // ------------------------------------------------------------------

        void createBlitPipeline()
        {
            // TODO: Blit pipeline requires D3DCompile from d3dcompiler.lib.
            // Add d3dcompiler to target_link_libraries and uncomment the code below
            // once d3dcompiler linkage is available in this project.
            //
            // The blit pipeline is used for Blit and GenerateMipmaps operations.
            const char vsSource[] = R"(
            struct VSOutput {
                float4 Position : SV_Position;
                float2 UV : TEXCOORD0;
            };
            VSOutput main(uint vertexId : SV_VertexID) {
                VSOutput output;
                output.UV = float2((vertexId << 1) & 2, vertexId & 2);
                output.Position = float4(output.UV * float2(2, -2) + float2(-1, 1), 0, 1);
                return output;
            }
        )";

            const char psSource[] = R"(
            Texture2D srcTexture : register(t0);
            SamplerState srcSampler : register(s0);
            float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                return srcTexture.Sample(srcSampler, uv);
            }
        )";

            ComPtr<ID3DBlob> errorBlob;

            // Compile VS.
            HRESULT hr = D3DCompile(vsSource, sizeof(vsSource) - 1, nullptr, nullptr, nullptr,
                                    "main", "vs_5_0", 0, 0, &m_blitVsBlob, &errorBlob);
            if (FAILED(hr))
            {
                if (errorBlob)
                    LogErrorf("DxDevice: blit VS compile error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
                return;
            }
            errorBlob.Reset();

            // Compile PS.
            hr = D3DCompile(psSource, sizeof(psSource) - 1, nullptr, nullptr, nullptr, "main",
                            "ps_5_0", 0, 0, &m_blitPsBlob, &errorBlob);
            if (FAILED(hr))
            {
                if (errorBlob)
                    LogErrorf("DxDevice: blit PS compile error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
                m_blitVsBlob.Reset();
                return;
            }
            errorBlob.Reset();

            m_blitVsBytecode = {m_blitVsBlob->GetBufferPointer(), m_blitVsBlob->GetBufferSize()};
            m_blitPsBytecode = {m_blitPsBlob->GetBufferPointer(), m_blitPsBlob->GetBufferSize()};

            // Root signature: 1 SRV descriptor table (t0) + 1 static linear sampler (s0).
            D3D12_DESCRIPTOR_RANGE srvRange{};
            srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange.NumDescriptors = 1;
            srvRange.BaseShaderRegister = 0;
            srvRange.RegisterSpace = 0;
            srvRange.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER rootParam{};
            rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rootParam.DescriptorTable.NumDescriptorRanges = 1;
            rootParam.DescriptorTable.pDescriptorRanges = &srvRange;

            D3D12_STATIC_SAMPLER_DESC staticSampler{};
            staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.MaxAnisotropy = 1;
            staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            staticSampler.MinLOD = 0;
            staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
            staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 1;
            rsDesc.pParameters = &rootParam;
            rsDesc.NumStaticSamplers = 1;
            rsDesc.pStaticSamplers = &staticSampler;

            ComPtr<ID3DBlob> signatureBlob;
            hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob,
                                             &errorBlob);
            if (FAILED(hr))
            {
                if (errorBlob)
                    LogErrorf("DxDevice: blit root sig serialize error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
                return;
            }

            m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                          signatureBlob->GetBufferSize(),
                                          IID_PPV_ARGS(&m_blitRootSignature));
        }

        // ------------------------------------------------------------------
        // Member data
        // ------------------------------------------------------------------

        IAllocator& m_allocator;
        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12InfoQueue> m_infoQueue;
        DxAdapterImpl* m_adapter = nullptr;

        // Queues.
        Array<DxQueueImpl*> m_graphicsQueues;
        Array<DxQueueImpl*> m_computeQueues;
        Array<DxQueueImpl*> m_transferQueues;

        // Descriptor heap allocators (CPU-side for staging).
        DxDescriptorHeapAllocator m_rtvHeap;
        DxDescriptorHeapAllocator m_dsvHeap;
        DxDescriptorHeapAllocator m_srvHeap;
        DxDescriptorHeapAllocator m_samplerHeap;

        // GPU-visible descriptor heaps (shader-visible, for command buffer binding).
        DxGpuDescriptorHeap m_gpuSrvHeap;
        DxGpuDescriptorHeap m_gpuSamplerHeap;

        // CPU-visible descriptor heaps (non-shader-visible, bind groups write here).
        DxGpuDescriptorHeap m_cpuSrvHeap;
        DxGpuDescriptorHeap m_cpuSamplerHeap;

        // Cached command signatures for indirect execution.
        ComPtr<ID3D12CommandSignature> m_drawSignature;
        ComPtr<ID3D12CommandSignature> m_drawIndexedSignature;
        ComPtr<ID3D12CommandSignature> m_dispatchSignature;
        ComPtr<ID3D12CommandSignature> m_dispatchMeshSignature;

        // Internal blit pipeline.
        ComPtr<ID3D12RootSignature> m_blitRootSignature;
        D3D12_SHADER_BYTECODE m_blitVsBytecode{};
        D3D12_SHADER_BYTECODE m_blitPsBytecode{};
        ComPtr<ID3DBlob> m_blitVsBlob;
        ComPtr<ID3DBlob> m_blitPsBlob;
        std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> m_blitPsoCache;
        std::mutex m_blitMutex;

        // Extension flags.
        bool m_meshEnabled = false;
        bool m_rtEnabled = false;
    };

    // ==================================================================
    // Adapter::CreateDevice implementation
    // ==================================================================

    Status DxAdapterImpl::CreateDevice(const DeviceDesc& desc, Device*& out)
    {
        auto* dev = m_allocator.New<DxDeviceImpl>(m_allocator);
        if (dev->init(this, desc) != ErrorCode::Ok)
        {
            m_allocator.Delete(dev);
            out = nullptr;
            return ErrorCode::Unknown;
        }
        out = dev;
        return ErrorCode::Ok;
    }

    // ---- CommandEncoder out-of-line: blitSubresource (needs DxDeviceImpl) ----

    void DxCommandEncoderImpl::blitSubresource(DxTextureImpl* srcTex, u32 srcMip,
                                               DxTextureImpl* dstTex, u32 dstMip, u32 dstWidth,
                                               u32 dstHeight, DXGI_FORMAT dxgiFormat)
    {

        auto* blitRootSig = m_device->blitRootSignature();
        if (!blitRootSig)
            return;
        auto* blitPso = m_device->getOrCreateBlitPSO(dxgiFormat);
        if (!blitPso)
            return;

        // Allocate temp RTV for destination mip.
        auto rtvHandle = m_device->rtvHeap()->allocate();

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = dxgiFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = dstMip;
        m_device->handle()->CreateRenderTargetView(dstTex->handle(), &rtvDesc, rtvHandle);

        // Allocate temp SRV in CPU heap, write, then stage-copy to GPU heap.
        i32 tempSrvOff = m_device->cpuSrvHeap()->allocate(1);
        if (tempSrvOff < 0)
        {
            m_device->rtvHeap()->free(rtvHandle);
            return;
        }

        auto tempCpuHandle = m_device->cpuSrvHeap()->getCpuHandle(static_cast<u32>(tempSrvOff));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = srcMip;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->handle()->CreateShaderResourceView(srcTex->handle(), &srvDesc, tempCpuHandle);

        // Copy from CPU heap into GPU staging, then free CPU temp slot.
        i32 stagedOff = m_pool->srvStaging()->copyFrom(static_cast<u32>(tempSrvOff), 1);
        m_device->cpuSrvHeap()->free(static_cast<u32>(tempSrvOff), 1);
        if (stagedOff < 0)
        {
            m_device->rtvHeap()->free(rtvHandle);
            return;
        }

        auto srvGpuHandle = m_device->gpuSrvHeap()->getGpuHandle(static_cast<u32>(stagedOff));

        ensureDescriptorHeaps();

        // Set blit pipeline.
        m_cmdList->SetGraphicsRootSignature(blitRootSig);
        m_cmdList->SetPipelineState(blitPso);
        m_cmdList->SetGraphicsRootDescriptorTable(0, srvGpuHandle);
        m_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT vp{};
        vp.Width = static_cast<FLOAT>(dstWidth);
        vp.Height = static_cast<FLOAT>(dstHeight);
        vp.MaxDepth = 1.0f;
        m_cmdList->RSSetViewports(1, &vp);
        D3D12_RECT sc{};
        sc.right = static_cast<LONG>(dstWidth);
        sc.bottom = static_cast<LONG>(dstHeight);
        m_cmdList->RSSetScissorRects(1, &sc);

        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmdList->DrawInstanced(3, 1, 0, 0);

        m_device->rtvHeap()->free(rtvHandle);
    }

    // ---- CommandPool out-of-line methods (need DxCommandEncoderImpl + context structs) ----

    Status DxCommandPoolImpl::CreateEncoder(CommandEncoder*& out)
    {
        out = nullptr;

        ComPtr<ID3D12Device> d3dDev;
        m_allocator->GetDevice(IID_PPV_ARGS(&d3dDev));
        if (!d3dDev)
            return ErrorCode::Unknown;

        ID3D12GraphicsCommandList* cmdList = nullptr;
        HRESULT hr = d3dDev->CreateCommandList(0, m_type, m_allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&cmdList));
        if (FAILED(hr))
            return ErrorCode::Unknown;

        DxRenderPassContext rpeCtx{};
        rpeCtx.cmdList = cmdList;
        rpeCtx.srvStaging = &m_srvStaging;
        rpeCtx.gpuSrvHeap = m_device->gpuSrvHeap();
        rpeCtx.gpuSamplerHeap = m_device->gpuSamplerHeap();
        rpeCtx.drawSig = m_device->drawSignature();
        rpeCtx.drawIndexedSig = m_device->drawIndexedSignature();
        rpeCtx.dispatchMeshSig = m_device->dispatchMeshSignature();

        DxComputePassContext cpeCtx{};
        cpeCtx.cmdList = cmdList;
        cpeCtx.srvStaging = &m_srvStaging;
        cpeCtx.gpuSrvHeap = m_device->gpuSrvHeap();
        cpeCtx.gpuSamplerHeap = m_device->gpuSamplerHeap();
        cpeCtx.dispatchSig = m_device->dispatchSignature();

        auto* enc = m_allocPtr->New<DxCommandEncoderImpl>(m_device, cmdList, this, rpeCtx, cpeCtx,
                                                          *m_allocPtr);
        ++m_unfinishedEncoders; // balanced by Finish() closing the list
        out = enc;
        return ErrorCode::Ok;
    }

    void DxCommandPoolImpl::DestroyEncoder(CommandEncoder*& encoder)
    {
        if (encoder)
        {
            m_allocPtr->Delete(static_cast<DxCommandEncoderImpl*>(encoder));
            encoder = nullptr;
        }
    }

    RenderBundleEncoder* DxCommandPoolImpl::CreateRenderBundleEncoder(const RenderBundleDesc& desc)
    {
        (void)desc; // DX12 bundles carry no creation-time attachment state

        // A bundle records into its own BUNDLE-type allocator + list; the pool's
        // primary allocator is untouched, so bundles need no open command encoder.
        ComPtr<ID3D12CommandAllocator> alloc;
        if (FAILED(m_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE,
                                                       IID_PPV_ARGS(&alloc))))
            return nullptr;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(m_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, alloc.Get(),
                                                  nullptr, IID_PPV_ARGS(&list))))
            return nullptr;

        // The bundle's descriptor heaps must match the executing command list's at
        // ExecuteBundle time; both use the device's two shader-visible heaps.
        ID3D12DescriptorHeap* heaps[2] = {m_device->gpuSrvHeap()->heap(),
                                          m_device->gpuSamplerHeap()->heap()};
        list->SetDescriptorHeaps(2, heaps);

        DxRenderPassContext ctx{};
        ctx.cmdList = list.Get();
        ctx.srvStaging = &m_srvStaging;
        ctx.gpuSrvHeap = m_device->gpuSrvHeap();
        ctx.gpuSamplerHeap = m_device->gpuSamplerHeap();
        ctx.drawSig = m_device->drawSignature();
        ctx.drawIndexedSig = m_device->drawIndexedSignature();
        ctx.dispatchMeshSig = m_device->dispatchMeshSignature();

        auto* enc = m_allocPtr->New<DxRenderBundleEncoderImpl>(ctx, list, alloc, *m_allocPtr);
        m_trackedBundleEncoders.PushBack(enc); // pool-owned: freed on Reset (fence-guarded)
        return enc;
    }

    void DxCommandPoolImpl::releaseBundleEncoders()
    {
        for (auto* e : m_trackedBundleEncoders)
            m_allocPtr->Delete(static_cast<DxRenderBundleEncoderImpl*>(e));
        m_trackedBundleEncoders.Clear();
    }

} // namespace draconic::rhi::dx12
