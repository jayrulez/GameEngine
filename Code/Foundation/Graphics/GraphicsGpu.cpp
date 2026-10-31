// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Graphics - implementation unit for `foundation.graphics.gpu`.
//
// All the backend-coupled code (Vulkan, DX12, validation) lives here rather than in
// the interface unit. Because these imports sit in the implementation unit,
// consumers of foundation.graphics.gpu never load the backend BMIs at compile time;
// they only link the backends. This keeps the render host's only Vulkan-coupled
// part off every consumer's module closure.

module;
#include "Core/Prelude.h"

module foundation.graphics.gpu;

import foundation.core;
import foundation.rhi;
#ifdef OPTION_HAS_VULKAN
import foundation.rhi.vulkan;
#endif
#ifdef OPTION_HAS_DX12
import foundation.rhi.dx12;
#endif
#ifdef OPTION_HAS_WEBGPU
import foundation.rhi.webgpu;
#endif
import foundation.rhi.validation;
import foundation.graphics;
import foundation.graphics.null; // Null backend delegation

namespace core = foundation::core;
namespace rhi = foundation::rhi;

namespace foundation::graphics
{
    namespace
    {
        [[nodiscard]] const char* BackendName(BackendType b)
        {
            switch (b)
            {
            case BackendType::Vulkan:
                return "Vulkan";
            case BackendType::DX12:
                return "DX12";
            case BackendType::WebGPU:
                return "WebGPU";
            case BackendType::Null:
                return "Null";
            }
            return "?";
        }

        // Whether this backend is compiled into this binary, as distinct from whether it failed to
        // initialise: a build without the Vulkan SDK has no Vulkan RHI at all, and "failed to
        // create graphics device" alone does not tell those apart.
        [[nodiscard]] bool BackendCompiledIn(BackendType b)
        {
            switch (b)
            {
            case BackendType::Vulkan:
#ifdef OPTION_HAS_VULKAN
                return true;
#else
                return false;
#endif
            case BackendType::DX12:
#ifdef OPTION_HAS_DX12
                return true;
#else
                return false;
#endif
            case BackendType::WebGPU:
#ifdef OPTION_HAS_WEBGPU
                return true;
#else
                return false;
#endif
            case BackendType::Null:
                return true;
            }
            return false;
        }

        // One attempt at one backend. `raw` is left null when the backend is absent or fails.
        // (`desc` goes unread where only backends without options are compiled in: the web build.)
        [[nodiscard]] bool TryCreateBackend(BackendType b, [[maybe_unused]] const GraphicsDeviceDesc& desc,
                                            rhi::Backend*& raw)
        {
            raw = nullptr;
            switch (b)
            {
            case BackendType::Vulkan:
            {
#ifdef OPTION_HAS_VULKAN
                rhi::vk::VkBackendDesc bd{};
                bd.enableValidation = desc.enableValidation;
                if (!rhi::vk::CreateBackend(bd, raw).IsOk())
                {
                    rhi::LogError("CreateGraphicsDevice: Vulkan CreateBackend failed");
                    raw = nullptr;
                    return false;
                }
                return true;
#else
                return false;
#endif
            }
            case BackendType::DX12:
            {
#ifdef OPTION_HAS_DX12
                rhi::dx12::DxBackendDesc bd{};
                bd.enableValidation = desc.enableValidation;
                if (!rhi::dx12::CreateDxBackend(bd, raw).IsOk())
                {
                    rhi::LogError("CreateGraphicsDevice: DX12 CreateDxBackend failed");
                    raw = nullptr;
                    return false;
                }
                return true;
#else
                return false;
#endif
            }
            case BackendType::WebGPU:
            {
#ifdef OPTION_HAS_WEBGPU
                rhi::webgpu::WebGpuBackendDesc bd{};
                if (!rhi::webgpu::CreateBackend(bd, raw, core::DefaultAllocator()).IsOk())
                {
                    rhi::LogError("CreateGraphicsDevice: WebGPU CreateBackend failed");
                    raw = nullptr;
                    return false;
                }
                return true;
#else
                return false;
#endif
            }
            case BackendType::Null:
                return false; // handled by the caller, never a fallback candidate
            }
            return false;
        }
    }

    core::Result<core::UniquePtr<GraphicsDevice>>
    CreateGraphicsDevice(const GraphicsDeviceDesc& desc)
    {
        if (desc.backend == BackendType::Null)
        {
            return CreateNullGraphicsDevice(desc.framesInFlight);
        }

        // The requested backend first, then the other GPU backends this binary carries.
        //
        // The requested backend is a default or a guess far more often than an explicit demand,
        // and refusing to start on a machine whose GPU we do support is the worst outcome: a
        // Windows build without the Vulkan SDK (so no Vulkan RHI) asks for Vulkan by default, and
        // would fail on a working DX12 machine with DX12 compiled in. Null is not a fallback:
        // silently running headless looks like a hang, not a diagnosis.
        const BackendType order[] = {desc.backend, BackendType::Vulkan, BackendType::DX12,
                                     BackendType::WebGPU};

        rhi::Backend* raw = nullptr;
        BackendType chosen = desc.backend;
        for (core::usize i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        {
            const BackendType candidate = order[i];
            // Slot 0 is the request, so any later repeat of it is a duplicate: skip by index
            // rather than by "have we tried one yet", which would re-report an uncompiled
            // request twice (it never gets as far as being tried).
            if (i > 0 && candidate == desc.backend)
            {
                continue;
            }
            if (!BackendCompiledIn(candidate))
            {
                // Say so, and say it for the request specifically: "not built into this binary" is
                // a different problem from "your driver refused", and only the log can tell them
                // apart after the fact.
                rhi::LogWarningf("CreateGraphicsDevice: %s backend is not compiled into this "
                                 "build%s",
                                 BackendName(candidate),
                                 candidate == desc.backend ? " (it was the requested one)" : "");
                continue;
            }
            if (TryCreateBackend(candidate, desc, raw) && raw != nullptr)
            {
                chosen = candidate;
                break;
            }
            raw = nullptr;
        }

        if (raw == nullptr)
        {
            rhi::LogError("CreateGraphicsDevice: no usable graphics backend. Requested "
                          "backend unavailable and every compiled-in fallback failed - run with "
                          "--dx12 / --vulkan / --webgpu to force one, or --null-gpu for headless.");
            return core::Err(core::ErrorCode::Unknown);
        }
        if (chosen != desc.backend)
        {
            rhi::LogWarningf("CreateGraphicsDevice: %s unavailable; using %s instead",
                             BackendName(desc.backend), BackendName(chosen));
        }

        rhi::Backend* backend =
            desc.enableValidation ? rhi::validation::CreateValidatedBackend(raw) : raw;
        return GraphicsDevice::FromBackend(backend, desc.framesInFlight, desc.requiredFeatures);
    }
}
