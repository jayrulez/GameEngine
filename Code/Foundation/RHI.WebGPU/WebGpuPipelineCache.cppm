// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:pipeline_cache - PipelineCache stand-in.
///
/// WebGPU has no pipeline-cache object (the browser or driver caches internally), and
/// the RHI contract treats caches as best-effort, so creation succeeds with an empty
/// cache: pipelines ignore it and GetData serves zero bytes.

module;
#include "Core/Prelude.h"

export module foundation.rhi.webgpu:pipeline_cache;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::webgpu
{
    class WebGpuPipelineCache final : public PipelineCache
    {
    public:
        u32 GetDataSize() override { return 0; }
        Status GetData(Span<u8>) override { return ErrorCode::Ok; }
    };
}
