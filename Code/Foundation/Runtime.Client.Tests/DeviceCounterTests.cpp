// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The device's counts to the profiler once a frame, on the Null device
// (which counts what it is asked to do): a frame's counters are what that frame recorded (the
// differences of the device's totals), memory is what is held now, and a device without a
// driver budget publishes none.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.profiler;
import foundation.runtime.client;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace profiler = foundation::profiler;

namespace
{
    // The value a frame's record holds for the counter named `name`, or -1 when it holds none.
    f64 CounterIn(const profiler::Profiler& p, const profiler::FrameRecord& frame, StringView name)
    {
        for (const profiler::CounterValue& value : frame.counters)
        {
            if (p.Counters()[value.counter].name.AsView() == name)
            {
                return value.value;
            }
        }
        return -1.0;
    }

    void DrawFrame(rhi::Device& device, u32 draws)
    {
        rhi::CommandPool* pool = nullptr;
        REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
        rhi::CommandEncoder* encoder = nullptr;
        REQUIRE(pool->CreateEncoder(encoder).IsOk());
        rhi::RenderPassEncoder* pass = encoder->BeginRenderPass(rhi::RenderPassDesc{});
        for (u32 i = 0; i < draws; ++i)
        {
            pass->Draw(3);
        }
        pass->End();
        device.DestroyCommandPool(pool);
    }
}

TEST_CASE("device counters: a frame's counters are what it recorded, memory what is held")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    profiler::Profiler p(DefaultAllocator());
    foundation::runtime::DeviceCounterPublisher publisher;
    p.Arm();

    DrawFrame(device, 5); // before the first publish: not any frame's
    p.BeginFrame();
    publisher.Publish(device, p);
    p.EndFrame();
    REQUIRE(p.LatestFrame() != nullptr);
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Draws")) == -1.0); // no earlier totals yet
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Buffer memory")) == doctest::Approx(0.0));

    p.BeginFrame();
    DrawFrame(device, 3);
    rhi::BufferDesc bd{};
    bd.size = 2048;
    rhi::Buffer* buffer = nullptr;
    REQUIRE(device.CreateBuffer(bd, buffer).IsOk());
    publisher.Publish(device, p);
    p.EndFrame();
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Draws")) == doctest::Approx(3.0));
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Render passes")) == doctest::Approx(1.0));
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Buffer memory")) == doctest::Approx(2048.0));

    p.BeginFrame();
    publisher.Publish(device, p);
    p.EndFrame();
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Draws")) == doctest::Approx(0.0));
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"Buffer memory")) == doctest::Approx(2048.0)); // still held
    CHECK(CounterIn(p, *p.LatestFrame(), StringView(u8"GPU memory budget")) == -1.0); // Null has no driver

    device.DestroyBuffer(buffer);
}
