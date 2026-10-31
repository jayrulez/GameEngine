// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Graphics - the `foundation.graphics.gpu` module (interface unit).
//
// The GPU-backend factory for GraphicsDevice: turns a GraphicsDeviceDesc into a
// live device on Vulkan or DX12 (validation-wrapped on request), then delegates
// the backend-agnostic bring-up to GraphicsDevice::FromBackend. Null is delegated
// to GraphicsDevice::CreateNull.
//
// The declaration here is backend-agnostic: it imports only foundation.graphics and
// core, so consumers (Application, the UI, the renderer, the editor) pull no backend
// BMIs at compile time. The Vulkan/DX12/validation coupling lives entirely in the
// implementation unit (GraphicsGpu.cpp) and is a link-time dependency only. Keeping the backends out of this interface keeps every
// consumer's transitive module closure small, which matters on large apps (the
// editor) that would otherwise exhaust clang's per-TU source-location budget.

module;
#include "Core/Prelude.h"

export module foundation.graphics.gpu;

import foundation.core;
import foundation.graphics;

namespace core = foundation::core;

export namespace foundation::graphics
{
    // Create a GraphicsDevice for the requested backend. Returns an error if the
    // backend is unavailable (e.g. DX12 off this platform) or bring-up fails.
    core::Result<core::UniquePtr<GraphicsDevice>>
    CreateGraphicsDevice(const GraphicsDeviceDesc& desc);
}
