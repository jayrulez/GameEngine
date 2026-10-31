// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>
#include <cstring>

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.rhi.validation;

using namespace foundation::core;
using namespace foundation::rhi;

TEST_CASE("rhi.validation: wraps a backend and forwards valid calls")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());

    // Heap-create through the factory: Destroy() is the wrapper's only teardown path
    // (it frees the adapter wrappers, destroys the inner backend, then itself).
    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    auto adapters = vb->EnumerateAdapters();
    REQUIRE(adapters.Size() >= 1u);

    Device* device = nullptr;
    REQUIRE(adapters[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    REQUIRE(device != nullptr);

    BufferDesc bufferDesc{};
    bufferDesc.size = 128;
    Buffer* buffer = nullptr;
    CHECK(device->CreateBuffer(bufferDesc, buffer).IsOk());
    CHECK(buffer != nullptr);

    device->DestroyBuffer(buffer);
    device->Destroy();
    vb->Destroy();
}

TEST_CASE("rhi.validation: catches invalid usage (null texture)")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());

    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    Device* device = nullptr;
    REQUIRE(vb->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    // The validation layer rejects a null texture (and logs a diagnostic) instead
    // of forwarding it to the backend.
    TextureView* view = nullptr;
    CHECK_FALSE(device->CreateTextureView(nullptr, TextureViewDesc{}, view).IsOk());
    CHECK(view == nullptr);

    device->Destroy();
    vb->Destroy();
}

namespace
{
    // Captures RHI error lines so a void-returning validation report can be asserted on.
    struct ErrorCapture
    {
        int errors = 0;
        int matching = 0;
        const char* needle = "";
        static bool Sink(void* context, bool error, const char* utf8)
        {
            auto* self = static_cast<ErrorCapture*>(context);
            if (error)
            {
                ++self->errors;
                if (std::strstr(utf8, self->needle) != nullptr)
                    ++self->matching;
            }
            return true; // swallow: the test decides what to print
        }
    };
}

TEST_CASE("rhi.validation: a fenced submit without a fence is reported on BOTH fenced overloads")
{
    // The contract: the fenced overloads require a signal fence; the plain overload is the
    // unsignalled path. Both fenced overloads must report a missing fence: submitting
    // unsignalled instead would, on a graphics queue, consume the swap-chain sync.
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());
    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    Device* device = nullptr;
    REQUIRE(vb->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics, 0);
    REQUIRE(queue != nullptr);
    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());
    CommandBuffer* cb = enc->Finish();
    REQUIRE(cb != nullptr);
    CommandBuffer* cbs[1] = {cb};

    ErrorCapture capture;
    capture.needle = "signalFence is null";
    SetLogSink(&ErrorCapture::Sink, &capture);

    queue->Submit(Span<CommandBuffer* const>(cbs, 1), nullptr, 1);
    CHECK(capture.matching == 1);
    queue->Submit(Span<CommandBuffer* const>(cbs, 1), Span<Fence* const>{}, Span<const u64>{},
                  nullptr, 1);
    CHECK(capture.matching == 2);
    // The plain overload is the unsignalled path: no fence, no report.
    queue->Submit(Span<CommandBuffer* const>(cbs, 1));
    CHECK(capture.matching == 2);

    SetLogSink(nullptr, nullptr);
    pool->DestroyEncoder(enc);
    device->DestroyCommandPool(pool);
    device->Destroy();
    vb->Destroy();
}

namespace
{
    // Captures every log line (any level) holding a needle: leak reports are warnings.
    struct LineCapture
    {
        int matching = 0;
        const char* needle = "";
        static bool Sink(void* context, bool, const char* utf8)
        {
            auto* self = static_cast<LineCapture*>(context);
            if (std::strstr(utf8, self->needle) != nullptr)
                ++self->matching;
            return true;
        }
    };
}

