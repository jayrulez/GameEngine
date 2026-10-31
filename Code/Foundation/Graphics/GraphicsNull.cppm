// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Graphics - the `foundation.graphics.null` module.
//
// Headless GraphicsDevice factory over the Null RHI backend (no GPU). For CI,
// servers, and tests, and the reference for what a real backend provides. Kept in
// its own module so the core host (foundation.graphics) imports only the base
// RHI: importing a backend module into the core interface trips GCC's module
// reader, and this keeps the host GPU-backend-agnostic.

module;
#include "Core/Prelude.h"

export module foundation.graphics.null;

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.graphics;

namespace core = foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::graphics
{
    // Create a headless GraphicsDevice backed by the Null RHI. No Vulkan required.
    core::Result<core::UniquePtr<GraphicsDevice>>
    CreateNullGraphicsDevice(core::u32 framesInFlight = 2)
    {
        rhi::Backend* raw = nullptr;
        if (!rhi::null::CreateNullBackend(raw).IsOk())
        {
            return core::Err(core::ErrorCode::Unknown);
        }
        return GraphicsDevice::FromBackend(raw, framesInFlight);
    }
}
