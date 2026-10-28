// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::RHI.TestSupport - `foundation.rhi.testsupport`.
//
// A tiny, backend-agnostic substrate for offscreen render tests: create a device, and read a
// rendered color target back to a CPU-side image you can probe. Extracted from the inline copies in
// VG.Backend.Tests and Render.Backend.Tests (each rolled its own make-device + copy-to-buffer + map).
//
// STRUCTURAL probes only: callers assert PROPERTIES of the pixels (edge coverage, luma splits, color
// agreement), never diff against stored golden images - so there is no cross-driver/GPU drift. This
// module owns only the reusable plumbing (device, readback, pixel access); each test keeps its own
// render setup (VG builds a pass; the 3D tests drive RenderFrame) and its own per-feature assertions.

module;
#include "Core/Prelude.h"

export module foundation.rhi.testsupport;

import foundation.core;
import foundation.rhi;
import foundation.vfs;

using namespace foundation::core;

export namespace foundation::rhi::testsupport
{
    namespace rhi = foundation::rhi;

    // The engine data root, found the way every executable finds it (the Data/.dataroot walk
    // from the test binary under Bin/). Empty = not found (the caller REQUIREs or skips).
    [[nodiscard]] inline StringView DataRoot()
    {
        static const String root = foundation::vfs::FindDataRoot();
        return root.AsView();
    }

    // A process-wide mount over the data root - what a ShaderSystemHost or a file provider reads
    // engine shaders through in a test, exactly as the application's subsystems do.
    [[nodiscard]] inline foundation::vfs::IFileSystem& DataFileSystem()
    {
        static foundation::vfs::NativeFileSystem fs(DataRoot(), DefaultAllocator());
        return fs;
    }

    // A data-relative path resolved against the root ("Engine/Fonts/x.ttf" -> absolute).
    [[nodiscard]] inline String DataPath(StringView relative)
    {
        return foundation::vfs::DataPath(DataRoot(), relative);
    }

    // Create a device from a backend's first adapter. Returns null (test should skip) when the
    // backend has no adapter or device creation fails - e.g. no GPU in a headless CI box.
    [[nodiscard]] inline rhi::Device* MakeTestDevice(rhi::Backend* backend)
    {
        if (backend == nullptr || backend->EnumerateAdapters().IsEmpty())
        {
            return nullptr;
        }
        rhi::Device* device = nullptr;
        if (!backend->EnumerateAdapters()[0]->CreateDevice(rhi::DeviceDesc{}, device).IsOk())
        {
            return nullptr;
        }
        return device;
    }

    // A CPU-side RGBA8 image read back from a render target. Tightly packed (width*height*4), so the
    // 256-byte row alignment of the readback buffer never leaks into the probe math.
    struct CapturedImage
    {
        bool valid = false;
        u32 width = 0;
        u32 height = 0;
        Array<u8> rgba;

        [[nodiscard]] const u8* At(u32 x, u32 y) const
        {
            return rgba.Data() + (static_cast<usize>(y) * width + x) * 4;
        }
        // A cheap luma proxy in [0, 765]: r + g + b. Enough to separate dark/mid/bright for the
        // structural probes; not a perceptual metric.
        [[nodiscard]] u32 Luma(u32 x, u32 y) const
        {
            const u8* p = At(x, y);
            return static_cast<u32>(p[0]) + p[1] + p[2];
        }
        // Count pixels whose RGBA satisfies pred(const u8* rgba).
        template <typename Pred>
        [[nodiscard]] u32 CountWhere(Pred&& pred) const
        {
            u32 n = 0;
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    if (pred(At(x, y)))
                    {
                        ++n;
                    }
                }
            }
            return n;
        }
    };

    // Read back a color target that has ALREADY been rendered and left in ResourceState::CopySrc.
    // Owns a fresh command buffer for the copy + a GpuToCpu staging buffer, blocks on a fence, and
    // unpacks the 256-aligned rows into a tightly-packed CapturedImage. Decoupled from the render
    // command stream (the target is a persistent texture), so a caller renders however it likes and
    // then calls this. Returns {valid=false} on any failure.
    [[nodiscard]] inline CapturedImage Readback(rhi::Device& device, rhi::Texture* colorTarget,
                                                u32 width, u32 height)
    {
        CapturedImage out;
        if (colorTarget == nullptr || width == 0 || height == 0)
        {
            return out;
        }
        const u32 bytesPerRow = (width * 4u + 255u) & ~255u; // WebGPU/Vulkan require 256-byte rows

        rhi::BufferDesc rb{};
        rb.size = static_cast<u64>(bytesPerRow) * height;
        rb.usage = rhi::BufferUsage::CopyDst;
        rb.memory = rhi::MemoryLocation::GpuToCpu;
        rhi::Buffer* readback = nullptr;
        if (!device.CreateBuffer(rb, readback).IsOk())
        {
            return out;
        }

        rhi::CommandPool* pool = nullptr;
        rhi::Fence* fence = nullptr;
        rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
        if (queue == nullptr || !device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk() ||
            !device.CreateFence(0, fence).IsOk())
        {
            if (fence != nullptr)
            {
                device.DestroyFence(fence);
            }
            if (pool != nullptr)
            {
                device.DestroyCommandPool(pool);
            }
            device.DestroyBuffer(readback);
            return out;
        }

        rhi::CommandEncoder* encoder = nullptr;
        if (pool->CreateEncoder(encoder).IsOk())
        {
            rhi::BufferTextureCopyRegion region;
            region.bytesPerRow = bytesPerRow;
            region.rowsPerImage = height;
            region.textureExtent = rhi::Extent3D{width, height, 1};
            encoder->CopyTextureToBuffer(colorTarget, readback, region);
            rhi::CommandBuffer* cmd = encoder->Finish();
            if (cmd != nullptr)
            {
                rhi::CommandBuffer* list[] = {cmd};
                queue->Submit(Span<rhi::CommandBuffer* const>(list, 1), fence, 1);
                fence->Wait(1, ~0ull);
                if (const u8* mapped = static_cast<const u8*>(readback->Map()))
                {
                    out.width = width;
                    out.height = height;
                    out.rgba.Resize(static_cast<usize>(width) * height * 4);
                    for (u32 y = 0; y < height; ++y)
                    {
                        const u8* src = mapped + static_cast<usize>(y) * bytesPerRow;
                        u8* dst = out.rgba.Data() + static_cast<usize>(y) * width * 4;
                        for (u32 x = 0; x < width * 4u; ++x)
                        {
                            dst[x] = src[x];
                        }
                    }
                    readback->Unmap();
                    out.valid = true;
                }
            }
        }

        device.WaitIdle();
        if (encoder != nullptr)
        {
            pool->DestroyEncoder(encoder); // WaitIdle'd: safe (DestroyCommandPool does not free it)
        }
        device.DestroyFence(fence);
        device.DestroyCommandPool(pool);
        device.DestroyBuffer(readback);
        return out;
    }
}
