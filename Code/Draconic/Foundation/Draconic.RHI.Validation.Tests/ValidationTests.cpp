#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.rhi.validation;

using namespace draconic::foundation;
using namespace draconic::rhi;

TEST_CASE("rhi.validation: wraps a backend and forwards valid calls")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());

    validation::ValidatedBackend vb(inner, DefaultAllocator());
    auto adapters = vb.EnumerateAdapters();
    REQUIRE(adapters.Size() >= 1u);

    Device* device = nullptr;
    REQUIRE(adapters[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    REQUIRE(device != nullptr);

    BufferDesc bufferDesc{};
    bufferDesc.size = 128;
    Buffer* buffer = nullptr;
    CHECK(device->CreateBuffer(bufferDesc, buffer).IsOk());
    CHECK(buffer != nullptr);
}

TEST_CASE("rhi.validation: catches invalid usage (null texture)")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());

    validation::ValidatedBackend vb(inner, DefaultAllocator());
    Device* device = nullptr;
    REQUIRE(vb.EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    // The validation layer rejects a null texture (and logs a diagnostic) instead
    // of forwarding it to the backend.
    TextureView* view = nullptr;
    CHECK_FALSE(device->CreateTextureView(nullptr, TextureViewDesc{}, view).IsOk());
    CHECK(view == nullptr);
}
