/// Vulkan implementation of Device.
/// Ported from Sedulous.RHI.Vulkan/VulkanDevice.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

#include <cstdio>
#include <cstring>

export module draconic.rhi.vulkan:device;

import draconic.foundation;
import draconic.rhi;
import :adapter;
import :surface;
import :buffer;
import :texture;
import :texture_view;
import :sampler;
import :shader_module;
import :fence;
import :query_set;
import :binding_shifts;
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
import :command_buffer;
import :queue;
import :swap_chain;
import :descriptor_pool_manager;
import :conversions;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkDeviceImpl : public Device
    {
    public:
        explicit VkDeviceImpl(IAllocator& allocator) noexcept : m_allocator(allocator) {}

        Status init(VkAdapterImpl* adapter, const DeviceDesc& desc)
        {
            m_adapter = adapter;
            features = adapter->buildFeatures();

            bool bindlessEnabled =
                desc.requiredFeatures.bindlessDescriptors && adapter->supportsDescriptorIndexing();
            bool meshEnabled = desc.requiredFeatures.meshShaders && adapter->supportsMeshShader();
            bool rtEnabled = desc.requiredFeatures.rayTracing && adapter->supportsRayTracing();
            m_meshEnabled = meshEnabled;
            m_rtEnabled = rtEnabled;
            m_bindlessEnabled = bindlessEnabled;
            // Validation flag: set externally by Adapter.createDevice after init.

            // Find queue families.
            i32 gfxFamily = adapter->findQueueFamily(QueueType::Graphics);
            i32 compFamily = adapter->findQueueFamily(QueueType::Compute);
            i32 xferFamily = adapter->findQueueFamily(QueueType::Transfer);
            if (gfxFamily < 0)
                return ErrorCode::Unknown;

            // Build queue create infos.
            struct FamilyRequest
            {
                u32 family;
                u32 count;
            };
            Array<FamilyRequest> familyReqs;
            auto addFamily = [&](i32 f, u32 requested)
            {
                if (f < 0 || requested == 0)
                    return;
                u32 avail = adapter->queueFamilies()[f].queueCount;
                for (auto& fr : familyReqs)
                {
                    if (fr.family == static_cast<u32>(f))
                    {
                        fr.count = Min(fr.count + requested, avail);
                        return;
                    }
                }
                familyReqs.PushBack({static_cast<u32>(f), Min(requested, avail)});
            };
            addFamily(gfxFamily, desc.graphicsQueueCount);
            addFamily(compFamily, desc.computeQueueCount);
            addFamily(xferFamily, desc.transferQueueCount);

            Array<VkDeviceQueueCreateInfo> queueCis;
            Array<Array<f32>> priorities;
            for (auto& fr : familyReqs)
            {
                priorities.EmplaceBack(fr.count, 1.0f);
                VkDeviceQueueCreateInfo qci{};
                qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                qci.queueFamilyIndex = fr.family;
                qci.queueCount = fr.count;
                qci.pQueuePriorities = priorities.Back().Data();
                queueCis.PushBack(qci);
            }

            // Extensions.
            Array<const char*> exts;
            exts.PushBack(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            if (meshEnabled)
                exts.PushBack("VK_EXT_mesh_shader");
            if (rtEnabled)
            {
                exts.PushBack("VK_KHR_ray_tracing_pipeline");
                exts.PushBack("VK_KHR_acceleration_structure");
                exts.PushBack("VK_KHR_deferred_host_operations");
                exts.PushBack("VK_KHR_ray_query");
            }

            // Feature chain.
            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            features13.dynamicRendering = VK_TRUE;
            features13.synchronization2 = VK_TRUE;
            features13.shaderDemoteToHelperInvocation = VK_TRUE;

            VkPhysicalDeviceVulkan12Features features12{};
            features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            features12.pNext = &features13;
            features12.timelineSemaphore = VK_TRUE;
            if (bindlessEnabled)
            {
                features12.descriptorIndexing = VK_TRUE;
                features12.descriptorBindingPartiallyBound = VK_TRUE;
                features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
                features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
                features12.runtimeDescriptorArray = VK_TRUE;
                features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
                features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            }
            if (rtEnabled)
                features12.bufferDeviceAddress = VK_TRUE;

            void* featureChainTail = &features12;

            VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{};
            meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
            if (meshEnabled)
            {
                meshFeatures.taskShader = VK_TRUE;
                meshFeatures.meshShader = VK_TRUE;
                meshFeatures.pNext = featureChainTail;
                featureChainTail = &meshFeatures;
            }

            VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeFeatures{};
            rtPipeFeatures.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
            asFeatures.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{};
            rqFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            if (rtEnabled)
            {
                rtPipeFeatures.rayTracingPipeline = VK_TRUE;
                asFeatures.accelerationStructure = VK_TRUE;
                rqFeatures.rayQuery = VK_TRUE;
                rtPipeFeatures.pNext = featureChainTail;
                featureChainTail = &rtPipeFeatures;
                asFeatures.pNext = featureChainTail;
                featureChainTail = &asFeatures;
                rqFeatures.pNext = featureChainTail;
                featureChainTail = &rqFeatures;
            }

            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = featureChainTail;
            features2.features = adapter->features10();

            VkDeviceCreateInfo dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            dci.pNext = &features2;
            dci.queueCreateInfoCount = static_cast<u32>(queueCis.Size());
            dci.pQueueCreateInfos = queueCis.Data();
            dci.enabledExtensionCount = static_cast<u32>(exts.Size());
            dci.ppEnabledExtensionNames = exts.Data();

            if (vkCreateDevice(adapter->physicalDevice(), &dci, nullptr, &m_device) != VK_SUCCESS)
                return ErrorCode::Unknown;

            type = DeviceType::Vulkan;

            // Retrieve queues.
            auto retrieveQueues = [&](i32 family, u32 count, QueueType qt, u32 offset)
            {
                if (family < 0 || count == 0)
                    return;
                f32 tsPeriod = adapter->properties().limits.timestampPeriod;
                for (u32 i = 0; i < count; ++i)
                {
                    VkQueue q = VK_NULL_HANDLE;
                    vkGetDeviceQueue(m_device, static_cast<u32>(family), offset + i, &q);
                    auto* vkQ = m_allocator.New<VkQueueImpl>(
                        q, qt, static_cast<u32>(family), tsPeriod, this, m_device,
                        adapter->physicalDevice(), m_allocator);
                    m_allQueues.PushBack(vkQ);
                    switch (qt)
                    {
                    case QueueType::Graphics:
                        m_gfxQueues.PushBack(vkQ);
                        break;
                    case QueueType::Compute:
                        m_compQueues.PushBack(vkQ);
                        break;
                    case QueueType::Transfer:
                        m_xferQueues.PushBack(vkQ);
                        break;
                    }
                }
            };
            // Track how many queues have been claimed from each family to avoid overrun.
            auto familyAvail = [&](i32 family) -> u32
            {
                u32 total = adapter->queueFamilies()[family].queueCount;
                for (auto& fr : familyReqs)
                    if (fr.family == static_cast<u32>(family))
                        return fr.count;
                return total;
            };

            u32 gfxCount = Min(desc.graphicsQueueCount, familyAvail(gfxFamily));
            retrieveQueues(gfxFamily, gfxCount, QueueType::Graphics, 0);

            if (compFamily >= 0)
            {
                u32 offset = (compFamily == gfxFamily) ? gfxCount : 0;
                u32 avail = adapter->queueFamilies()[compFamily].queueCount - offset;
                u32 compCount = Min(desc.computeQueueCount, avail);
                retrieveQueues(compFamily, compCount, QueueType::Compute, offset);
            }
            if (xferFamily >= 0)
            {
                u32 offset = 0;
                if (xferFamily == gfxFamily)
                    offset = gfxCount + static_cast<u32>(m_compQueues.Size());
                else if (xferFamily == compFamily)
                    offset = static_cast<u32>(m_compQueues.Size());
                u32 avail = adapter->queueFamilies()[xferFamily].queueCount - offset;
                u32 xferCount = Min(desc.transferQueueCount, avail);
                retrieveQueues(xferFamily, xferCount, QueueType::Transfer, offset);
            }

            // Descriptor pool manager.
            m_poolManager = m_allocator.New<VkDescriptorPoolManager>(m_device, 256, rtEnabled);

            // Probe depth-stencil format support. D24_S8 is optional (unsupported on AMD/RADV).
            // Configures toVkFormat() to substitute D32F/D32F_S8 when D24 variants aren't supported.
            {
                VkFormatProperties fp{};
                vkGetPhysicalDeviceFormatProperties(adapter->physicalDevice(),
                                                    VK_FORMAT_D24_UNORM_S8_UINT, &fp);
                bool d24s8 = (fp.optimalTilingFeatures &
                              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
                vkGetPhysicalDeviceFormatProperties(adapter->physicalDevice(),
                                                    VK_FORMAT_X8_D24_UNORM_PACK32, &fp);
                bool d24 = (fp.optimalTilingFeatures &
                            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
                setDepthFormatSupport(d24s8, d24);
            }

            // RT properties.
            if (rtEnabled)
            {
                VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
                rtProps.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
                VkPhysicalDeviceProperties2 p2{};
                p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                p2.pNext = &rtProps;
                vkGetPhysicalDeviceProperties2(adapter->physicalDevice(), &p2);
                shaderGroupHandleSize = rtProps.shaderGroupHandleSize;
                shaderGroupHandleAlignment = rtProps.shaderGroupHandleAlignment;
                shaderGroupBaseAlignment = rtProps.shaderGroupBaseAlignment;
            }

            return ErrorCode::Ok;
        }

        // ---- Device interface ----

        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            return ShaderFormat::SpirV;
        }

        // Vulkan flips clip-space Y via a negative-height viewport (VkRenderPassEncoder::SetViewport),
        // so screen-space reconstruction is already consistent - no compensation needed.
        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override { return false; }

        Queue* GetQueue(QueueType t, u32 index) override
        {
            switch (t)
            {
            case QueueType::Graphics:
                return index < m_gfxQueues.Size() ? m_gfxQueues[index] : nullptr;
            case QueueType::Compute:
                return index < m_compQueues.Size() ? m_compQueues[index] : nullptr;
            case QueueType::Transfer:
                return index < m_xferQueues.Size() ? m_xferQueues[index] : nullptr;
            }
            return nullptr;
        }
        u32 GetQueueCount(QueueType t) override
        {
            switch (t)
            {
            case QueueType::Graphics:
                return static_cast<u32>(m_gfxQueues.Size());
            case QueueType::Compute:
                return static_cast<u32>(m_compQueues.Size());
            case QueueType::Transfer:
                return static_cast<u32>(m_xferQueues.Size());
            }
            return 0;
        }

        FormatSupport GetFormatSupport(TextureFormat format) override
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(m_adapter->physicalDevice(), toVkFormat(format),
                                                &fp);
            auto opt = fp.optimalTilingFeatures;
            auto buf = fp.bufferFeatures;
            FormatSupport s = FormatSupport::Unsupported;
            if (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                s = s | FormatSupport::Texture;
            if (opt & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                s = s | FormatSupport::StorageTexture;
            if (opt & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
                s = s | FormatSupport::ColorAttachment;
            if (opt & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                s = s | FormatSupport::DepthStencil;
            if (opt & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
                s = s | FormatSupport::BlendableColor;
            if (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                s = s | FormatSupport::LinearFilter;
            if (buf & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)
                s = s | FormatSupport::Buffer;
            if (buf & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT)
                s = s | FormatSupport::StorageBuffer;
            if (buf & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
                s = s | FormatSupport::VertexBuffer;
            return s;
        }

        // ---- Resource creation ----
        Status CreateBuffer(const BufferDesc& d, Buffer*& out) override
        {
            auto* b = m_allocator.New<VkBufferImpl>();
            if (b->init(m_device, m_adapter, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(b);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = b;
            return ErrorCode::Ok;
        }
        Status CreateTexture(const TextureDesc& d, Texture*& out) override
        {
            auto* t = m_allocator.New<VkTextureImpl>();
            if (t->init(m_device, m_adapter, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(t);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = t;
            return ErrorCode::Ok;
        }
        Status CreateTextureView(Texture* tex, const TextureViewDesc& d, TextureView*& out) override
        {
            auto* v = m_allocator.New<VkTextureViewImpl>();
            if (v->init(m_device, static_cast<VkTextureImpl*>(tex), d) != ErrorCode::Ok)
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
            auto* s = m_allocator.New<VkSamplerImpl>();
            if (s->init(m_device, d) != ErrorCode::Ok)
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
            auto* m = m_allocator.New<VkShaderModuleImpl>();
            if (m->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(m);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = m;
            return ErrorCode::Ok;
        }
        Status CreateBindGroupLayout(const BindGroupLayoutDesc& d, BindGroupLayout*& out) override
        {
            auto* l = m_allocator.New<VkBindGroupLayoutImpl>();
            if (l->init(m_device, d, m_bindingShifts) != ErrorCode::Ok)
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
            auto* g = m_allocator.New<VkBindGroupImpl>();
            if (g->init(m_device, m_poolManager, d, m_bindingShifts) != ErrorCode::Ok)
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
            auto* l = m_allocator.New<VkPipelineLayoutImpl>();
            if (l->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(l);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = l;
            return ErrorCode::Ok;
        }
        Status CreatePipelineCache(const PipelineCacheDesc& d, PipelineCache*& out) override
        {
            auto* c = m_allocator.New<VkPipelineCacheImpl>();
            if (c->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(c);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = c;
            return ErrorCode::Ok;
        }
        Status CreateRenderPipeline(const RenderPipelineDesc& d, RenderPipeline*& out) override
        {
            auto* p = m_allocator.New<VkRenderPipelineImpl>();
            if (p->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = p;
            return ErrorCode::Ok;
        }
        Status CreateComputePipeline(const ComputePipelineDesc& d, ComputePipeline*& out) override
        {
            auto* p = m_allocator.New<VkComputePipelineImpl>();
            if (p->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = p;
            return ErrorCode::Ok;
        }
        Status CreateCommandPool(QueueType qt, CommandPool*& out) override
        {
            auto* p = m_allocator.New<VkCommandPoolImpl>();
            p->ownerDevice = this;
            if (p->init(m_device, m_adapter, qt, m_allocator) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = p;
            return ErrorCode::Ok;
        }
        Status CreateFence(u64 initialValue, Fence*& out) override
        {
            auto* f = m_allocator.New<VkFenceImpl>();
            if (f->init(m_device, initialValue) != ErrorCode::Ok)
            {
                m_allocator.Delete(f);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = f;
            return ErrorCode::Ok;
        }
        Status CreateQuerySet(const QuerySetDesc& d, QuerySet*& out) override
        {
            auto* q = m_allocator.New<VkQuerySetImpl>();
            if (q->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(q);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = q;
            return ErrorCode::Ok;
        }
        Status CreateSwapChain(Surface* surface, const SwapChainDesc& d, SwapChain*& out) override
        {
            auto* sc = m_allocator.New<VkSwapChainImpl>();
            auto* vkSurf = static_cast<VkSurfaceImpl*>(surface);
            if (sc->init(m_device, m_adapter->physicalDevice(), vkSurf->handle(), d, this,
                         m_allocator) != ErrorCode::Ok)
            {
                m_allocator.Delete(sc);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = sc;
            return ErrorCode::Ok;
        }

        // ---- Mesh shader (folded in) ----
        Status CreateMeshPipeline(const MeshPipelineDesc& d, MeshPipeline*& out) override
        {
            if (!m_meshEnabled)
            {
                out = nullptr;
                return ErrorCode::NotSupported;
            }
            auto* p = m_allocator.New<VkMeshPipelineImpl>();
            if (p->init(m_device, d) != ErrorCode::Ok)
            {
                m_allocator.Delete(p);
                out = nullptr;
                return ErrorCode::Unknown;
            }
            out = p;
            return ErrorCode::Ok;
        }
        void DestroyMeshPipeline(MeshPipeline*& p) override
        {
            if (p)
            {
                static_cast<VkMeshPipelineImpl*>(p)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkMeshPipelineImpl*>(p));
                p = nullptr;
            }
        }

        // ---- Ray tracing (folded in) ----
        Status CreateAccelStruct(const AccelStructDesc& d, AccelStruct*& out) override
        {
            if (!m_rtEnabled)
            {
                out = nullptr;
                return ErrorCode::NotSupported;
            }
            auto* a = m_allocator.New<VkAccelStructImpl>();
            if (a->init(m_device, m_adapter, d, 256 * 1024) != ErrorCode::Ok)
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
                static_cast<VkAccelStructImpl*>(a)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkAccelStructImpl*>(a));
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
            auto* p = m_allocator.New<VkRayTracingPipelineImpl>();
            if (p->init(m_device, d) != ErrorCode::Ok)
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
                static_cast<VkRayTracingPipelineImpl*>(p)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkRayTracingPipelineImpl*>(p));
                p = nullptr;
            }
        }
        Status GetShaderGroupHandles(RayTracingPipeline* pipeline, u32 firstGroup, u32 groupCount,
                                     Span<u8> outData) override
        {
            if (!m_rtEnabled)
                return ErrorCode::NotSupported;
            auto* p = static_cast<VkRayTracingPipelineImpl*>(pipeline);
            auto pfn = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR"));
            if (!pfn)
                return ErrorCode::Unknown;
            return pfn(m_device, p->handle(), firstGroup, groupCount, outData.Size(),
                       outData.Data()) == VK_SUCCESS
                       ? ErrorCode::Ok
                       : ErrorCode::Unknown;
        }

        // ---- Resource destruction ----
        void DestroyBuffer(Buffer*& b) override
        {
            if (b)
            {
                static_cast<VkBufferImpl*>(b)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkBufferImpl*>(b));
                b = nullptr;
            }
        }
        void DestroyTexture(Texture*& t) override
        {
            if (t)
            {
                static_cast<VkTextureImpl*>(t)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkTextureImpl*>(t));
                t = nullptr;
            }
        }
        void DestroyTextureView(TextureView*& v) override
        {
            if (v)
            {
                static_cast<VkTextureViewImpl*>(v)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkTextureViewImpl*>(v));
                v = nullptr;
            }
        }
        void DestroySampler(Sampler*& s) override
        {
            if (s)
            {
                static_cast<VkSamplerImpl*>(s)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkSamplerImpl*>(s));
                s = nullptr;
            }
        }
        void DestroyShaderModule(ShaderModule*& m) override
        {
            if (m)
            {
                static_cast<VkShaderModuleImpl*>(m)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkShaderModuleImpl*>(m));
                m = nullptr;
            }
        }
        void DestroyBindGroupLayout(BindGroupLayout*& l) override
        {
            if (l)
            {
                static_cast<VkBindGroupLayoutImpl*>(l)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkBindGroupLayoutImpl*>(l));
                l = nullptr;
            }
        }
        void DestroyBindGroup(BindGroup*& g) override
        {
            if (g)
            {
                static_cast<VkBindGroupImpl*>(g)->cleanup(m_device, m_poolManager);
                m_allocator.Delete(static_cast<VkBindGroupImpl*>(g));
                g = nullptr;
            }
        }
        void DestroyPipelineLayout(PipelineLayout*& l) override
        {
            if (l)
            {
                static_cast<VkPipelineLayoutImpl*>(l)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkPipelineLayoutImpl*>(l));
                l = nullptr;
            }
        }
        void DestroyPipelineCache(PipelineCache*& c) override
        {
            if (c)
            {
                static_cast<VkPipelineCacheImpl*>(c)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkPipelineCacheImpl*>(c));
                c = nullptr;
            }
        }
        void DestroyRenderPipeline(RenderPipeline*& p) override
        {
            if (p)
            {
                static_cast<VkRenderPipelineImpl*>(p)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkRenderPipelineImpl*>(p));
                p = nullptr;
            }
        }
        void DestroyComputePipeline(ComputePipeline*& p) override
        {
            if (p)
            {
                static_cast<VkComputePipelineImpl*>(p)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkComputePipelineImpl*>(p));
                p = nullptr;
            }
        }
        void DestroyCommandPool(CommandPool*& p) override
        {
            if (p)
            {
                static_cast<VkCommandPoolImpl*>(p)->cleanup();
                m_allocator.Delete(static_cast<VkCommandPoolImpl*>(p));
                p = nullptr;
            }
        }
        void DestroyFence(Fence*& f) override
        {
            if (f)
            {
                static_cast<VkFenceImpl*>(f)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkFenceImpl*>(f));
                f = nullptr;
            }
        }
        void DestroyQuerySet(QuerySet*& q) override
        {
            if (q)
            {
                static_cast<VkQuerySetImpl*>(q)->cleanup(m_device);
                m_allocator.Delete(static_cast<VkQuerySetImpl*>(q));
                q = nullptr;
            }
        }
        void DestroySwapChain(SwapChain*& sc) override
        {
            if (sc)
            {
                static_cast<VkSwapChainImpl*>(sc)->cleanup();
                m_allocator.Delete(static_cast<VkSwapChainImpl*>(sc));
                sc = nullptr;
            }
        }
        void DestroySurface(Surface*& s) override
        {
            if (s)
            {
                static_cast<VkSurfaceImpl*>(s)->Destroy();
                m_allocator.Delete(static_cast<VkSurfaceImpl*>(s));
                s = nullptr;
            }
        }

        bool IsLost() override { return m_lost; }

        /// Latch device loss. Vulkan has no polling query (unlike DX12's
        /// GetDeviceRemovedReason), so queue submit/present/wait sites report
        /// VK_ERROR_DEVICE_LOST here. Sticky by contract.
        void markLost() { m_lost = true; }

        void WaitIdle() override
        {
            if (vkDeviceWaitIdle(m_device) == VK_ERROR_DEVICE_LOST)
                m_lost = true;
        }
        void Destroy() override
        {
            WaitIdle();
            if (m_poolManager)
            {
                m_poolManager->Destroy();
                m_allocator.Delete(m_poolManager);
                m_poolManager = nullptr;
            }
            for (auto* q : m_allQueues)
                m_allocator.Delete(q);
            m_allQueues.Clear();
            m_gfxQueues.Clear();
            m_compQueues.Clear();
            m_xferQueues.Clear();
            if (m_device != VK_NULL_HANDLE)
            {
                vkDestroyDevice(m_device, nullptr);
                m_device = VK_NULL_HANDLE;
            }
            IAllocator& alloc = m_allocator;
            this->~VkDeviceImpl();
            alloc.Free(this);
        }

        // ---- Swap chain sync ----
        void setPendingSwapChainSync(VkSemaphore acquire, VkSemaphore present)
        {
            m_pendingAcquire = acquire;
            m_pendingPresent = present;
            m_hasPendingSync = true;
        }
        bool consumePendingSwapChainSync(VkSemaphore& acquire, VkSemaphore& present)
        {
            if (!m_hasPendingSync)
                return false;
            acquire = m_pendingAcquire;
            present = m_pendingPresent;
            m_hasPendingSync = false;
            return true;
        }

        [[nodiscard]] VkDevice handle() const { return m_device; }
        [[nodiscard]] VkAdapterImpl* adapter() const { return m_adapter; }
        [[nodiscard]] bool validationEnabled() const { return m_validationEnabled; }

        [[nodiscard]] const BindingShifts& bindingShifts() const { return m_bindingShifts; }
        void setBindingShifts(const BindingShifts& s) { m_bindingShifts = s; }

        /// Set a Vulkan debug name on an object (only when validation is enabled).
        void setDebugName(VkObjectType objectType, u64 objectHandle, StringView name)
        {
            if (!m_validationEnabled || name.IsEmpty())
                return;
            auto pfn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetDeviceProcAddr(m_device, "vkSetDebugUtilsObjectNameEXT"));
            if (!pfn)
                return;
            char buf[256]{};
            auto len = Min(name.Size(), static_cast<usize>(255));
            std::memcpy(buf, name.Data(), len);
            VkDebugUtilsObjectNameInfoEXT ni{};
            ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            ni.objectType = objectType;
            ni.objectHandle = objectHandle;
            ni.pObjectName = buf;
            pfn(m_device, &ni);
        }

    private:
        IAllocator& m_allocator;
        VkDevice m_device = VK_NULL_HANDLE;
        VkAdapterImpl* m_adapter = nullptr;
        bool m_lost = false; // sticky device-lost latch (see markLost)
        bool m_meshEnabled = false;
        bool m_rtEnabled = false;
        bool m_bindlessEnabled = false;
        bool m_validationEnabled = false;
        BindingShifts m_bindingShifts = BindingShifts::standard();
        VkDescriptorPoolManager* m_poolManager = nullptr;

        Array<VkQueueImpl*> m_allQueues, m_gfxQueues, m_compQueues, m_xferQueues;

        VkSemaphore m_pendingAcquire = VK_NULL_HANDLE;
        VkSemaphore m_pendingPresent = VK_NULL_HANDLE;
        bool m_hasPendingSync = false;
    };

    // ---- Adapter::CreateDevice implementation ----

    Status VkAdapterImpl::CreateDevice(const DeviceDesc& desc, Device*& out)
    {
        auto* dev = m_allocator.New<VkDeviceImpl>(m_allocator);
        if (dev->init(this, desc) != ErrorCode::Ok)
        {
            m_allocator.Delete(dev);
            out = nullptr;
            return ErrorCode::Unknown;
        }
        out = dev;
        return ErrorCode::Ok;
    }

    // ---- SwapChain acquire/present implementations ----

    Status VkSwapChainImpl::AcquireNextImage()
    {
        VkSemaphore acquireSem = m_acquireSems[m_frameIndex];
        u32 imgIdx = 0;
        VkResult vr = vkAcquireNextImageKHR(m_device, m_swapchain, ~0ull, acquireSem,
                                            VK_NULL_HANDLE, &imgIdx);
        m_currentImageIndex = imgIdx;
        if (vr == VK_ERROR_OUT_OF_DATE_KHR)
            return ErrorCode::Unknown;
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR)
            return ErrorCode::Unknown;
        VkSemaphore presentSem = m_presentSems[m_currentImageIndex];
        m_owner->setPendingSwapChainSync(acquireSem, presentSem);
        return ErrorCode::Ok;
    }

    Status VkSwapChainImpl::Present(Queue* queue)
    {
        auto* vkQ = static_cast<VkQueueImpl*>(queue);
        VkSemaphore waitSem = m_presentSems[m_currentImageIndex];
        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &waitSem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &m_swapchain;
        pi.pImageIndices = &m_currentImageIndex;
        VkResult vr = vkQueuePresentKHR(vkQ->handle(), &pi);
        m_frameIndex = (m_frameIndex + 1) % m_bufferCount;
        if (vr == VK_ERROR_DEVICE_LOST)
            vkQ->owner()->markLost();
        if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR)
            return ErrorCode::Unknown;
        return vr == VK_SUCCESS ? ErrorCode::Ok : ErrorCode::Unknown;
    }

    // ---- Queue submits (out-of-line: report device loss + swap chain sync need Device) ----

    void VkQueueImpl::Submit(Span<CommandBuffer* const> cmdBufs)
    {
        if (cmdBufs.Size() == 0)
            return;
        Array<VkCommandBuffer> bufs(cmdBufs.Size());
        for (usize i = 0; i < cmdBufs.Size(); ++i)
            bufs[i] = static_cast<VkCommandBufferImpl*>(cmdBufs[i])->handle();

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = static_cast<u32>(bufs.Size());
        si.pCommandBuffers = bufs.Data();
        if (vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST)
            m_device->markLost();
    }

    void VkQueueImpl::Submit(Span<CommandBuffer* const> cmdBufs, Fence* signalFence,
                             u64 signalValue)
    {
        if (cmdBufs.Size() == 0)
            return;
        Array<VkCommandBuffer> bufs(cmdBufs.Size());
        for (usize i = 0; i < cmdBufs.Size(); ++i)
            bufs[i] = static_cast<VkCommandBufferImpl*>(cmdBufs[i])->handle();

        auto* vkFence = static_cast<VkFenceImpl*>(signalFence);
        if (!vkFence)
            return;

        VkSemaphore acquireSem = VK_NULL_HANDLE, presentSem = VK_NULL_HANDLE;
        bool hasSync = m_device->consumePendingSwapChainSync(acquireSem, presentSem);

        VkSemaphore waitSems[1] = {acquireSem};
        VkPipelineStageFlags waitStages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        u64 waitValues[1] = {0};

        auto timelineSem = vkFence->handle();
        VkSemaphore signalSems[2] = {timelineSem, presentSem};
        u64 signalValues[2] = {signalValue, 0};

        VkTimelineSemaphoreSubmitInfo tsi{};
        tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsi.signalSemaphoreValueCount = hasSync ? 2u : 1u;
        tsi.pSignalSemaphoreValues = signalValues;
        tsi.waitSemaphoreValueCount = hasSync ? 1u : 0u;
        tsi.pWaitSemaphoreValues = waitValues;

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext = &tsi;
        si.commandBufferCount = static_cast<u32>(bufs.Size());
        si.pCommandBuffers = bufs.Data();
        si.signalSemaphoreCount = hasSync ? 2u : 1u;
        si.pSignalSemaphores = signalSems;
        if (hasSync)
        {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = waitSems;
            si.pWaitDstStageMask = waitStages;
        }

        if (vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST)
            m_device->markLost();
    }

    void VkQueueImpl::Submit(Span<CommandBuffer* const> cmdBufs, Span<Fence* const> waitFences,
                             Span<const u64> waitVals, Fence* signalFence, u64 signalValue)
    {
        if (cmdBufs.Size() == 0)
            return;
        Array<VkCommandBuffer> bufs(cmdBufs.Size());
        for (usize i = 0; i < cmdBufs.Size(); ++i)
            bufs[i] = static_cast<VkCommandBufferImpl*>(cmdBufs[i])->handle();

        // 5-arg submit does NOT consume swap chain sync - matching Sedulous.
        // The 2-arg submit (used by the first queue submit each frame) handles it.
        Array<VkSemaphore> waitSems(waitFences.Size());
        Array<VkPipelineStageFlags> waitStages(waitFences.Size());
        for (usize i = 0; i < waitFences.Size(); ++i)
        {
            waitSems[i] = static_cast<VkFenceImpl*>(waitFences[i])->handle();
            waitStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }

        VkTimelineSemaphoreSubmitInfo tsi{};
        tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsi.waitSemaphoreValueCount = static_cast<u32>(waitVals.Size());
        tsi.pWaitSemaphoreValues = waitVals.Data();

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext = &tsi;
        si.commandBufferCount = static_cast<u32>(bufs.Size());
        si.pCommandBuffers = bufs.Data();
        si.waitSemaphoreCount = static_cast<u32>(waitSems.Size());
        si.pWaitSemaphores = waitSems.Data();
        si.pWaitDstStageMask = waitStages.Data();

        VkSemaphore signalSem = VK_NULL_HANDLE;
        if (signalFence)
        {
            signalSem = static_cast<VkFenceImpl*>(signalFence)->handle();
            tsi.signalSemaphoreValueCount = 1;
            tsi.pSignalSemaphoreValues = &signalValue;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &signalSem;
        }

        if (vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST)
            m_device->markLost();
    }

} // namespace draconic::rhi::vk