TEST_CASE("rhi.validation: a device destroyed with live textures names them by their creation label")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());
    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    Device* device = nullptr;
    REQUIRE(vb->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = TextureFormat::RGBA8Unorm;
    td.usage = TextureUsage::Sampled;
    td.label = u8"leak.probe.texture";
    Texture* leaked = nullptr;
    REQUIRE(device->CreateTexture(td, leaked).IsOk());
    TextureView* leakedView = nullptr;
    REQUIRE(device->CreateTextureView(leaked, TextureViewDesc{}, leakedView).IsOk());
    Texture* freed = nullptr;
    td.label = u8"freed.texture";
    REQUIRE(device->CreateTexture(td, freed).IsOk());
    device->DestroyTexture(freed); // a destroyed one is forgotten, never reported

    LineCapture capture;
    capture.needle = "leak.probe.texture";
    SetLogSink(&LineCapture::Sink, &capture);
    device->Destroy(); // the texture and its view are still alive: both named
    SetLogSink(nullptr, nullptr);
    CHECK(capture.matching == 2);
    // The leak was on purpose; hand the two null objects back to their allocator through a
    // sibling null device so the sanitizer lanes stay quiet.
    Device* janitor = nullptr;
    REQUIRE(inner->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, janitor).IsOk());
    janitor->DestroyTextureView(leakedView);
    janitor->DestroyTexture(leaked);
    janitor->Destroy();
    vb->Destroy();
}

// A stage runs the entry it names, else its module's (ShaderModuleDesc::entryPoint). With neither,
// no backend can build it, so the validation layer refuses it before the driver does.
TEST_CASE("rhi.validation: a pipeline stage with no entry point anywhere is refused")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());
    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    Device* device = nullptr;
    REQUIRE(vb->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    const u8 code[4] = {1, 2, 3, 4};
    ShaderModuleDesc named{};
    named.code = Span<const u8>(code, 4);
    named.entryPoint = u8"VSMain";
    ShaderModuleDesc unnamed{};
    unnamed.code = Span<const u8>(code, 4);
    ShaderModule* withEntry = nullptr;
    ShaderModule* withoutEntry = nullptr;
    REQUIRE(device->CreateShaderModule(named, withEntry).IsOk());
    REQUIRE(device->CreateShaderModule(unnamed, withoutEntry).IsOk());
    CHECK(withEntry->entryPoint.AsView() == StringView(u8"VSMain"));

    PipelineLayout* layout = nullptr;
    REQUIRE(device->CreatePipelineLayout(PipelineLayoutDesc{}, layout).IsOk());

    const auto build = [&](ProgrammableStage stage) -> bool
    {
        RenderPipelineDesc desc{};
        desc.layout = layout;
        desc.vertex.shader = stage;
        RenderPipeline* pipeline = nullptr;
        const bool ok = device->CreateRenderPipeline(desc, pipeline).IsOk();
        if (pipeline != nullptr)
        {
            device->DestroyRenderPipeline(pipeline);
        }
        return ok;
    };
    CHECK(build(StageOf(withEntry, ShaderStage::Vertex)));
    CHECK(build(ProgrammableStage{withoutEntry, u8"main", ShaderStage::Vertex}));
    CHECK_FALSE(build(StageOf(withoutEntry, ShaderStage::Vertex)));

    device->DestroyPipelineLayout(layout);
    device->DestroyShaderModule(withEntry);
    device->DestroyShaderModule(withoutEntry);
    device->Destroy();
    vb->Destroy();
}

