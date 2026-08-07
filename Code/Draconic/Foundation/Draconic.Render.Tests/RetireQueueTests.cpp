// GpuRetireQueue: frames-in-flight deferred GPU destruction (the web-safe replacement
// for grow-path WaitIdle). Null-RHI: verifies aging (freed only after framesInFlight+1
// ticks), typed retire, and Flush.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.render;

using namespace draconic::foundation;
using namespace draconic::render;
namespace rhi = draconic::rhi;

TEST_CASE("render.retire: entries age out after framesInFlight + 1 ticks")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuRetireQueue queue;
    queue.Initialize(&device, /*framesInFlight*/ 2);

    rhi::BufferDesc bd{};
    bd.size = 256;
    bd.usage = rhi::BufferUsage::Storage;
    rhi::Buffer* buffer = nullptr;
    REQUIRE(device.CreateBuffer(bd, buffer).IsOk());
    rhi::TextureDesc td = rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8Unorm, 4, 4);
    rhi::Texture* texture = nullptr;
    REQUIRE(device.CreateTexture(td, texture).IsOk());
    rhi::TextureView* view = nullptr;
    REQUIRE(device.CreateTextureView(texture, rhi::TextureViewDesc{}, view).IsOk());

    queue.Retire(view);
    queue.Retire(texture);
    queue.Retire(buffer);
    CHECK(queue.PendingCount() == 3u);

    // framesInFlight=2 -> age 3: two ticks keep everything, the third frees.
    queue.Tick();
    CHECK(queue.PendingCount() == 3u);
    queue.Tick();
    CHECK(queue.PendingCount() == 3u);
    queue.Tick();
    CHECK(queue.PendingCount() == 0u);

    // Null retires are no-ops; Flush drains immediately.
    queue.Retire(static_cast<rhi::Buffer*>(nullptr));
    CHECK(queue.PendingCount() == 0u);
    rhi::Buffer* another = nullptr;
    REQUIRE(device.CreateBuffer(bd, another).IsOk());
    queue.Retire(another);
    CHECK(queue.PendingCount() == 1u);
    queue.Flush();
    CHECK(queue.PendingCount() == 0u);
}
