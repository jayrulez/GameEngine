// WebGPU backend bring-up tests. ENVIRONMENT-DEPENDENT by nature (they need the
// wgpu-native sidecar AND a GPU the runtime can drive) - when the backend cannot
// initialize, the suite reports that once and passes vacuously rather than failing
// a GPU-less machine, mirroring how Vulkan/DX12 stay sample-verified.
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.webgpu;

using namespace draconic::foundation;
using namespace draconic::rhi;

namespace
{
    Backend* TryCreateBackend()
    {
        Backend* backend = nullptr;
        if (!webgpu::CreateBackend(webgpu::WebGpuBackendDesc{}, backend).IsOk())
        {
            return nullptr;
        }
        if (backend->EnumerateAdapters().IsEmpty())
        {
            backend->Destroy();
            return nullptr;
        }
        return backend;
    }
}

TEST_CASE("rhi.webgpu: sidecar backend enumerates adapters and creates a live device")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        MESSAGE("wgpu-native sidecar or GPU unavailable - webgpu backend tests skipped");
        return;
    }
    CHECK(backend->isInitialized);

    // Adapter info is real hardware data - shape checks only.
    auto adapters = backend->EnumerateAdapters();
    const AdapterInfo info = adapters[0]->Info();
    CHECK(!info.name.IsEmpty());
    CHECK(info.supportedFeatures.maxBindGroups >= 4u);

    Device* device = nullptr;
    REQUIRE(adapters[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    REQUIRE(device != nullptr);
    CHECK(device->type == DeviceType::WebGPU);
    CHECK(!device->IsLost());

    // One WebGPU queue behind all three RHI queue types.
    Queue* graphics = device->GetQueue(QueueType::Graphics);
    REQUIRE(graphics != nullptr);
    CHECK(graphics->queueType == QueueType::Graphics);
    CHECK(device->GetQueue(QueueType::Compute) != nullptr);
    CHECK(device->GetQueueCount(QueueType::Graphics) == 1u);

    device->WaitIdle();
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: fences signal through empty submissions")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return; // reported once by the first case
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CHECK(fence->CompletedValue() == 0u);

    Queue* queue = device->GetQueue(QueueType::Graphics);
    queue->Submit(Span<CommandBuffer* const>{}, fence, 7);
    CHECK(fence->Wait(7, 0));
    CHECK(fence->CompletedValue() == 7u);

    device->DestroyFence(fence);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: unimplemented stages fail honestly, extensions unsupported")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    // Buffers are implemented now - an EMPTY desc must fail validation, not
    // hand back wgpu's "invalid object".
    Buffer* buffer = nullptr;
    CHECK(device->CreateBuffer(BufferDesc{}, buffer).Code() == ErrorCode::InvalidArgument);
    CHECK(buffer == nullptr);

    // Pipeline statistics have no WebGPU shape - honest NotSupported.
    QuerySetDesc statisticsDesc;
    statisticsDesc.type = QueryType::PipelineStatistics;
    statisticsDesc.count = 1;
    QuerySet* statistics = nullptr;
    CHECK(device->CreateQuerySet(statisticsDesc, statistics).Code() ==
          ErrorCode::NotSupported);

    MeshPipeline* mesh = nullptr;
    CHECK(device->CreateMeshPipeline(MeshPipelineDesc{}, mesh).Code() ==
          ErrorCode::NotSupported);

    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: a missing sidecar fails with NotFound, not a crash")
{
    Backend* backend = nullptr;
    webgpu::WebGpuBackendDesc desc;
    desc.libraryPathOverride = u8"definitely_not_wgpu_native.so";
    // The override misses, and the fallback chain may still find the vendored lib -
    // either a clean failure or a working backend is acceptable; never a crash.
    const Status status = webgpu::CreateBackend(desc, backend);
    if (status.IsOk())
    {
        backend->Destroy();
    }
    else
    {
        CHECK(backend == nullptr);
    }
}

TEST_CASE("rhi.webgpu: resources - buffer map emulation, texture + view, sampler, WGSL shader")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    // CpuToGpu buffer: Map hands out the CPU shadow, Unmap flushes via WriteBuffer.
    BufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
    bufferDesc.memory = MemoryLocation::CpuToGpu;
    Buffer* buffer = nullptr;
    REQUIRE(device->CreateBuffer(bufferDesc, buffer).IsOk());
    void* mapped = buffer->Map();
    REQUIRE(mapped != nullptr);
    MemSet(mapped, 0xAB, 256);
    buffer->Unmap(); // queue-ordered upload; GPU validation would log on error
    device->WaitIdle();
    CHECK(!device->IsLost());

    // GpuOnly: no host view, by contract.
    BufferDesc gpuOnly;
    gpuOnly.size = 64;
    gpuOnly.usage = BufferUsage::Storage;
    gpuOnly.memory = MemoryLocation::GpuOnly;
    Buffer* deviceLocal = nullptr;
    REQUIRE(device->CreateBuffer(gpuOnly, deviceLocal).IsOk());
    CHECK(deviceLocal->Map() == nullptr);

    // Texture + a full-resource view.
    Texture* texture = nullptr;
    REQUIRE(device
                ->CreateTexture(TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 64, 64),
                                texture)
                .IsOk());
    TextureViewDesc viewDesc;
    viewDesc.format = TextureFormat::RGBA8Unorm;
    TextureView* view = nullptr;
    REQUIRE(device->CreateTextureView(texture, viewDesc, view).IsOk());
    CHECK(view->texture == texture);

    // A format WebGPU does not have fails honestly.
    Texture* unsupported = nullptr;
    CHECK(device
              ->CreateTexture(TextureDesc::RenderTarget(TextureFormat::RGBA16Unorm, 4, 4),
                              unsupported)
              .Code() == ErrorCode::NotSupported);

    // Sampler, including the anisotropy-requires-linear clamp.
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Nearest;
    samplerDesc.maxAnisotropy = 8; // invalid with nearest - backend clamps to 1
    Sampler* sampler = nullptr;
    REQUIRE(device->CreateSampler(samplerDesc, sampler).IsOk());

    // WGSL shader module (the non-SPIR-V ingestion branch; SPIR-V rides the samples).
    const char8_t* wgsl =
        u8"@vertex fn main() -> @builtin(position) vec4f { return vec4f(0.0); }";
    ShaderModuleDesc moduleDesc;
    moduleDesc.code = Span<const u8>(reinterpret_cast<const u8*>(wgsl),
                                     StringView(wgsl).Size());
    ShaderModule* shaderModule = nullptr;
    REQUIRE(device->CreateShaderModule(moduleDesc, shaderModule).IsOk());

    device->DestroyShaderModule(shaderModule);
    device->DestroySampler(sampler);
    device->DestroyTextureView(view);
    device->DestroyTexture(texture);
    device->DestroyBuffer(deviceLocal);
    device->DestroyBuffer(buffer);
    device->WaitIdle();
    CHECK(!device->IsLost());
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: bind groups + pipelines - the DXC shift scheme end-to-end")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    // Layout with all three shift classes: CBV (0), SRV (+1000), sampler (+3000).
    const BindGroupLayoutEntry entries[] = {
        BindGroupLayoutEntry::UniformBuffer(0, ShaderStage::Vertex | ShaderStage::Fragment),
        BindGroupLayoutEntry::SampledTexture(0, ShaderStage::Fragment),
        BindGroupLayoutEntry::Sampler(0, ShaderStage::Fragment),
    };
    BindGroupLayoutDesc layoutDesc;
    layoutDesc.entries = Span<const BindGroupLayoutEntry>(entries, 3);
    BindGroupLayout* layout = nullptr;
    REQUIRE(device->CreateBindGroupLayout(layoutDesc, layout).IsOk());
    CHECK(layout->Entries().Size() == 3u);

    // Resources to bind.
    BufferDesc uboDesc;
    uboDesc.size = 16;
    uboDesc.usage = BufferUsage::Uniform;
    uboDesc.memory = MemoryLocation::CpuToGpu;
    Buffer* ubo = nullptr;
    REQUIRE(device->CreateBuffer(uboDesc, ubo).IsOk());
    Texture* texture = nullptr;
    TextureDesc texDesc = TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 4, 4);
    texDesc.usage = TextureUsage::Sampled | TextureUsage::CopyDst;
    REQUIRE(device->CreateTexture(texDesc, texture).IsOk());
    TextureViewDesc viewDesc;
    viewDesc.format = TextureFormat::RGBA8Unorm;
    TextureView* view = nullptr;
    REQUIRE(device->CreateTextureView(texture, viewDesc, view).IsOk());
    Sampler* sampler = nullptr;
    REQUIRE(device->CreateSampler(SamplerDesc{}, sampler).IsOk());

    const BindGroupEntry groupEntries[] = {
        BindGroupEntry::BufferEntry(ubo, 0, 16),
        BindGroupEntry::TextureEntry(view),
        BindGroupEntry::SamplerEntry(sampler),
    };
    BindGroupDesc groupDesc;
    groupDesc.layout = layout;
    groupDesc.entries = Span<const BindGroupEntry>(groupEntries, 3);
    BindGroup* group = nullptr;
    REQUIRE(device->CreateBindGroup(groupDesc, group).IsOk());
    CHECK(group->Layout() == layout);

    // Pipeline layout + a render pipeline whose WGSL uses the SHIFTED binding
    // numbers (the compact WebGPU profile: SRV +100, sampler +300) - if the shift
    // scheme mismatched the layout, creation would fail.
    PipelineLayoutDesc plDesc;
    BindGroupLayout* layouts[] = {layout};
    plDesc.bindGroupLayouts = Span<BindGroupLayout* const>(layouts, 1);
    PipelineLayout* pipelineLayout = nullptr;
    REQUIRE(device->CreatePipelineLayout(plDesc, pipelineLayout).IsOk());

    const char8_t* wgsl =
        u8"@group(0) @binding(0) var<uniform> tintUniform : vec4f;\n"
        u8"@group(0) @binding(100) var sceneTexture : texture_2d<f32>;\n"
        u8"@group(0) @binding(300) var sceneSampler : sampler;\n"
        u8"@vertex fn vertexMain(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f\n"
        u8"{ return vec4f(f32(i), 0.0, 0.0, 1.0); }\n"
        u8"@fragment fn fragmentMain() -> @location(0) vec4f\n"
        u8"{ return textureSampleLevel(sceneTexture, sceneSampler, vec2f(0.5), 0.0)\n"
        u8"    * tintUniform; }\n";
    ShaderModuleDesc moduleDesc;
    moduleDesc.code =
        Span<const u8>(reinterpret_cast<const u8*>(wgsl), StringView(wgsl).Size());
    ShaderModule* shaderModule = nullptr;
    REQUIRE(device->CreateShaderModule(moduleDesc, shaderModule).IsOk());

    RenderPipelineDesc rpDesc;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.shader = ProgrammableStage{shaderModule, u8"vertexMain", ShaderStage::Vertex};
    ColorTargetState target;
    target.format = TextureFormat::RGBA8Unorm;
    target.blend = BlendState::AlphaBlend();
    FragmentState fragment;
    fragment.shader = ProgrammableStage{shaderModule, u8"fragmentMain", ShaderStage::Fragment};
    fragment.targets = Span<const ColorTargetState>(&target, 1);
    rpDesc.fragment = fragment;
    RenderPipeline* renderPipeline = nullptr;
    REQUIRE(device->CreateRenderPipeline(rpDesc, renderPipeline).IsOk());

    // Wireframe has no WebGPU shape - honest NotSupported.
    RenderPipelineDesc wireframeDesc = rpDesc;
    wireframeDesc.primitive.fillMode = FillMode::Wireframe;
    RenderPipeline* wireframe = nullptr;
    CHECK(device->CreateRenderPipeline(wireframeDesc, wireframe).Code() ==
          ErrorCode::NotSupported);

    // Compute pipeline.
    const char8_t* computeWgsl =
        u8"@compute @workgroup_size(1) fn computeMain() { }";
    ShaderModuleDesc computeModuleDesc;
    computeModuleDesc.code = Span<const u8>(reinterpret_cast<const u8*>(computeWgsl),
                                            StringView(computeWgsl).Size());
    ShaderModule* computeModule = nullptr;
    REQUIRE(device->CreateShaderModule(computeModuleDesc, computeModule).IsOk());
    PipelineLayoutDesc emptyLayoutDesc;
    PipelineLayout* emptyLayout = nullptr;
    REQUIRE(device->CreatePipelineLayout(emptyLayoutDesc, emptyLayout).IsOk());
    ComputePipelineDesc cpDesc;
    cpDesc.layout = emptyLayout;
    cpDesc.compute = ProgrammableStage{computeModule, u8"computeMain", ShaderStage::Compute};
    ComputePipeline* computePipeline = nullptr;
    REQUIRE(device->CreateComputePipeline(cpDesc, computePipeline).IsOk());

    device->DestroyComputePipeline(computePipeline);
    device->DestroyPipelineLayout(emptyLayout);
    device->DestroyShaderModule(computeModule);
    device->DestroyRenderPipeline(renderPipeline);
    device->DestroyShaderModule(shaderModule);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyBindGroup(group);
    device->DestroySampler(sampler);
    device->DestroyTextureView(view);
    device->DestroyTexture(texture);
    device->DestroyBuffer(ubo);
    device->WaitIdle();
    CHECK(!device->IsLost());
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: encode + submit + readback - a full GPU round trip")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    // Offscreen 4x4 target, cleared to a known color by a real render pass.
    TextureDesc targetDesc = TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 4, 4);
    targetDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    Texture* target = nullptr;
    REQUIRE(device->CreateTexture(targetDesc, target).IsOk());
    TextureViewDesc viewDesc;
    viewDesc.format = TextureFormat::RGBA8Unorm;
    TextureView* view = nullptr;
    REQUIRE(device->CreateTextureView(target, viewDesc, view).IsOk());

    // Readback buffer: 4 rows x 256 bytes (WebGPU's bytesPerRow alignment).
    BufferDesc readbackDesc;
    readbackDesc.size = 4 * 256;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    RenderPassDesc pass;
    ColorAttachment color;
    color.view = view;
    color.loadOp = LoadOp::Clear;
    color.storeOp = StoreOp::Store;
    color.clearValue = ClearColor{1.0f, 0.0f, 0.0f, 1.0f}; // pure red
    pass.colorAttachments.Add(color);
    RenderPassEncoder* renderPass = encoder->BeginRenderPass(pass);
    REQUIRE(renderPass != nullptr);
    renderPass->End();

    BufferTextureCopyRegion region;
    region.bytesPerRow = 256;
    region.rowsPerImage = 4;
    region.textureExtent = Extent3D{4, 4, 1};
    encoder->CopyTextureToBuffer(target, readback, region);

    CommandBuffer* commandBuffer = encoder->Finish();
    REQUIRE(commandBuffer != nullptr);

    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffers[] = {commandBuffer};
    device->GetQueue(QueueType::Graphics)
        ->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    // Map the readback and verify the clear color survived the round trip.
    const u8* pixels = static_cast<const u8*>(readback->Map());
    REQUIRE(pixels != nullptr);
    CHECK(pixels[0] == 255); // R
    CHECK(pixels[1] == 0);   // G
    CHECK(pixels[2] == 0);   // B
    CHECK(pixels[3] == 255); // A
    CHECK(pixels[256 * 3 + 0] == 255); // last row, first pixel
    readback->Unmap();

    CHECK(!device->IsLost());
    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(readback);
    device->DestroyTextureView(view);
    device->DestroyTexture(target);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: transfer batch uploads verify through GPU readback")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Transfer);
    TransferBatch* batch = nullptr;
    REQUIRE(queue->CreateTransferBatch(batch).IsOk());

    // Batch-write a GPU-only buffer, then copy it into a readback buffer.
    BufferDesc gpuDesc;
    gpuDesc.size = 64;
    gpuDesc.usage = BufferUsage::Storage | BufferUsage::CopySrc | BufferUsage::CopyDst;
    gpuDesc.memory = MemoryLocation::GpuOnly;
    Buffer* gpuBuffer = nullptr;
    REQUIRE(device->CreateBuffer(gpuDesc, gpuBuffer).IsOk());

    u8 pattern[64];
    for (u32 i = 0; i < 64; ++i)
    {
        pattern[i] = static_cast<u8>(i * 3);
    }
    batch->WriteBuffer(gpuBuffer, 0, Span<const u8>(pattern, 64));

    // Batch-write a texture too (one 4x4 RGBA mip).
    TextureDesc texDesc;
    texDesc.format = TextureFormat::RGBA8Unorm;
    texDesc.width = 4;
    texDesc.height = 4;
    texDesc.usage = TextureUsage::CopyDst | TextureUsage::CopySrc;
    Texture* texture = nullptr;
    REQUIRE(device->CreateTexture(texDesc, texture).IsOk());
    u8 texels[4 * 4 * 4];
    for (u32 i = 0; i < sizeof(texels); ++i)
    {
        texels[i] = static_cast<u8>(255 - i);
    }
    TextureDataLayout layout;
    layout.bytesPerRow = 16;
    layout.rowsPerImage = 4;
    batch->WriteTexture(texture, Span<const u8>(texels, sizeof(texels)), layout,
                        Extent3D{4, 4, 1});

    REQUIRE(batch->Submit().IsOk()); // blocking, Vulkan-batch semantics

    // Read both back.
    BufferDesc readDesc;
    readDesc.size = 4 * 256;
    readDesc.usage = BufferUsage::CopyDst;
    readDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readDesc, readback).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    encoder->CopyBufferToBuffer(gpuBuffer, 0, readback, 0, 64);
    CommandBuffer* commandBuffer = encoder->Finish();
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffers[] = {commandBuffer};
    device->GetQueue(QueueType::Graphics)
        ->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u8* bytes = static_cast<const u8*>(readback->Map());
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 0);
    CHECK(bytes[21] == static_cast<u8>(21 * 3));
    CHECK(bytes[63] == static_cast<u8>(63 * 3));
    readback->Unmap();

    // Texture readback through the second copy path.
    BufferTextureCopyRegion region;
    region.bytesPerRow = 256;
    region.rowsPerImage = 4;
    region.textureExtent = Extent3D{4, 4, 1};
    encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    encoder->CopyTextureToBuffer(texture, readback, region);
    commandBuffer = encoder->Finish();
    CommandBuffer* second[] = {commandBuffer};
    device->GetQueue(QueueType::Graphics)
        ->Submit(Span<CommandBuffer* const>(second, 1), fence, 2);
    REQUIRE(fence->Wait(2, ~0ull));
    bytes = static_cast<const u8*>(readback->Map());
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 255);                 // first texel byte
    CHECK(bytes[15] == static_cast<u8>(255 - 15)); // last byte of row 0
    CHECK(bytes[256 + 0] == static_cast<u8>(255 - 16)); // row 1 starts at bytesPerRow
    readback->Unmap();

    queue->DestroyTransferBatch(batch);
    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(readback);
    device->DestroyTexture(texture);
    device->DestroyBuffer(gpuBuffer);
    CHECK(!device->IsLost());
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: per-frame map/unmap/resubmit cycle stays valid")
{
    // Sample024's frame shape: read back last frame's results (map + unmap), then
    // submit new work touching the same readback buffer. A pending or lingering map
    // would fail queue submission with "buffer still mapped".
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());

    BufferDesc sourceDesc;
    sourceDesc.size = 64;
    sourceDesc.usage = BufferUsage::CopySrc | BufferUsage::CopyDst;
    sourceDesc.memory = MemoryLocation::CpuToGpu;
    Buffer* source = nullptr;
    REQUIRE(device->CreateBuffer(sourceDesc, source).IsOk());
    BufferDesc readbackDesc;
    readbackDesc.size = 64;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    for (u64 frame = 1; frame <= 5; ++frame)
    {
        if (frame > 1)
        {
            REQUIRE(fence->Wait(frame - 1, ~0ull));
            void* mapped = readback->Map();
            CHECK(mapped != nullptr);
            if (mapped != nullptr)
            {
                readback->Unmap();
            }
        }
        CommandEncoder* encoder = nullptr;
        REQUIRE(pool->CreateEncoder(encoder).IsOk());
        encoder->CopyBufferToBuffer(source, 0, readback, 0, 64);
        CommandBuffer* commandBuffer = encoder->Finish();
        CommandBuffer* commandBuffers[] = {commandBuffer};
        queue->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, frame);
        pool->DestroyEncoder(encoder);
    }
    device->WaitIdle();
    CHECK(!device->IsLost());

    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(readback);
    device->DestroyBuffer(source);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: GenerateMipmaps + scaling Blit verify through readback")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    // An 8x8 texture with a 4-mip chain, filled solid green at mip 0.
    TextureDesc mipDesc;
    mipDesc.format = TextureFormat::RGBA8Unorm;
    mipDesc.width = 8;
    mipDesc.height = 8;
    mipDesc.mipLevelCount = 4;
    mipDesc.usage = TextureUsage::Sampled | TextureUsage::CopyDst | TextureUsage::CopySrc;
    Texture* mipTexture = nullptr;
    REQUIRE(device->CreateTexture(mipDesc, mipTexture).IsOk());

    u8 texels[8 * 8 * 4];
    for (u32 i = 0; i < 64; ++i)
    {
        texels[i * 4 + 0] = 0;
        texels[i * 4 + 1] = 255;
        texels[i * 4 + 2] = 0;
        texels[i * 4 + 3] = 255;
    }
    TransferBatch* batch = nullptr;
    REQUIRE(queue->CreateTransferBatch(batch).IsOk());
    TextureDataLayout layout;
    layout.bytesPerRow = 32;
    layout.rowsPerImage = 8;
    batch->WriteTexture(mipTexture, Span<const u8>(texels, sizeof(texels)), layout,
                        Extent3D{8, 8, 1});
    REQUIRE(batch->Submit().IsOk());
    queue->DestroyTransferBatch(batch);

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    encoder->GenerateMipmaps(mipTexture);

    // Read back mip 3 (1x1): a solid-color chain must stay solid through every level.
    BufferDesc readbackDesc;
    readbackDesc.size = 4 * 256; // sized for the 4x4 blit readback below too
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());
    BufferTextureCopyRegion region;
    region.bytesPerRow = 256;
    region.rowsPerImage = 1;
    region.textureMipLevel = 3;
    region.textureExtent = Extent3D{1, 1, 1};
    encoder->CopyTextureToBuffer(mipTexture, readback, region);

    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffer = encoder->Finish();
    CommandBuffer* commandBuffers[] = {commandBuffer};
    queue->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u8* pixel = static_cast<const u8*>(readback->Map());
    REQUIRE(pixel != nullptr);
    CHECK(pixel[0] == 0);   // R
    CHECK(pixel[1] == 255); // G survived three downsamples
    CHECK(pixel[2] == 0);   // B
    CHECK(pixel[3] == 255); // A
    readback->Unmap();

    // Scaling blit: the 8x8 green source into a 4x4 target of a DIFFERENT format.
    TextureDesc blitDesc = TextureDesc::RenderTarget(TextureFormat::BGRA8Unorm, 4, 4);
    blitDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    Texture* blitTarget = nullptr;
    REQUIRE(device->CreateTexture(blitDesc, blitTarget).IsOk());
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    encoder->Blit(mipTexture, blitTarget);
    region = BufferTextureCopyRegion{};
    region.bytesPerRow = 256;
    region.rowsPerImage = 4;
    region.textureExtent = Extent3D{4, 4, 1};
    encoder->CopyTextureToBuffer(blitTarget, readback, region);
    commandBuffer = encoder->Finish();
    CommandBuffer* second[] = {commandBuffer};
    queue->Submit(Span<CommandBuffer* const>(second, 1), fence, 2);
    REQUIRE(fence->Wait(2, ~0ull));
    pixel = static_cast<const u8*>(readback->Map());
    REQUIRE(pixel != nullptr);
    CHECK(pixel[0] == 0);   // B (BGRA order now)
    CHECK(pixel[1] == 255); // G
    CHECK(pixel[2] == 0);   // R
    readback->Unmap();

    device->DestroyFence(fence);
    device->DestroyBuffer(readback);
    device->DestroyCommandPool(pool);
    device->DestroyTexture(blitTarget);
    device->DestroyTexture(mipTexture);
    CHECK(!device->IsLost());
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: persistent mapping - writes without Unmap reach the GPU")
{
    // Vulkan's Map contract: map once, hold the pointer, write per frame, never
    // Unmap. The shadow emulation flushes OPEN mappings before every submit.
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    BufferDesc uboDesc;
    uboDesc.size = 64;
    uboDesc.usage = BufferUsage::Uniform | BufferUsage::CopySrc;
    uboDesc.memory = MemoryLocation::CpuToGpu;
    Buffer* ubo = nullptr;
    REQUIRE(device->CreateBuffer(uboDesc, ubo).IsOk());
    BufferDesc readbackDesc;
    readbackDesc.size = 64;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());

    u8* persistent = static_cast<u8*>(ubo->Map()); // NEVER unmapped
    REQUIRE(persistent != nullptr);

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());

    for (u64 frame = 1; frame <= 3; ++frame)
    {
        MemSet(persistent, static_cast<i32>(frame * 17), 64); // write through the held pointer
        CommandEncoder* encoder = nullptr;
        REQUIRE(pool->CreateEncoder(encoder).IsOk());
        encoder->CopyBufferToBuffer(ubo, 0, readback, 0, 64);
        CommandBuffer* commandBuffer = encoder->Finish();
        CommandBuffer* commandBuffers[] = {commandBuffer};
        queue->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, frame);
        REQUIRE(fence->Wait(frame, ~0ull));

        const u8* bytes = static_cast<const u8*>(readback->Map());
        REQUIRE(bytes != nullptr);
        CHECK(bytes[0] == static_cast<u8>(frame * 17)); // this frame's write arrived
        CHECK(bytes[63] == static_cast<u8>(frame * 17));
        readback->Unmap();
        pool->DestroyEncoder(encoder);
    }

    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(readback);
    device->DestroyBuffer(ubo);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: persistent shadow flush skips byte-identical re-uploads")
{
    // The persistent-map coherence flush used to re-upload every open shadow on EVERY
    // submit. On web each wgpuQueueWriteBuffer crosses the wasm->JS boundary, so that
    // dominated the frame. The flush now skips when the shadow is byte-identical to the
    // last upload. Assert the upload count only advances on real changes, and that the
    // GPU still holds correct data across the skipped flushes.
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    BufferDesc uboDesc;
    uboDesc.size = 64;
    uboDesc.usage = BufferUsage::Uniform | BufferUsage::CopySrc;
    uboDesc.memory = MemoryLocation::CpuToGpu;
    Buffer* ubo = nullptr;
    REQUIRE(device->CreateBuffer(uboDesc, ubo).IsOk());
    auto* webgpuUbo = static_cast<webgpu::WebGpuBuffer*>(ubo);

    BufferDesc readbackDesc;
    readbackDesc.size = 64;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());

    u8* persistent = static_cast<u8*>(ubo->Map()); // held open, never Unmapped
    REQUIRE(persistent != nullptr);

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());

    // Each submit copies the ubo into the readback AND triggers the shadow flush.
    const auto submitCopy = [&](u64 frame)
    {
        CommandEncoder* encoder = nullptr;
        REQUIRE(pool->CreateEncoder(encoder).IsOk());
        encoder->CopyBufferToBuffer(ubo, 0, readback, 0, 64);
        CommandBuffer* commandBuffer = encoder->Finish();
        CommandBuffer* commandBuffers[] = {commandBuffer};
        queue->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, frame);
        REQUIRE(fence->Wait(frame, ~0ull));
        pool->DestroyEncoder(encoder);
    };

    // Frame 1: first write reaches the GPU - exactly one upload.
    MemSet(persistent, 0x11, 64);
    submitCopy(1);
    CHECK(webgpuUbo->UploadCount() == 1u);

    // Frames 2-4: the shadow is untouched, so the flush must skip the write every time.
    submitCopy(2);
    submitCopy(3);
    submitCopy(4);
    CHECK(webgpuUbo->UploadCount() == 1u); // three redundant re-uploads elided

    // ...and the GPU still holds frame 1's bytes despite those skipped flushes.
    const u8* bytes = static_cast<const u8*>(readback->Map());
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 0x11);
    CHECK(bytes[63] == 0x11);
    readback->Unmap();

    // Frame 5: a real change uploads again and the new data lands.
    MemSet(persistent, 0x22, 64);
    submitCopy(5);
    CHECK(webgpuUbo->UploadCount() == 2u);
    bytes = static_cast<const u8*>(readback->Map());
    REQUIRE(bytes != nullptr);
    CHECK(bytes[0] == 0x22);
    CHECK(bytes[63] == 0x22);
    readback->Unmap();

    CHECK(!device->IsLost());
    device->DestroyFence(fence);
    device->DestroyCommandPool(pool);
    device->DestroyBuffer(readback);
    device->DestroyBuffer(ubo);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: cube faces render + cube view samples correctly")
{
    // The IBL/sky shape: render INTO per-face 2D views of a cube, then a pipeline
    // samples the CUBE view and the readback proves the right face was fetched.
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    TextureDesc cubeDesc;
    cubeDesc.format = TextureFormat::RGBA8Unorm;
    cubeDesc.width = 4;
    cubeDesc.height = 4;
    cubeDesc.arrayLayerCount = 6;
    cubeDesc.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
    Texture* cube = nullptr;
    REQUIRE(device->CreateTexture(cubeDesc, cube).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    // Clear each face to a distinct red level via its own 2D layer view.
    TextureView* faceViews[6] = {};
    for (u32 face = 0; face < 6; ++face)
    {
        TextureViewDesc faceDesc;
        faceDesc.format = TextureFormat::RGBA8Unorm;
        faceDesc.dimension = TextureViewDimension::Texture2D;
        faceDesc.baseArrayLayer = face;
        faceDesc.arrayLayerCount = 1;
        REQUIRE(device->CreateTextureView(cube, faceDesc, faceViews[face]).IsOk());
        RenderPassDesc pass;
        ColorAttachment color;
        color.view = faceViews[face];
        color.clearValue = ClearColor{static_cast<f32>(face + 1) / 8.0f, 0, 0, 1};
        pass.colorAttachments.Add(color);
        encoder->BeginRenderPass(pass)->End();
    }

    // Sample the cube's +X face (direction 1,0,0) into a 1x1 target.
    TextureViewDesc cubeViewDesc;
    cubeViewDesc.format = TextureFormat::RGBA8Unorm;
    cubeViewDesc.dimension = TextureViewDimension::TextureCube;
    cubeViewDesc.arrayLayerCount = 6;
    TextureView* cubeView = nullptr;
    REQUIRE(device->CreateTextureView(cube, cubeViewDesc, cubeView).IsOk());
    Sampler* sampler = nullptr;
    REQUIRE(device->CreateSampler(SamplerDesc{}, sampler).IsOk());

    const BindGroupLayoutEntry layoutEntries[] = {
        BindGroupLayoutEntry::SampledTexture(0, ShaderStage::Fragment,
                                             TextureViewDimension::TextureCube),
        BindGroupLayoutEntry::Sampler(0, ShaderStage::Fragment),
    };
    BindGroupLayoutDesc layoutDesc;
    layoutDesc.entries = Span<const BindGroupLayoutEntry>(layoutEntries, 2);
    BindGroupLayout* layout = nullptr;
    REQUIRE(device->CreateBindGroupLayout(layoutDesc, layout).IsOk());
    const BindGroupEntry groupEntries[] = {
        BindGroupEntry::TextureEntry(cubeView),
        BindGroupEntry::SamplerEntry(sampler),
    };
    BindGroupDesc groupDesc;
    groupDesc.layout = layout;
    groupDesc.entries = Span<const BindGroupEntry>(groupEntries, 2);
    BindGroup* group = nullptr;
    REQUIRE(device->CreateBindGroup(groupDesc, group).IsOk());

    const char8_t* wgsl =
        u8"@group(0) @binding(100) var environmentCube : texture_cube<f32>;\n"
        u8"@group(0) @binding(300) var environmentSampler : sampler;\n"
        u8"@vertex fn vertexMain(@builtin(vertex_index) index : u32)\n"
        u8"    -> @builtin(position) vec4f {\n"
        u8"  let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));\n"
        u8"  return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
        u8"}\n"
        u8"@fragment fn fragmentMain() -> @location(0) vec4f {\n"
        u8"  return textureSampleLevel(environmentCube, environmentSampler,\n"
        u8"                            vec3f(1.0, 0.0, 0.0), 0.0);\n"
        u8"}\n";
    ShaderModuleDesc moduleDesc;
    moduleDesc.code =
        Span<const u8>(reinterpret_cast<const u8*>(wgsl), StringView(wgsl).Size());
    ShaderModule* shaderModule = nullptr;
    REQUIRE(device->CreateShaderModule(moduleDesc, shaderModule).IsOk());

    PipelineLayoutDesc pipelineLayoutDesc;
    BindGroupLayout* layouts[] = {layout};
    pipelineLayoutDesc.bindGroupLayouts = Span<BindGroupLayout* const>(layouts, 1);
    PipelineLayout* pipelineLayout = nullptr;
    REQUIRE(device->CreatePipelineLayout(pipelineLayoutDesc, pipelineLayout).IsOk());
    RenderPipelineDesc pipelineDesc;
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.vertex.shader =
        ProgrammableStage{shaderModule, u8"vertexMain", ShaderStage::Vertex};
    ColorTargetState target;
    target.format = TextureFormat::RGBA8Unorm;
    FragmentState fragment;
    fragment.shader = ProgrammableStage{shaderModule, u8"fragmentMain", ShaderStage::Fragment};
    fragment.targets = Span<const ColorTargetState>(&target, 1);
    pipelineDesc.fragment = fragment;
    RenderPipeline* pipeline = nullptr;
    REQUIRE(device->CreateRenderPipeline(pipelineDesc, pipeline).IsOk());

    TextureDesc outDesc = TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 1, 1);
    outDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    Texture* outTexture = nullptr;
    REQUIRE(device->CreateTexture(outDesc, outTexture).IsOk());
    TextureViewDesc outViewDesc;
    outViewDesc.format = TextureFormat::RGBA8Unorm;
    TextureView* outView = nullptr;
    REQUIRE(device->CreateTextureView(outTexture, outViewDesc, outView).IsOk());

    RenderPassDesc samplePass;
    ColorAttachment sampleColor;
    sampleColor.view = outView;
    samplePass.colorAttachments.Add(sampleColor);
    RenderPassEncoder* pass = encoder->BeginRenderPass(samplePass);
    pass->SetPipeline(pipeline);
    pass->SetBindGroup(0, group, Span<const u32>{});
    pass->Draw(3, 1, 0, 0);
    pass->End();

    BufferDesc readbackDesc;
    readbackDesc.size = 256;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());
    BufferTextureCopyRegion region;
    region.bytesPerRow = 256;
    region.rowsPerImage = 1;
    region.textureExtent = Extent3D{1, 1, 1};
    encoder->CopyTextureToBuffer(outTexture, readback, region);

    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffer = encoder->Finish();
    CommandBuffer* commandBuffers[] = {commandBuffer};
    queue->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u8* pixel = static_cast<const u8*>(readback->Map());
    REQUIRE(pixel != nullptr);
    // +X is face 0: red = 1/8 = 32.
    CHECK(pixel[0] == 32);
    readback->Unmap();

    device->DestroyFence(fence);
    device->DestroyBuffer(readback);
    device->DestroyTextureView(outView);
    device->DestroyTexture(outTexture);
    device->DestroyRenderPipeline(pipeline);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyShaderModule(shaderModule);
    device->DestroyBindGroup(group);
    device->DestroyBindGroupLayout(layout);
    device->DestroySampler(sampler);
    device->DestroyTextureView(cubeView);
    for (u32 face = 0; face < 6; ++face)
    {
        device->DestroyTextureView(faceViews[face]);
    }
    device->DestroyTexture(cube);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: sky-shaped draw - z=1.0 vs cleared depth, read-only pass, MRT")
{
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    // Depth target cleared to 1.0 by a first pass (the prepass stand-in).
    TextureDesc depthDesc = TextureDesc::DepthBuffer(TextureFormat::Depth32Float, 4, 4);
    Texture* depthTexture = nullptr;
    REQUIRE(device->CreateTexture(depthDesc, depthTexture).IsOk());
    TextureViewDesc depthViewDesc;
    depthViewDesc.format = TextureFormat::Depth32Float;
    TextureView* depthView = nullptr;
    REQUIRE(device->CreateTextureView(depthTexture, depthViewDesc, depthView).IsOk());

    TextureDesc colorDesc = TextureDesc::RenderTarget(TextureFormat::RGBA8Unorm, 4, 4);
    colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    Texture* colorTexture = nullptr;
    REQUIRE(device->CreateTexture(colorDesc, colorTexture).IsOk());
    TextureViewDesc colorViewDesc;
    colorViewDesc.format = TextureFormat::RGBA8Unorm;
    TextureView* colorView = nullptr;
    REQUIRE(device->CreateTextureView(colorTexture, colorViewDesc, colorView).IsOk());
    TextureDesc velocityDesc = TextureDesc::RenderTarget(TextureFormat::RG16Float, 4, 4);
    Texture* velocityTexture = nullptr;
    REQUIRE(device->CreateTexture(velocityDesc, velocityTexture).IsOk());
    TextureViewDesc velocityViewDesc;
    velocityViewDesc.format = TextureFormat::RG16Float;
    velocityViewDesc.dimension = TextureViewDimension::Texture2D;
    TextureView* velocityView = nullptr;
    REQUIRE(device->CreateTextureView(velocityTexture, velocityViewDesc, velocityView).IsOk());

    const char8_t* wgsl =
        u8"struct FragmentOutput {\n"
        u8"  @location(0) color : vec4f,\n"
        u8"  @location(1) velocity : vec2f,\n"
        u8"}\n"
        u8"@vertex fn vertexMain(@builtin(vertex_index) index : u32)\n"
        u8"    -> @builtin(position) vec4f {\n"
        u8"  let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));\n"
        u8"  return vec4f(uv * 2.0 - 1.0, 1.0, 1.0);\n" // z = w = 1.0: the far plane
        u8"}\n"
        u8"@fragment fn fragmentMain() -> FragmentOutput {\n"
        u8"  var output : FragmentOutput;\n"
        u8"  output.color = vec4f(0.0, 0.0, 1.0, 1.0);\n" // sky blue
        u8"  output.velocity = vec2f(0.0);\n"
        u8"  return output;\n"
        u8"}\n";
    ShaderModuleDesc moduleDesc;
    moduleDesc.code =
        Span<const u8>(reinterpret_cast<const u8*>(wgsl), StringView(wgsl).Size());
    ShaderModule* shaderModule = nullptr;
    REQUIRE(device->CreateShaderModule(moduleDesc, shaderModule).IsOk());
    PipelineLayoutDesc emptyLayout;
    PipelineLayout* pipelineLayout = nullptr;
    REQUIRE(device->CreatePipelineLayout(emptyLayout, pipelineLayout).IsOk());

    RenderPipelineDesc pipelineDesc;
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.vertex.shader =
        ProgrammableStage{shaderModule, u8"vertexMain", ShaderStage::Vertex};
    ColorTargetState targets[2];
    targets[0].format = TextureFormat::RGBA8Unorm;
    targets[1].format = TextureFormat::RG16Float;
    FragmentState fragment;
    fragment.shader = ProgrammableStage{shaderModule, u8"fragmentMain", ShaderStage::Fragment};
    fragment.targets = Span<const ColorTargetState>(targets, 2);
    pipelineDesc.fragment = fragment;
    DepthStencilState depthState;
    depthState.format = TextureFormat::Depth32Float;
    depthState.depthTestEnabled = true;
    depthState.depthWriteEnabled = false;
    depthState.depthCompare = CompareFunction::LessEqual;
    pipelineDesc.depthStencil = depthState;
    RenderPipeline* pipeline = nullptr;
    REQUIRE(device->CreateRenderPipeline(pipelineDesc, pipeline).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    // Pass 1: clear depth to 1.0 (and color to black).
    {
        RenderPassDesc pass;
        ColorAttachment color;
        color.view = colorView;
        color.clearValue = ClearColor::Black();
        pass.colorAttachments.Add(color);
        DepthStencilAttachment depth;
        depth.view = depthView;
        depth.depthLoadOp = LoadOp::Clear;
        depth.depthClearValue = 1.0f;
        pass.depthStencilAttachment = depth;
        encoder->BeginRenderPass(pass)->End();
    }
    // Pass 2: the sky shape - load color, READ-ONLY depth, fullscreen at z=1.
    {
        RenderPassDesc pass;
        ColorAttachment color;
        color.view = colorView;
        color.loadOp = LoadOp::Load;
        pass.colorAttachments.Add(color);
        ColorAttachment velocity;
        velocity.view = velocityView;
        velocity.loadOp = LoadOp::Clear;
        pass.colorAttachments.Add(velocity);
        DepthStencilAttachment depth;
        depth.view = depthView;
        depth.depthReadOnly = true;
        pass.depthStencilAttachment = depth;
        RenderPassEncoder* sky = encoder->BeginRenderPass(pass);
        sky->SetPipeline(pipeline);
        sky->Draw(3, 1, 0, 0);
        sky->End();
    }

    BufferDesc readbackDesc;
    readbackDesc.size = 4 * 256;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());
    BufferTextureCopyRegion region;
    region.bytesPerRow = 256;
    region.rowsPerImage = 4;
    region.textureExtent = Extent3D{4, 4, 1};
    encoder->CopyTextureToBuffer(colorTexture, readback, region);

    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffer = encoder->Finish();
    CommandBuffer* commandBuffers[] = {commandBuffer};
    queue->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u8* pixel = static_cast<const u8*>(readback->Map());
    REQUIRE(pixel != nullptr);
    CHECK(pixel[2] == 255); // the sky-blue fragment SURVIVED the far-plane depth test
    readback->Unmap();

    device->DestroyFence(fence);
    device->DestroyBuffer(readback);
    device->DestroyCommandPool(pool);
    device->DestroyRenderPipeline(pipeline);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyShaderModule(shaderModule);
    device->DestroyTextureView(velocityView);
    device->DestroyTexture(velocityTexture);
    device->DestroyTextureView(colorView);
    device->DestroyTexture(colorTexture);
    device->DestroyTextureView(depthView);
    device->DestroyTexture(depthTexture);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: push-constant UNIFORM FALLBACK - forced on desktop, verified on GPU")
{
    // Browsers (Dawn/emdawnwebgpu) have no immediates, so push constants there are emulated as
    // a uniform buffer bound at @group(space) @binding(0). That path is unreachable on web
    // today (no runtime), so we FORCE it on the wgpu-native device and prove, against a real
    // GPU, that a SetPushConstants value actually reaches the shader through the emulation.
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        MESSAGE("wgpu-native sidecar or GPU unavailable - push-constant fallback test skipped");
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    // Force the uniform-buffer fallback even though this device supports immediates.
    static_cast<webgpu::WebGpuDevice*>(device)->SetForceUniformPushConstants(true);

    // Compute shader: read the emulated push-constant uniform at @group(1) @binding(0) and copy
    // it into a storage buffer at @group(0) @binding(200) (UAV shift). If the emulation binds
    // the wrong buffer/group, the readback below will not match.
    const char8_t* wgsl =
        u8"struct PushBlock { data : vec4<u32> };\n"
        u8"@group(1) @binding(0) var<uniform> pc : PushBlock;\n"
        u8"@group(0) @binding(200) var<storage, read_write> outBuf : array<u32>;\n"
        u8"@compute @workgroup_size(1) fn computeMain() {\n"
        u8"    outBuf[0] = pc.data.x; outBuf[1] = pc.data.y;\n"
        u8"    outBuf[2] = pc.data.z; outBuf[3] = pc.data.w;\n"
        u8"}\n";
    ShaderModuleDesc moduleDesc;
    moduleDesc.code = Span<const u8>(reinterpret_cast<const u8*>(wgsl), StringView(wgsl).Size());
    ShaderModule* shaderModule = nullptr;
    REQUIRE(device->CreateShaderModule(moduleDesc, shaderModule).IsOk());

    // Group 0: the read-write storage output.
    const BindGroupLayoutEntry layoutEntries[] = {
        BindGroupLayoutEntry::StorageBuffer(0, ShaderStage::Compute, /*readOnly=*/false),
    };
    BindGroupLayoutDesc bglDesc;
    bglDesc.entries = Span<const BindGroupLayoutEntry>(layoutEntries, 1);
    BindGroupLayout* bgl = nullptr;
    REQUIRE(device->CreateBindGroupLayout(bglDesc, bgl).IsOk());

    BufferDesc storageDesc;
    storageDesc.size = 16;
    storageDesc.usage = BufferUsage::Storage | BufferUsage::CopySrc;
    storageDesc.memory = MemoryLocation::GpuOnly;
    Buffer* storage = nullptr;
    REQUIRE(device->CreateBuffer(storageDesc, storage).IsOk());

    const BindGroupEntry groupEntries[] = {BindGroupEntry::BufferEntry(storage, 0, 16)};
    BindGroupDesc groupDesc;
    groupDesc.layout = bgl;
    groupDesc.entries = Span<const BindGroupEntry>(groupEntries, 1);
    BindGroup* bindGroup = nullptr;
    REQUIRE(device->CreateBindGroup(groupDesc, bindGroup).IsOk());

    // Pipeline layout declares a push-constant range targeting @group(1) - the layout must
    // synthesize the emulated uniform bind-group layout there rather than declaring immediates.
    PushConstantRange pushRange;
    pushRange.stages = ShaderStage::Compute;
    pushRange.offset = 0;
    pushRange.size = 16;
    pushRange.bindGroupIndex = 1;
    PipelineLayoutDesc plDesc;
    BindGroupLayout* layouts[] = {bgl};
    plDesc.bindGroupLayouts = Span<BindGroupLayout* const>(layouts, 1);
    plDesc.pushConstantRanges = Span<const PushConstantRange>(&pushRange, 1);
    PipelineLayout* pipelineLayout = nullptr;
    REQUIRE(device->CreatePipelineLayout(plDesc, pipelineLayout).IsOk());

    ComputePipelineDesc cpDesc;
    cpDesc.layout = pipelineLayout;
    cpDesc.compute = ProgrammableStage{shaderModule, u8"computeMain", ShaderStage::Compute};
    ComputePipeline* pipeline = nullptr;
    REQUIRE(device->CreateComputePipeline(cpDesc, pipeline).IsOk());

    // Readback buffer for the storage output.
    BufferDesc readbackDesc;
    readbackDesc.size = 16;
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Compute, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    const u32 pushValues[4] = {0xA1A1A1A1u, 0xB2B2B2B2u, 0xC3C3C3C3u, 0xD4D4D4D4u};
    ComputePassEncoder* computePass = encoder->BeginComputePass();
    REQUIRE(computePass != nullptr);
    computePass->SetPipeline(pipeline);
    computePass->SetBindGroup(0, bindGroup, Span<const u32>());
    computePass->SetPushConstants(ShaderStage::Compute, 0, 16, pushValues);
    computePass->Dispatch(1, 1, 1);
    computePass->End();
    encoder->CopyBufferToBuffer(storage, 0, readback, 0, 16);

    CommandBuffer* commandBuffer = encoder->Finish();
    REQUIRE(commandBuffer != nullptr);
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffers[] = {commandBuffer};
    device->GetQueue(QueueType::Compute)
        ->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u32* result = static_cast<const u32*>(readback->Map());
    REQUIRE(result != nullptr);
    CHECK(result[0] == pushValues[0]); // the push-constant data reached the shader...
    CHECK(result[1] == pushValues[1]); // ...through the emulated uniform bind group
    CHECK(result[2] == pushValues[2]);
    CHECK(result[3] == pushValues[3]);
    readback->Unmap();

    device->DestroyFence(fence);
    device->DestroyBuffer(readback);
    device->DestroyCommandPool(pool);
    device->DestroyComputePipeline(pipeline);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyBuffer(storage);
    device->DestroyBindGroup(bindGroup);
    device->DestroyBindGroupLayout(bgl);
    device->DestroyShaderModule(shaderModule);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: RENDER-pass push emulation - group 0, no bind groups, two draws")
{
    // The debug_geom shape: a pipeline with NO bind groups and its push block at group 0
    // (PushConstantRange.bindGroupIndex = 0, not the default 1) - the emulation must
    // synthesize the uniform at @group(0). Two draws with different push values into two
    // pixels also prove the fresh-buffer-per-draw design: the second SetPushConstants
    // must not clobber the first draw's data.
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    static_cast<webgpu::WebGpuDevice*>(device)->SetForceUniformPushConstants(true);

    const char8_t* wgsl =
        u8"struct PushBlock { color : vec4f };\n"
        u8"@group(0) @binding(0) var<uniform> pc : PushBlock;\n"
        u8"@vertex fn vertexMain(@builtin(vertex_index) index : u32)\n"
        u8"    -> @builtin(position) vec4f {\n"
        u8"  let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));\n"
        u8"  return vec4f(uv * 4.0 - 1.0, 0.0, 1.0);\n"
        u8"}\n"
        u8"@fragment fn fragmentMain() -> @location(0) vec4f { return pc.color; }\n";
    ShaderModuleDesc moduleDesc;
    moduleDesc.code = Span<const u8>(reinterpret_cast<const u8*>(wgsl), StringView(wgsl).Size());
    ShaderModule* shaderModule = nullptr;
    REQUIRE(device->CreateShaderModule(moduleDesc, shaderModule).IsOk());

    PushConstantRange pushRange;
    pushRange.stages = ShaderStage::Fragment;
    pushRange.offset = 0;
    pushRange.size = 16;
    pushRange.bindGroupIndex = 0; // no bind groups -> the push block lives at group 0
    PipelineLayoutDesc plDesc;
    plDesc.pushConstantRanges = Span<const PushConstantRange>(&pushRange, 1);
    PipelineLayout* pipelineLayout = nullptr;
    REQUIRE(device->CreatePipelineLayout(plDesc, pipelineLayout).IsOk());

    ColorTargetState target;
    target.format = TextureFormat::RGBA8Unorm;
    FragmentState frag;
    frag.shader = ProgrammableStage{shaderModule, u8"fragmentMain", ShaderStage::Fragment};
    frag.targets = Span<const ColorTargetState>(&target, 1);
    RenderPipelineDesc rpDesc;
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.shader = ProgrammableStage{shaderModule, u8"vertexMain", ShaderStage::Vertex};
    rpDesc.fragment = frag;
    rpDesc.primitive.topology = PrimitiveTopology::TriangleList;
    RenderPipeline* pipeline = nullptr;
    REQUIRE(device->CreateRenderPipeline(rpDesc, pipeline).IsOk());

    TextureDesc texDesc;
    texDesc.format = TextureFormat::RGBA8Unorm;
    texDesc.width = 2;
    texDesc.height = 1;
    texDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc;
    Texture* tex = nullptr;
    REQUIRE(device->CreateTexture(texDesc, tex).IsOk());
    TextureViewDesc viewDesc;
    viewDesc.format = TextureFormat::RGBA8Unorm;
    TextureView* view = nullptr;
    REQUIRE(device->CreateTextureView(tex, viewDesc, view).IsOk());

    BufferDesc readbackDesc;
    readbackDesc.size = 256; // bytesPerRow-aligned single row
    readbackDesc.usage = BufferUsage::CopyDst;
    readbackDesc.memory = MemoryLocation::GpuToCpu;
    Buffer* readback = nullptr;
    REQUIRE(device->CreateBuffer(readbackDesc, readback).IsOk());

    CommandPool* pool = nullptr;
    REQUIRE(device->CreateCommandPool(QueueType::Graphics, pool).IsOk());
    CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    RenderPassDesc pass;
    ColorAttachment color;
    color.view = view;
    color.clearValue = ClearColor{0, 0, 0, 0};
    pass.colorAttachments.Add(color);
    RenderPassEncoder* rp = encoder->BeginRenderPass(pass);
    REQUIRE(rp != nullptr);
    rp->SetPipeline(pipeline);
    const f32 red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const f32 green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    rp->SetPushConstants(ShaderStage::Fragment, 0, 16, red);
    rp->SetScissor(0, 0, 1, 1); // left pixel
    rp->Draw(3, 1, 0, 0);
    rp->SetPushConstants(ShaderStage::Fragment, 0, 16, green);
    rp->SetScissor(1, 0, 1, 1); // right pixel
    rp->Draw(3, 1, 0, 0);
    rp->End();

    BufferTextureCopyRegion region;
    region.bytesPerRow = 256;
    region.rowsPerImage = 1;
    region.textureExtent = Extent3D{2, 1, 1};
    encoder->CopyTextureToBuffer(tex, readback, region);

    CommandBuffer* commandBuffer = encoder->Finish();
    REQUIRE(commandBuffer != nullptr);
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    CommandBuffer* commandBuffers[] = {commandBuffer};
    device->GetQueue(QueueType::Graphics)
        ->Submit(Span<CommandBuffer* const>(commandBuffers, 1), fence, 1);
    REQUIRE(fence->Wait(1, ~0ull));

    const u8* pixels = static_cast<const u8*>(readback->Map());
    REQUIRE(pixels != nullptr);
    CHECK(pixels[0] == 255); // left = red (first draw's push data survived the second Set)
    CHECK(pixels[1] == 0);
    CHECK(pixels[4] == 0); // right = green
    CHECK(pixels[5] == 255);
    readback->Unmap();

    device->DestroyFence(fence);
    device->DestroyBuffer(readback);
    device->DestroyCommandPool(pool);
    device->DestroyRenderPipeline(pipeline);
    device->DestroyPipelineLayout(pipelineLayout);
    device->DestroyShaderModule(shaderModule);
    device->DestroyTextureView(view);
    device->DestroyTexture(tex);
    device->Destroy();
    backend->Destroy();
}

TEST_CASE("rhi.webgpu: destroying a fence with a pending work-done callback is safe")
{
    // The fb7d7e8d regression shape: a queued OnSubmittedWorkDone callback may be
    // delivered AFTER the fence it signals is destroyed. The detach/orphan handshake
    // must keep that delivery writing into live memory, and teardown must not crash
    // with signals still queued.
    Backend* backend = TryCreateBackend();
    if (backend == nullptr)
    {
        return;
    }
    Device* device = nullptr;
    REQUIRE(backend->EnumerateAdapters()[0]->CreateDevice(DeviceDesc{}, device).IsOk());
    Queue* queue = device->GetQueue(QueueType::Graphics);

    // 1. Fast path: submit + wait (resolves the signal), destroy, then keep pumping.
    Fence* fence = nullptr;
    REQUIRE(device->CreateFence(0, fence).IsOk());
    queue->Submit(Span<CommandBuffer* const>{}, fence, 1);
    CHECK(fence->Wait(1, ~0ull));
    device->DestroyFence(fence);
    queue->WaitIdle(); // pump: any late callback must hit the detached record, not the fence

    // 2. Destroy WITHOUT waiting: the callback (if still queued) is delivered after the
    //    fence is gone; then more submissions + a full teardown with work in flight.
    Fence* abandoned = nullptr;
    REQUIRE(device->CreateFence(0, abandoned).IsOk());
    queue->Submit(Span<CommandBuffer* const>{}, abandoned, 1);
    device->DestroyFence(abandoned); // no Wait - the signal may still be queued
    queue->Submit(Span<CommandBuffer* const>{}, nullptr, 0);
    queue->WaitIdle();

    device->Destroy();
    backend->Destroy();
}
