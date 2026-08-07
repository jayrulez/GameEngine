#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;

using namespace draconic::foundation;
using namespace draconic::rhi;

TEST_CASE("rhi.null: backend enumerates an adapter and creates a device")
{
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    REQUIRE(backend != nullptr);
    CHECK(backend->isInitialized);

    auto adapters = backend->EnumerateAdapters();
    REQUIRE(adapters.Size() == 1u);

    const AdapterInfo info = adapters[0]->Info();
    CHECK(info.name == u8"Null Device");
    CHECK(info.type == AdapterType::Cpu);

    Device* device = nullptr;
    REQUIRE(adapters[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    REQUIRE(device != nullptr);

    backend->Destroy();
}

TEST_CASE("rhi.null: device creates resources and a mappable buffer")
{
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    BufferDesc bufferDesc{};
    bufferDesc.size = 256;
    Buffer* buffer = nullptr;
    REQUIRE(device->CreateBuffer(bufferDesc, buffer).IsOk());
    REQUIRE(buffer != nullptr);
    CHECK(buffer->Map() != nullptr); // null backend backs it with host memory
    buffer->Unmap();

    Texture* texture = nullptr;
    CHECK(device->CreateTexture(TextureDesc{}, texture).IsOk());
    CHECK(texture != nullptr);

    // Unsupported extensions degrade, they don't crash.
    MeshPipeline* mesh = nullptr;
    CHECK(device->CreateMeshPipeline(MeshPipelineDesc{}, mesh).Code() == ErrorCode::NotSupported);

    backend->Destroy();
}

TEST_CASE("rhi: texture views carry unique monotonic ids (address-reuse guard)")
{
    // Caches keyed by TextureView* validate uniqueId on every hit: a destroyed view's
    // address can be reused by the very next allocation, and a stale cached descriptor
    // then samples a dead image view (the UI-render-target device-lost). Ids must be
    // unique across create/destroy/create cycles - an address is never enough.
    Backend* backend = nullptr;
    REQUIRE(null::CreateNullBackend(backend).IsOk());
    REQUIRE(backend != nullptr);
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    Texture* texture = nullptr;
    REQUIRE(device->CreateTexture(TextureDesc{}, texture).IsOk());

    TextureView* a = nullptr;
    REQUIRE(device->CreateTextureView(texture, TextureViewDesc{}, a).IsOk());
    const draconic::foundation::u64 idA = a->uniqueId;
    CHECK(idA != 0u);

    TextureView* b = nullptr;
    REQUIRE(device->CreateTextureView(texture, TextureViewDesc{}, b).IsOk());
    CHECK(b->uniqueId != idA);

    // Destroy + recreate: even if the allocator reuses the address, the id is fresh.
    device->DestroyTextureView(a);
    TextureView* c = nullptr;
    REQUIRE(device->CreateTextureView(texture, TextureViewDesc{}, c).IsOk());
    CHECK(c->uniqueId != idA);
    CHECK(c->uniqueId != b->uniqueId);

    backend->Destroy();
}
