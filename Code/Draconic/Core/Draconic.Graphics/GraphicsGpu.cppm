// Draconic::GraphicsGpu - the `draconic.graphics.gpu` module (interface unit).
//
// The GPU-backend factory for GraphicsDevice: turns a GraphicsDeviceDesc into a
// live device on Vulkan or DX12 (validation-wrapped on request), then delegates
// the backend-agnostic bring-up to GraphicsDevice::FromBackend. Null is delegated
// to GraphicsDevice::CreateNull.
//
// The declaration here is deliberately backend-agnostic: it imports only
// draconic.graphics/core, so consumers (Application, the UI, the renderer, the
// editor) pull NO backend BMIs at compile time - the Vulkan/DX12/validation
// coupling lives entirely in the implementation unit (GraphicsGpu.cpp) and is a
// link-time dependency only. Keeping the backends out of this interface keeps every
// consumer's transitive module closure small, which matters on large apps (the
// editor) that would otherwise exhaust clang's per-TU source-location budget.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.graphics.gpu;

import draconic.foundation;
import draconic.graphics;

namespace foundation = draconic::foundation;

export namespace draconic::graphics
{
    // Create a GraphicsDevice for the requested backend. Returns an error if the
    // backend is unavailable (e.g. DX12 off this platform) or bring-up fails.
    foundation::Result<foundation::UniquePtr<GraphicsDevice>>
    CreateGraphicsDevice(const GraphicsDeviceDesc& desc);
}