TEST_CASE("rhi.validation: pipeline statistics queries are one at a time, on a statistics set")
{
    // A statistics query cannot nest, must be on a PipelineStatistics set, ends the query it
    // began, and is closed before the encoder finishes; the layer reports each, and passes the
    // device's answers (here Null's: no statistics, no calibration) through.
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());
    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    Device* device = nullptr;
    REQUIRE(vb->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    CHECK(device->QueryResultSize(QueryType::Timestamp) == 8u);
    CHECK(device->QueryResultSize(QueryType::PipelineStatistics) == 0u);
    CHECK(device->DecodePipelineStatistics(nullptr).inputVertices == 0u);
    u64 gpu = 1, cpu = 2;
    CHECK_FALSE(device->GetQueue(QueueType::Graphics, 0)->CalibrateTimestamps(gpu, cpu));
    CHECK(gpu == 1u);

    QuerySetDesc qd{};
    qd.type = QueryType::PipelineStatistics;
    qd.count = 2;
    QuerySet* stats = nullptr;
    REQUIRE(device->CreateQuerySet(qd, stats).IsOk());
    qd.type = QueryType::Timestamp;
    QuerySet* timestamps = nullptr;
    REQUIRE(device->CreateQuerySet(qd, timestamps).IsOk());
    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());

    ErrorCapture capture;
    SetLogSink(&ErrorCapture::Sink, &capture);
    enc->BeginPipelineStatisticsQuery(timestamps, 0); // the wrong kind of set
    CHECK(capture.errors == 1);
    enc->BeginPipelineStatisticsQuery(stats, 0);
    CHECK(capture.errors == 1); // a good begin
    enc->BeginPipelineStatisticsQuery(stats, 1); // nested
    CHECK(capture.errors == 2);
    enc->EndPipelineStatisticsQuery(stats, 1); // not the open one
    CHECK(capture.errors == 3);
    enc->EndPipelineStatisticsQuery(stats, 0);
    CHECK(capture.errors == 3); // a good end
    enc->BeginPipelineStatisticsQuery(stats, 1);
    CommandBuffer* cb = enc->Finish(); // left open
    CHECK(capture.errors == 4);
    CHECK(cb != nullptr);
    SetLogSink(nullptr, nullptr);

    pool->DestroyEncoder(enc);
    device->DestroyCommandPool(pool);
    device->DestroyQuerySet(stats);
    device->DestroyQuerySet(timestamps);
    device->Destroy();
    vb->Destroy();
}

TEST_CASE("rhi.validation: a view in a texture's sRGB or UNORM twin needs the texture to declare it")
{
    Backend* inner = nullptr;
    REQUIRE(null::CreateNullBackend(inner).IsOk());
    Backend* vb = validation::CreateValidatedBackend(inner, DefaultAllocator());
    REQUIRE(vb != nullptr);
    Device* device = nullptr;
    REQUIRE(vb->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    CHECK(SrgbTwin(TextureFormat::RGBA8UnormSrgb) == TextureFormat::RGBA8Unorm);
    CHECK(SrgbTwin(TextureFormat::BGRA8Unorm) == TextureFormat::BGRA8UnormSrgb);
    CHECK(SrgbTwin(TextureFormat::RGBA16Float) == TextureFormat::Undefined);

    // Without the declaration, a UNORM view of an sRGB texture is refused.
    TextureDesc plain = TextureDesc::RenderTarget(TextureFormat::RGBA8UnormSrgb, 4, 4);
    Texture* texture = nullptr;
    REQUIRE(device->CreateTexture(plain, texture).IsOk());
    TextureViewDesc unorm{};
    unorm.format = TextureFormat::RGBA8Unorm;
    TextureView* view = nullptr;
    CHECK_FALSE(device->CreateTextureView(texture, unorm, view).IsOk());
    CHECK(view == nullptr);
    device->DestroyTexture(texture);

    // Declared, it is made, and so is a view in the texture's own format.
    TextureDesc twin = plain;
    twin.alternateViewFormat = TextureFormat::RGBA8Unorm;
    REQUIRE(device->CreateTexture(twin, texture).IsOk());
    CHECK(device->CreateTextureView(texture, unorm, view).IsOk());
    TextureView* own = nullptr;
    CHECK(device->CreateTextureView(texture, TextureViewDesc{}, own).IsOk());
    device->DestroyTextureView(view);
    device->DestroyTextureView(own);
    device->DestroyTexture(texture);

    // A destroyed texture is reported, and its freed desc is not read for the twin check.
    REQUIRE(device->CreateTexture(plain, texture).IsOk());
    device->DestroyTexture(texture);
    TextureView* stale = nullptr;
    (void)device->CreateTextureView(texture, unorm, stale); // no read of freed memory (ASAN checks)
    if (stale != nullptr)
        device->DestroyTextureView(stale);

    // An alternate that is not the twin is refused at creation.
    TextureDesc wrong = plain;
    wrong.alternateViewFormat = TextureFormat::RGBA16Float;
    CHECK_FALSE(device->CreateTexture(wrong, texture).IsOk());

    device->Destroy();
    vb->Destroy();
}
