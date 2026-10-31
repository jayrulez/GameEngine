// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:adapter - Adapter over WGPUAdapter.
///
/// GetInfo maps WGPUAdapterInfo, limits and features onto the RHI's AdapterInfo;
/// CreateDevice performs the async wgpuAdapterRequestDevice through the ProcessEvents
/// pump (see :api) and registers the device-lost and uncaptured-error callbacks before
/// the WebGpuDevice wrapper exists to receive them.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

export module foundation.rhi.webgpu:adapter;

import foundation.core;
import foundation.rhi;
import :api;
import :device;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuAdapter final : public Adapter
    {
    public:
        WebGpuAdapter(const WebGpuApi& api, WGPUInstance instance, WGPUAdapter adapter,
                      IAllocator& allocator)
            : m_api(&api), m_instance(instance), m_adapter(adapter), m_allocator(allocator)
        {
        }

        [[nodiscard]] WGPUAdapter Handle() const { return m_adapter; }

        // The wgpu backend this adapter drives (Vulkan/D3D12/...). One physical GPU appears
        // once per instance backend, so the enumeration ordering needs this to prefer the
        // backend that can present on the platform.
        [[nodiscard]] WGPUBackendType WgpuBackendType() const
        {
            WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
            m_api->wgpuAdapterGetInfo(m_adapter, &info);
            const WGPUBackendType type = info.backendType;
            m_api->wgpuAdapterInfoFreeMembers(info);
            return type;
        }

        void GetInfo(AdapterInfo& out) override
        {
            WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
            m_api->wgpuAdapterGetInfo(m_adapter, &info);

            out.name = String(StringView(reinterpret_cast<const utf8char*>(info.device.data),
                                         info.device.length));
            out.vendorId = info.vendorID;
            out.deviceId = info.deviceID;
            switch (info.adapterType)
            {
            case WGPUAdapterType_DiscreteGPU:
                out.type = AdapterType::DiscreteGpu;
                break;
            case WGPUAdapterType_IntegratedGPU:
                out.type = AdapterType::IntegratedGpu;
                break;
            case WGPUAdapterType_CPU:
                out.type = AdapterType::Cpu;
                break;
            default:
                out.type = AdapterType::Unknown;
                break;
            }
            m_api->wgpuAdapterInfoFreeMembers(info);

            WGPULimits limits = WGPU_LIMITS_INIT;
            if (m_api->wgpuAdapterGetLimits(m_adapter, &limits) == WGPUStatus_Success)
            {
                out.supportedFeatures.maxBindGroups = limits.maxBindGroups;
                out.supportedFeatures.maxBindingsPerGroup =
                    limits.maxSampledTexturesPerShaderStage;
                out.supportedFeatures.maxTextureDimension2D = limits.maxTextureDimension2D;
                out.supportedFeatures.maxTextureArrayLayers = limits.maxTextureArrayLayers;
                out.supportedFeatures.maxComputeWorkgroupSizeX = limits.maxComputeWorkgroupSizeX;
                out.supportedFeatures.maxComputeWorkgroupSizeY = limits.maxComputeWorkgroupSizeY;
                out.supportedFeatures.maxComputeWorkgroupSizeZ = limits.maxComputeWorkgroupSizeZ;
                out.supportedFeatures.maxComputeWorkgroupsPerDimension =
                    limits.maxComputeWorkgroupsPerDimension;
                out.supportedFeatures.minUniformBufferOffsetAlignment =
                    limits.minUniformBufferOffsetAlignment;
                out.supportedFeatures.minStorageBufferOffsetAlignment =
                    limits.minStorageBufferOffsetAlignment;
                out.supportedFeatures.maxBufferSize = limits.maxBufferSize;
            }

            WGPUSupportedFeatures features = {};
            m_api->wgpuAdapterGetFeatures(m_adapter, &features);
            for (usize i = 0; i < features.featureCount; ++i)
            {
                switch (features.features[i])
                {
                case WGPUFeatureName_TimestampQuery:
                    out.supportedFeatures.timestampQueries = true;
                    break;
                case WGPUFeatureName_TextureCompressionBC:
                    out.supportedFeatures.textureCompressionBC = true;
                    break;
                case WGPUFeatureName_TextureCompressionASTC:
                    out.supportedFeatures.textureCompressionASTC = true;
                    break;
                case WGPUFeatureName_DepthClipControl:
                    out.supportedFeatures.depthClamp = true;
                    break;
                default:
                    break;
                }
            }
            m_api->wgpuSupportedFeaturesFreeMembers(features);

            // WebGPU guarantees per-attachment blend state. Push-constant support
            // (Immediates) is decided at CreateDevice; the device stamps its
            // maxPushConstantSize accordingly. Occlusion works via the pass-begin
            // declaration (RenderPassDesc.occlusionQuerySet).
            out.supportedFeatures.independentBlend = true;
            out.supportedFeatures.occlusionQueries = true;
        }

        Status CreateDevice(const DeviceDesc&, Device*& out) override
        {
            out = nullptr;

            // The wrapper does not exist until the request completes, so loss routes
            // through a slot the callback fills lazily.
            struct LostRoute
            {
                WebGpuDevice* device = nullptr;
            };
            // Heap record handed to the wrapper below (AdoptLostRoute): WebGpuDevice::Destroy
            // frees it after releasing the wgpu device and flushing pending callbacks, the one
            // point where the still-registered lost callback provably cannot fire again.
            auto* lostRoute = m_allocator.New<LostRoute>();

            Array<WGPUFeatureName> required;
            AdapterInfo info;
            GetInfo(info);
            // Enable what the adapter has, like the Vulkan backend does; callers gate on
            // device->features, not on what they requested.
            if (info.supportedFeatures.timestampQueries)
            {
                required.PushBack(WGPUFeatureName_TimestampQuery);
            }
            if (info.supportedFeatures.textureCompressionBC)
            {
                required.PushBack(WGPUFeatureName_TextureCompressionBC);
            }
            // ASTC is the mobile-web compressed family. Desktop browsers usually expose only BC;
            // mobile browsers expose ASTC. Request whichever the adapter has.
            if (info.supportedFeatures.textureCompressionASTC)
            {
                required.PushBack(WGPUFeatureName_TextureCompressionASTC);
            }
            if (info.supportedFeatures.depthClamp)
            {
                required.PushBack(WGPUFeatureName_DepthClipControl);
            }
            // Push constants are WebGPU immediates (a wgpu-native feature; the field is in
            // the standard pipeline-layout descriptor). The feature enum is
            // wgpu-native-only and Dawn/emdawnwebgpu has no immediates path, so on web
            // SetPushConstants goes through the uniform-buffer emulation (the
            // push_constant_emulator partition), which is fully functional.
#if PLATFORM_WEB
            const bool immediatesSupported = false;
#else
            const auto immediates = static_cast<WGPUFeatureName>(WGPUNativeFeature_Immediates);
            const bool immediatesSupported =
                m_api->wgpuAdapterHasFeature(m_adapter, immediates) != 0u;
            if (immediatesSupported)
            {
                required.PushBack(immediates);
            }
#endif
            // 32-bit float textures are non-filterable in core WebGPU; the renderer
            // linear-samples HDR sky/IBL sources, so enable filtering when available.
            const bool f32Filter =
                m_api->wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_Float32Filterable) != 0u;
            if (f32Filter)
            {
                required.PushBack(WGPUFeatureName_Float32Filterable);
            }
            LogInfof("WebGpuAdapter: float32-filterable %s",
                     f32Filter ? "ENABLED" : "MISSING (rgba32f linear-sample will fail)");
            // Encoder-level WriteTimestamp (the RHI's CommandEncoder::WriteTimestamp, used
            // by the GPU GraphProfiler) is a separate wgpu feature from pass-boundary
            // timestamps. The enum is wgpu-native-only, so it is unavailable on web.
            bool encoderTimestampsEnabled = false;
#if !PLATFORM_WEB
            const auto encoderTimestamps =
                static_cast<WGPUFeatureName>(WGPUNativeFeature_TimestampQueryInsideEncoders);
            if (info.supportedFeatures.timestampQueries &&
                m_api->wgpuAdapterHasFeature(m_adapter, encoderTimestamps) != 0u)
            {
                required.PushBack(encoderTimestamps);
                encoderTimestampsEnabled = true;
            }
#endif

            // Request the adapter's own limits wholesale (always legal, unlocks real
            // texture-size/buffer ceilings); the immediates budget is the RHI's
            // 128-byte push-constant contract.
            WGPULimits adapterLimits = WGPU_LIMITS_INIT;
            WGPULimits requiredLimits = WGPU_LIMITS_INIT;
            bool haveLimits =
                m_api->wgpuAdapterGetLimits(m_adapter, &adapterLimits) == WGPUStatus_Success;
            if (haveLimits)
            {
                requiredLimits = adapterLimits; // adapter-supported values are always legal
                if (immediatesSupported && requiredLimits.maxImmediateSize < 128 &&
                    adapterLimits.maxImmediateSize >= 128)
                {
                    requiredLimits.maxImmediateSize = 128;
                }
            }

            WGPUDeviceDescriptor deviceDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
            deviceDesc.requiredFeatureCount = required.Size();
            deviceDesc.requiredFeatures = required.Data();
            if (haveLimits)
            {
                deviceDesc.requiredLimits = &requiredLimits;
            }
            deviceDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
            deviceDesc.deviceLostCallbackInfo.callback =
                [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message,
                   void* userdata1, void*)
            {
                if (reason == WGPUDeviceLostReason_Destroyed)
                {
                    return; // orderly teardown is not a loss
                }
                auto* route = static_cast<LostRoute*>(userdata1);
                if (route->device != nullptr)
                {
                    route->device->MarkLost();
                }
                LogErrorf("[webgpu] device LOST (reason %d): %.*s", static_cast<int>(reason),
                          static_cast<int>(message.length),
                          reinterpret_cast<const char*>(message.data));
            };
            deviceDesc.deviceLostCallbackInfo.userdata1 = lostRoute;
            deviceDesc.uncapturedErrorCallbackInfo.callback =
                [](WGPUDevice const*, WGPUErrorType errorType, WGPUStringView message, void*,
                   void*)
            {
                LogErrorf("[webgpu] uncaptured error (type %d): %.*s",
                          static_cast<int>(errorType), static_cast<int>(message.length),
                          reinterpret_cast<const char*>(message.data));
            };

            // A heap record, orphaned on timeout: a stack record would leave the
            // still-registered callback writing through a dead frame if the pump gives up
            // before the request resolves.
            struct Result
            {
                IAllocator* allocator = nullptr;
                const WebGpuApi* api = nullptr;
                WGPUDevice device = nullptr;
                bool done = false;
                bool orphaned = false; // waiter gave up; the callback owns deletion
            };
            auto* result = m_allocator.New<Result>();
            result->allocator = &m_allocator;
            result->api = m_api;

            WGPURequestDeviceCallbackInfo callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowProcessEvents;
            callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice created,
                                   WGPUStringView message, void* userdata1, void*)
            {
                auto* r = static_cast<Result*>(userdata1);
                if (r->orphaned)
                {
                    if (status == WGPURequestDeviceStatus_Success && created != nullptr)
                    {
                        r->api->wgpuDeviceRelease(created); // nobody else will
                    }
                    r->allocator->Delete(r);
                    return;
                }
                if (status == WGPURequestDeviceStatus_Success)
                {
                    r->device = created;
                }
                else
                {
                    LogErrorf("[webgpu] RequestDevice failed: %.*s",
                              static_cast<int>(message.length),
                              reinterpret_cast<const char*>(message.data));
                }
                r->done = true;
            };
            callback.userdata1 = result;

            (void)m_api->wgpuAdapterRequestDevice(m_adapter, &deviceDesc, callback);
            m_api->PumpUntil(m_instance, result->done);

            WGPUDevice device = nullptr;
            if (!result->done)
            {
                LogError("[webgpu] RequestDevice callback did not fire (pump timed out)");
                result->orphaned = true; // the callback owns the record now
            }
            else
            {
                device = result->device;
                m_allocator.Delete(result);
            }
            if (device == nullptr)
            {
                m_allocator.Delete(lostRoute);
                return ErrorCode::Unknown;
            }

            auto* wrapper = m_allocator.New<WebGpuDevice>(*m_api, m_instance, m_adapter,
                                                          device, m_allocator);
            wrapper->features = info.supportedFeatures;
            // The RHI's timestamp feature is CommandEncoder::WriteTimestamp, which needs the
            // in-encoder feature; a browser's TimestampQuery alone writes nothing there.
            wrapper->features.timestampQueries = encoderTimestampsEnabled;
            // SetPushConstants works either way (native immediates or the uniform-buffer
            // emulation), so the budget is always advertised. Reporting 0 under emulation would
            // make capability-checking callers disable the very paths the emulator exists to
            // serve.
            wrapper->features.maxPushConstantSize = 128u;
            wrapper->SetImmediatesSupported(immediatesSupported);
            lostRoute->device = wrapper;
            wrapper->AdoptLostRoute(lostRoute); // wrapper frees it at Destroy (post-flush)
            out = wrapper;
            return ErrorCode::Ok;
        }

    private:
        const WebGpuApi* m_api;
        WGPUInstance m_instance;
        WGPUAdapter m_adapter;
        IAllocator& m_allocator;
    };
}
