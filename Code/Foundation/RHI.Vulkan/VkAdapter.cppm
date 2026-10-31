// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of Adapter.
/// Queries physical device properties, features, and queue families.
/// Provides feature detection and queue family selection.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

#include <cstring>

export module foundation.rhi.vulkan:adapter;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    class VkDeviceImpl; // forward

    class VkAdapterImpl : public Adapter
    {
    public:
        VkAdapterImpl(VkPhysicalDevice physicalDevice, VkInstance instance, IAllocator& allocator)
            : m_allocator(allocator), m_physicalDevice(physicalDevice), m_instance(instance)
        {
            vkGetPhysicalDeviceProperties(m_physicalDevice, &m_properties);
            vkGetPhysicalDeviceFeatures(m_physicalDevice, &m_features10);
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memoryProperties);

            u32 qfCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qfCount, nullptr);
            m_queueFamilies.Resize(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qfCount,
                                                     m_queueFamilies.Data());

            queryExtensionSupport();
        }

        // ---- Adapter interface ----

        void GetInfo(AdapterInfo& out) override
        {
            out.name =
                String(StringView(reinterpret_cast<const utf8char*>(m_properties.deviceName)));
            out.vendorId = m_properties.vendorID;
            out.deviceId = m_properties.deviceID;

            switch (m_properties.deviceType)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                out.type = AdapterType::DiscreteGpu;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                out.type = AdapterType::IntegratedGpu;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                out.type = AdapterType::Cpu;
                break;
            default:
                out.type = AdapterType::Unknown;
                break;
            }

            out.supportedFeatures = buildFeatures();
        }

        Status CreateDevice(const DeviceDesc& desc, Device*& out) override;

        // ---- Feature building ----

        DeviceFeatures buildFeatures() const
        {
            DeviceFeatures f{};

            f.bindlessDescriptors = m_supportsDescriptorIndexing;
            f.timestampQueries = m_properties.limits.timestampComputeAndGraphics;
            f.timestampCalibration = m_calibratedTimestamps != nullptr;
            f.memoryBudget = m_supportsMemoryBudget;
            f.occlusionQueries = true; // vkCmdBeginQuery works anywhere in a pass
            f.borderSampling = true;
            f.multiDrawIndirect = m_features10.multiDrawIndirect;
            f.depthClamp = m_features10.depthClamp;
            f.fillModeWireframe = m_features10.fillModeNonSolid;
            f.textureCompressionBC = m_features10.textureCompressionBC;
            f.textureCompressionASTC = m_features10.textureCompressionASTC_LDR;
            f.independentBlend = m_features10.independentBlend;
            f.multiViewport = m_features10.multiViewport;
            f.meshShaders = m_supportsMeshShader;
            f.rayTracing = m_supportsRayTracing;
            f.pipelineStatisticsQueries = m_features10.pipelineStatisticsQuery;

            // Mesh shader limits.
            if (m_supportsMeshShader)
            {
                VkPhysicalDeviceMeshShaderPropertiesEXT meshProps{};
                meshProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 p2{};
                p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                p2.pNext = &meshProps;
                vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
                f.maxMeshOutputVertices = meshProps.maxMeshOutputVertices;
                f.maxMeshOutputPrimitives = meshProps.maxMeshOutputPrimitives;
                f.maxMeshWorkgroupSize = meshProps.maxMeshWorkGroupInvocations;
                f.maxTaskWorkgroupSize = meshProps.maxTaskWorkGroupInvocations;
            }

            // Limits.
            f.maxBindGroups = m_properties.limits.maxBoundDescriptorSets;
            f.maxBindingsPerGroup = m_properties.limits.maxDescriptorSetUniformBuffers;
            f.maxPushConstantSize = m_properties.limits.maxPushConstantsSize;
            f.maxTextureDimension2D = m_properties.limits.maxImageDimension2D;
            f.maxTextureArrayLayers = m_properties.limits.maxImageArrayLayers;
            f.maxComputeWorkgroupSizeX = m_properties.limits.maxComputeWorkGroupSize[0];
            f.maxComputeWorkgroupSizeY = m_properties.limits.maxComputeWorkGroupSize[1];
            f.maxComputeWorkgroupSizeZ = m_properties.limits.maxComputeWorkGroupSize[2];
            f.maxComputeWorkgroupsPerDimension = m_properties.limits.maxComputeWorkGroupCount[0];
            f.maxBufferSize = static_cast<u64>(m_properties.limits.maxStorageBufferRange);
            f.minUniformBufferOffsetAlignment =
                static_cast<u32>(m_properties.limits.minUniformBufferOffsetAlignment);
            f.minStorageBufferOffsetAlignment =
                static_cast<u32>(m_properties.limits.minStorageBufferOffsetAlignment);
            f.timestampPeriodNs = static_cast<u32>(m_properties.limits.timestampPeriod);

            return f;
        }

        // ---- Queue family selection ----

        /// Finds the best queue family index for the given type.
        /// Prefers dedicated families for Compute and Transfer.
        i32 findQueueFamily(QueueType type) const
        {
            const auto count = static_cast<i32>(m_queueFamilies.Size());
            switch (type)
            {
            case QueueType::Graphics:
                for (i32 i = 0; i < count; ++i)
                    if (m_queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                        return i;
                break;
            case QueueType::Compute:
                // Prefer dedicated (no graphics).
                for (i32 i = 0; i < count; ++i)
                {
                    auto f = m_queueFamilies[i].queueFlags;
                    if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT))
                        return i;
                }
                for (i32 i = 0; i < count; ++i)
                    if (m_queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
                        return i;
                break;
            case QueueType::Transfer:
                // Prefer dedicated (no graphics or compute).
                for (i32 i = 0; i < count; ++i)
                {
                    auto f = m_queueFamilies[i].queueFlags;
                    if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) &&
                        !(f & VK_QUEUE_COMPUTE_BIT))
                        return i;
                }
                for (i32 i = 0; i < count; ++i)
                    if (m_queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
                        return i;
                break;
            }
            return -1;
        }

        /// Finds a memory type index matching the filter and property requirements.
        i32 findMemoryType(u32 typeFilter, VkMemoryPropertyFlags properties) const
        {
            for (u32 i = 0; i < m_memoryProperties.memoryTypeCount; ++i)
            {
                if ((typeFilter & (1 << i)) &&
                    (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
                    return static_cast<i32>(i);
            }
            return -1;
        }

        /// Maps a MemoryLocation to VkMemoryPropertyFlags.
        static VkMemoryPropertyFlags getMemoryFlags(MemoryLocation loc)
        {
            switch (loc)
            {
            case MemoryLocation::GpuOnly:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            case MemoryLocation::CpuToGpu:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            case MemoryLocation::GpuToCpu:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            case MemoryLocation::Auto:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            }
            return 0;
        }

        // ---- Internal accessors ----
        [[nodiscard]] VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
        [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return m_properties; }
        [[nodiscard]] const VkPhysicalDeviceFeatures& features10() const { return m_features10; }
        [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memoryProperties() const
        {
            return m_memoryProperties;
        }
        [[nodiscard]] const Array<VkQueueFamilyProperties>& queueFamilies() const
        {
            return m_queueFamilies;
        }

        [[nodiscard]] bool supportsDescriptorIndexing() const
        {
            return m_supportsDescriptorIndexing;
        }
        [[nodiscard]] bool supportsMeshShader() const { return m_supportsMeshShader; }
        [[nodiscard]] bool supportsRayTracing() const { return m_supportsRayTracing; }
        /// The calibrated-timestamps extension to enable (KHR or EXT), null when the device cannot
        /// read its timestamps together with the clock core::GetTicks reads.
        [[nodiscard]] const char* calibratedTimestampsExtension() const { return m_calibratedTimestamps; }
        /// That clock's time domain (CLOCK_MONOTONIC on Linux, QueryPerformanceCounter on Windows).
        [[nodiscard]] VkTimeDomainEXT hostTimeDomain() const { return m_hostTimeDomain; }
        [[nodiscard]] bool supportsMemoryBudget() const { return m_supportsMemoryBudget; }
        /// Swapchain images viewable in another format (an sRGB swapchain's UNORM view).
        [[nodiscard]] bool supportsSwapchainMutableFormat() const { return m_supportsSwapchainMutableFormat; }
        [[nodiscard]] IAllocator& allocator() const noexcept { return m_allocator; }

    private:
        void queryExtensionSupport()
        {
            u32 extCount = 0;
            vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
            Array<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, exts.Data());

            for (const auto& ext : exts)
            {
                const char* name = ext.extensionName;
                if (std::strcmp(name, "VK_KHR_dynamic_rendering") == 0)
                    m_supportsDynamicRendering = true;
                if (std::strcmp(name, "VK_KHR_timeline_semaphore") == 0)
                    m_supportsTimelineSemaphore = true;
                if (std::strcmp(name, "VK_KHR_synchronization2") == 0)
                    m_supportsSynchronization2 = true;
                if (std::strcmp(name, "VK_EXT_descriptor_indexing") == 0)
                    m_supportsDescriptorIndexing = true;
                if (std::strcmp(name, "VK_EXT_mesh_shader") == 0)
                    m_supportsMeshShader = true;
                if (std::strcmp(name, "VK_KHR_ray_tracing_pipeline") == 0)
                    m_supportsRayTracing = true;
                if (std::strcmp(name, "VK_EXT_memory_budget") == 0)
                    m_supportsMemoryBudget = true;
                if (std::strcmp(name, "VK_KHR_swapchain_mutable_format") == 0)
                    m_supportsSwapchainMutableFormat = true;
                if (std::strcmp(name, "VK_KHR_calibrated_timestamps") == 0)
                    m_calibratedTimestamps = "VK_KHR_calibrated_timestamps";
                if (std::strcmp(name, "VK_EXT_calibrated_timestamps") == 0 && m_calibratedTimestamps == nullptr)
                    m_calibratedTimestamps = "VK_EXT_calibrated_timestamps";
            }
            // Calibration is only of use when the device's domain and the host clock core::GetTicks
            // reads are both offered.
            if (m_calibratedTimestamps != nullptr && !HasCalibrationDomains())
            {
                m_calibratedTimestamps = nullptr;
            }

            // Vulkan 1.3+: these are core.
            u32 major = VK_API_VERSION_MAJOR(m_properties.apiVersion);
            u32 minor = VK_API_VERSION_MINOR(m_properties.apiVersion);
            if (major > 1 || (major == 1 && minor >= 3))
            {
                m_supportsDynamicRendering = true;
                m_supportsTimelineSemaphore = true;
                m_supportsSynchronization2 = true;
                m_supportsDescriptorIndexing = true;
            }
            else if (major == 1 && minor >= 2)
            {
                m_supportsTimelineSemaphore = true;
                m_supportsDescriptorIndexing = true;
            }
        }

        bool HasCalibrationDomains() const
        {
            // Ask for the entry point of the extension this device advertised, not KHR with an EXT
            // fallback. vkGetInstanceProcAddr answers for anything the loader knows, so the KHR
            // name returns a valid trampoline even on a driver that implements only the EXT form;
            // calling it then reaches an ICD with no such entry and the loader logs "ICD associated
            // with VkPhysicalDevice does not support GetPhysicalDeviceCalibrateableTimeDomainsKHR".
            // m_calibratedTimestamps holds the one queryExtensionSupport() found in this device's
            // extension list.
            if (m_calibratedTimestamps == nullptr)
            {
                return false;
            }
            const bool khr = std::strcmp(m_calibratedTimestamps, "VK_KHR_calibrated_timestamps") == 0;
            // The KHR and EXT entry points share a signature, so one pointer type serves both.
            auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
                vkGetInstanceProcAddr(m_instance,
                                      khr ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"
                                          : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
            if (query == nullptr)
            {
                return false;
            }
            u32 count = 0;
            if (query(m_physicalDevice, &count, nullptr) != VK_SUCCESS || count == 0)
            {
                return false;
            }
            Array<VkTimeDomainEXT> domains(count);
            if (query(m_physicalDevice, &count, domains.Data()) != VK_SUCCESS)
            {
                return false;
            }
            bool device = false, host = false;
            for (u32 i = 0; i < count; ++i)
            {
                device = device || domains[i] == VK_TIME_DOMAIN_DEVICE_EXT;
                host = host || domains[i] == m_hostTimeDomain;
            }
            return device && host;
        }

        IAllocator& m_allocator;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkInstance m_instance = VK_NULL_HANDLE;
        const char* m_calibratedTimestamps = nullptr;
        bool m_supportsMemoryBudget = false;
        bool m_supportsSwapchainMutableFormat = false;
#if PLATFORM_WINDOWS
        VkTimeDomainEXT m_hostTimeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
#else
        VkTimeDomainEXT m_hostTimeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
#endif
        VkPhysicalDeviceProperties m_properties{};
        VkPhysicalDeviceFeatures m_features10{};
        VkPhysicalDeviceMemoryProperties m_memoryProperties{};
        Array<VkQueueFamilyProperties> m_queueFamilies;

        bool m_supportsDynamicRendering = false;
        bool m_supportsTimelineSemaphore = false;
        bool m_supportsSynchronization2 = false;
        bool m_supportsDescriptorIndexing = false;
        bool m_supportsMeshShader = false;
        bool m_supportsRayTracing = false;
    };

} // namespace foundation::rhi::vk
