// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <new>
/// Sample028 -- Procedural RT (AABB Spheres).
/// Demonstrates procedural ray tracing geometry using AABBs.
/// Renders spheres via intersection shaders inside axis-aligned bounding boxes.
/// Tests GeometryType.AABBs, ProceduralHitGroup, IntersectionShaderIndex.

#include <cstdio>
#include <cstring>

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import samples.framework;

namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

class ProceduralRTSample : public samples::framework::SampleApp
{
public:
    using samples::framework::SampleApp::SampleApp;
    foundation::core::StringView Title() const override
    {
        return u8"Sample028 - Procedural RT (AABB Spheres)";
    }

protected:
    rhi::DeviceFeatures RequiredFeatures() const override
    {
        rhi::DeviceFeatures f{};
        f.rayTracing = true;
        return f;
    }
    foundation::core::Status OnInit() override;
    void OnRender() override;
    void OnResize(foundation::core::u32 w, foundation::core::u32 h) override;
    void OnShutdown() override;

private:
    // Ray tracing shader library (compiled as lib_6_3).
    // All RT entry points are in a single source compiled once as a library.
    static constexpr const char8_t kRtShaderSource[] = u8R"(
        [[vk::image_format("rgba8")]] RWTexture2D<float4> gOutput : register(u0, space0);
        RaytracingAccelerationStructure gScene : register(t0, space0);

        struct RayPayload
        {
            float3 Color;
            float HitT;
            float2 UV;
            float2 _pad;
        };

        struct SphereAttribs
        {
            float3 Normal;
            float HitDist;
        };

        [shader("raygeneration")]
        void RayGen()
        {
            uint2 launchIndex = DispatchRaysIndex().xy;
            uint2 launchDim = DispatchRaysDimensions().xy;

            float2 uv = (float2(launchIndex) + 0.5) / float2(launchDim);
            float2 ndc = uv * 2.0 - 1.0;
            ndc.y = -ndc.y;
            float aspect = float(launchDim.x) / float(launchDim.y);
            ndc.x *= aspect;

            RayDesc ray;
            ray.Origin = float3(ndc.x * 2.0, ndc.y * 2.0, -3.0);
            ray.Direction = float3(0.0, 0.0, 1.0);
            ray.TMin = 0.001;
            ray.TMax = 100.0;

            RayPayload payload;
            payload.Color = float3(0.0, 0.0, 0.0);
            payload.HitT = -1.0;
            payload.UV = uv;
            payload._pad = float2(0, 0);

            TraceRay(gScene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, payload);

            gOutput[launchIndex] = float4(payload.Color, 1.0);
        }

        [shader("intersection")]
        void SphereIntersection()
        {
            // AABB center is at origin of the geometry instance, radius 0.5
            float3 center = float3(0, 0, 0);
            float radius = 0.45;

            float3 origin = ObjectRayOrigin();
            float3 dir = ObjectRayDirection();
            float3 oc = origin - center;

            float a = dot(dir, dir);
            float b = 2.0 * dot(oc, dir);
            float c = dot(oc, oc) - radius * radius;
            float discriminant = b * b - 4.0 * a * c;

            if (discriminant >= 0.0)
            {
                float t = (-b - sqrt(discriminant)) / (2.0 * a);
                if (t >= RayTMin() && t <= RayTCurrent())
                {
                    float3 hitPos = origin + t * dir;
                    float3 normal = normalize(hitPos - center);

                    SphereAttribs attribs;
                    attribs.Normal = normal;
                    attribs.HitDist = t;
                    ReportHit(t, 0, attribs);
                }
            }
        }

        [shader("closesthit")]
        void ClosestHit(inout RayPayload payload, SphereAttribs attribs)
        {
            // Simple diffuse shading
            float3 lightDir = normalize(float3(0.5, 1.0, -0.5));
            float3 normal = normalize(mul((float3x3)ObjectToWorld3x4(), attribs.Normal));
            float ndotl = max(0.0, dot(normal, lightDir));
            float ambient = 0.15;

            // Color based on instance index
            uint instID = InstanceIndex();
            float3 baseColor;
            if (instID == 0) baseColor = float3(1.0, 0.3, 0.3);
            else if (instID == 1) baseColor = float3(0.3, 1.0, 0.3);
            else if (instID == 2) baseColor = float3(0.3, 0.3, 1.0);
            else baseColor = float3(1.0, 1.0, 0.3);

            payload.Color = baseColor * (ndotl + ambient);
            payload.HitT = attribs.HitDist;
        }

        [shader("miss")]
        void Miss(inout RayPayload payload)
        {
            payload.Color = float3(0.05, 0.05, 0.1) + float3(0.0, 0.0, 0.15) * payload.UV.y;
            payload.HitT = -1.0;
        }
    )";

    static constexpr int kSphereCount = 4;

    shaders::Compiler* m_compiler = nullptr;

    // RT resources.
    rhi::ShaderModule* m_rtShaderModule = nullptr;
    rhi::RayTracingPipeline* m_rtPipeline = nullptr;
    rhi::AccelStruct* m_blas = nullptr;
    rhi::AccelStruct* m_tlas = nullptr;
    rhi::Buffer* m_scratchBuffer = nullptr;
    rhi::Buffer* m_aabbBuffer = nullptr;     // AABB for BLAS.
    rhi::Buffer* m_instanceBuffer = nullptr; // TLAS instance data.
    rhi::Buffer* m_sbtBuffer = nullptr;      // Shader binding table.
    rhi::PipelineLayout* m_rtPipelineLayout = nullptr;
    rhi::BindGroupLayout* m_rtBindGroupLayout = nullptr;
    rhi::BindGroup* m_rtBindGroup = nullptr;

    // RT output texture.
    rhi::Texture* m_outputTexture = nullptr;
    rhi::TextureView* m_outputTextureView = nullptr;
    rhi::ResourceState m_outputTextureState = rhi::ResourceState::Undefined;

    // SBT layout info (cached for traceRays).
    foundation::core::u32 m_sbtAlignedStride = 0;

    rhi::CommandPool* m_pool = nullptr;
    rhi::Fence* m_fence = nullptr;
    foundation::core::u64 m_fenceVal = 0;
};

foundation::core::Status ProceduralRTSample::OnInit()
{
    using foundation::core::Status, foundation::core::Span;

    // ---- Check ray tracing support ----
    if (!m_device->features.rayTracing)
    {
        std::fprintf(stderr, "ERROR: Ray tracing is not supported by this device/backend\n");
        return foundation::core::ErrorCode::Unknown;
    }

    std::printf("Ray tracing extension available:\n");
    std::printf("  shaderGroupHandleSize:      %u\n", m_device->shaderGroupHandleSize);
    std::printf("  shaderGroupHandleAlignment: %u\n", m_device->shaderGroupHandleAlignment);
    std::printf("  shaderGroupBaseAlignment:   %u\n", m_device->shaderGroupBaseAlignment);

    // ---- Shader compiler ----
    if (shaders::createCompiler(shaders::CompilerDesc{}, m_compiler) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // ---- Compile RT shader library (lib_6_3) ----
    if (samples::framework::CompileToModule(
            m_compiler, m_device, kRtShaderSource, shaders::ShaderStage::RayGen, u8"",
            u8"ProcRTLib", u8"6_3", m_rtShaderModule) != foundation::core::ErrorCode::Ok)
    {
        std::fprintf(stderr, "ERROR: RT shader library compilation failed\n");
        return foundation::core::ErrorCode::Unknown;
    }

    // ---- Command pool and fence ----
    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) !=
        foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != foundation::core::ErrorCode::Ok)
        return foundation::core::ErrorCode::Unknown;

    // ---- Create RT output texture (storage and copy source) ----
    {
        rhi::TextureDesc td{};
        td.dimension = rhi::TextureDimension::Texture2D;
        td.format = rhi::TextureFormat::RGBA8Unorm;
        td.width = m_width;
        td.height = m_height;
        td.arrayLayerCount = 1;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc;
        td.label = u8"ProcRTOutput";
        if (m_device->CreateTexture(td, m_outputTexture) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        rhi::TextureViewDesc tvd{};
        tvd.label = u8"ProcRTOutputView";
        if (m_device->CreateTextureView(m_outputTexture, tvd, m_outputTextureView) !=
            foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // ---- AABB buffer: one AABB per sphere (6 floats: minX, minY, minZ, maxX, maxY, maxZ) ----
    // All AABBs are unit cubes [-0.5, 0.5] centered at the origin; instance transforms position them
    {
        float aabbs[6] = {-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
        foundation::core::u32 aabbSize = sizeof(aabbs);

        rhi::BufferDesc bd{};
        bd.size = aabbSize;
        bd.usage = rhi::BufferUsage::AccelStructInput | rhi::BufferUsage::CopyDst;
        bd.memory = rhi::MemoryLocation::GpuOnly;
        bd.label = u8"AABBBuffer";
        if (m_device->CreateBuffer(bd, m_aabbBuffer) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        rhi::TransferBatch* transfer = nullptr;
        if (m_graphicsQueue->CreateTransferBatch(transfer) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
        transfer->WriteBuffer(m_aabbBuffer, 0,
                              Span<const foundation::core::u8>(
                                  reinterpret_cast<const foundation::core::u8*>(aabbs), aabbSize));
        transfer->Submit();
        m_graphicsQueue->DestroyTransferBatch(transfer);
    }

    // ---- Create acceleration structures ----
    {
        rhi::AccelStructDesc asd{};
        asd.type = rhi::AccelStructType::BottomLevel;
        asd.label = u8"ProcBLAS";
        if (m_device->CreateAccelStruct(asd, m_blas) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        asd.type = rhi::AccelStructType::TopLevel;
        asd.label = u8"ProcTLAS";
        if (m_device->CreateAccelStruct(asd, m_tlas) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // ---- Create scratch buffer (256 KB) ----
    {
        rhi::BufferDesc bd{};
        bd.size = 256 * 1024;
        bd.usage = rhi::BufferUsage::AccelStructScratch;
        bd.memory = rhi::MemoryLocation::GpuOnly;
        bd.label = u8"ProcScratch";
        if (m_device->CreateBuffer(bd, m_scratchBuffer) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // ---- Create instance buffer (64 bytes * 4 spheres) ----
    {
        rhi::BufferDesc bd{};
        bd.size = 64 * kSphereCount;
        bd.usage = rhi::BufferUsage::AccelStructInput;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        bd.label = u8"ProcInstances";
        if (m_device->CreateBuffer(bd, m_instanceBuffer) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // Fill instance data: 4 spheres at different positions.
    {
        auto* ptr = static_cast<foundation::core::u8*>(m_instanceBuffer->Map());
        if (!ptr)
        {
            std::fprintf(stderr, "ERROR: Failed to map instance buffer\n");
            return foundation::core::ErrorCode::Unknown;
        }

        float positions[4][3] = {
            {-1.0f, 0.5f, 0.0f}, {1.0f, 0.5f, 0.0f}, {-1.0f, -0.5f, 0.0f}, {1.0f, -0.5f, 0.0f}};

        for (int i = 0; i < kSphereCount; i++)
        {
            foundation::core::u8* inst = ptr + i * 64;
            std::memset(inst, 0, 64);

            // 3x4 row-major transform.
            auto* xform = reinterpret_cast<float*>(inst);
            xform[0] = 1.0f;
            xform[3] = positions[i][0];
            xform[5] = 1.0f;
            xform[7] = positions[i][1];
            xform[10] = 1.0f;
            xform[11] = positions[i][2];

            // instanceCustomIndex (24 bit) + mask (8 bit) at offset 48.
            inst[48] = 0;
            inst[49] = 0;
            inst[50] = 0;
            inst[51] = 0xFF; // mask

            // SBT offset (24 bit) + flags (8 bit) at offset 52.
            inst[52] = 0;
            inst[53] = 0;
            inst[54] = 0;
            inst[55] = 0x04; // VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR

            // accelerationStructureReference at offset 56.
            *reinterpret_cast<foundation::core::u64*>(inst + 56) = m_blas->DeviceAddress();
        }

        m_instanceBuffer->Unmap();
    }

    // ---- Build BLAS from AABB geometry, then TLAS ----
    {
        rhi::CommandEncoder* encoder = nullptr;
        if (m_pool->CreateEncoder(encoder) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        if (auto* rtEnc = encoder->AsRayTracingExt())
        {
            // Build BLAS from AABB geometry.
            rhi::AccelStructGeometryAABBs aabbGeom{};
            aabbGeom.aabbBuffer = m_aabbBuffer;
            aabbGeom.offset = 0;
            aabbGeom.count = 1;
            aabbGeom.stride = 24;
            aabbGeom.flags = rhi::GeometryFlags::Opaque;

            rtEnc->BuildBottomLevelAccelStruct(
                m_blas, m_scratchBuffer, 0, Span<const rhi::AccelStructGeometryTriangles>{},
                Span<const rhi::AccelStructGeometryAABBs>(&aabbGeom, 1));

            // Barrier between BLAS and TLAS build.
            rhi::MemoryBarrier mb{};
            mb.oldState = rhi::ResourceState::AccelStructWrite;
            mb.newState = rhi::ResourceState::AccelStructRead;
            rhi::BarrierGroup bg{};
            bg.memoryBarriers = Span<const rhi::MemoryBarrier>(&mb, 1);
            encoder->Barrier(bg);

            // Build TLAS from instances.
            rtEnc->BuildTopLevelAccelStruct(m_tlas, m_scratchBuffer, 0, m_instanceBuffer, 0,
                                            kSphereCount);
        }
        else
        {
            std::fprintf(stderr, "ERROR: Command encoder does not support ray tracing\n");
            m_pool->DestroyEncoder(encoder);
            return foundation::core::ErrorCode::Unknown;
        }

        rhi::CommandBuffer* cb = encoder->Finish();
        m_fenceVal++;
        rhi::CommandBuffer* cbs[1] = {cb};
        m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);

        // Wait for build to complete.
        m_fence->Wait(m_fenceVal);
        m_pool->Reset();
        m_pool->DestroyEncoder(encoder);
    }

    std::printf("Procedural BLAS/TLAS built.\n");

    // ---- Create RT bind group layout and bind group ----
    {
        rhi::BindGroupLayoutEntry layoutEntries[2]{};

        // Storage texture (read-write).
        layoutEntries[0].binding = 0;
        layoutEntries[0].visibility = rhi::ShaderStage::RayGen;
        layoutEntries[0].type = rhi::BindingType::StorageTextureReadWrite;
        layoutEntries[0].storageTextureFormat = rhi::TextureFormat::RGBA8Unorm;
        layoutEntries[0].count = 1;

        // Acceleration structure.
        layoutEntries[1].binding = 0;
        layoutEntries[1].visibility = rhi::ShaderStage::RayGen | rhi::ShaderStage::ClosestHit;
        layoutEntries[1].type = rhi::BindingType::AccelerationStructure;
        layoutEntries[1].count = 1;

        rhi::BindGroupLayoutDesc bgld{};
        bgld.entries = Span<const rhi::BindGroupLayoutEntry>(layoutEntries, 2);
        bgld.label = u8"ProcRTBGL";
        if (m_device->CreateBindGroupLayout(bgld, m_rtBindGroupLayout) !=
            foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        // Create bind group with the output texture and TLAS.
        rhi::BindGroupEntry bgEntries[2]{};
        bgEntries[0] = rhi::BindGroupEntry::TextureEntry(m_outputTextureView);
        bgEntries[1] = rhi::BindGroupEntry::AccelStructEntry(m_tlas);

        rhi::BindGroupDesc bgd{};
        bgd.layout = m_rtBindGroupLayout;
        bgd.entries = Span<const rhi::BindGroupEntry>(bgEntries, 2);
        bgd.label = u8"ProcRTBG";
        if (m_device->CreateBindGroup(bgd, m_rtBindGroup) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;
    }

    // ---- Create RT pipeline layout and pipeline ----
    {
        rhi::BindGroupLayout* bglArr[1] = {m_rtBindGroupLayout};

        rhi::PipelineLayoutDesc pld{};
        pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>(bglArr, 1);
        pld.label = u8"ProcRTPL";
        if (m_device->CreatePipelineLayout(pld, m_rtPipelineLayout) !=
            foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        // 4 stages: RayGen, Intersection, ClosestHit, Miss, all from the same shader module.
        rhi::ProgrammableStage stages[4]{};
        stages[0] = {m_rtShaderModule, u8"RayGen", rhi::ShaderStage::RayGen};
        stages[1] = {m_rtShaderModule, u8"SphereIntersection", rhi::ShaderStage::Intersection};
        stages[2] = {m_rtShaderModule, u8"ClosestHit", rhi::ShaderStage::ClosestHit};
        stages[3] = {m_rtShaderModule, u8"Miss", rhi::ShaderStage::Miss};

        // 3 groups: raygen (general), procedural hit group (intersection and closest hit), miss (general).
        rhi::RayTracingShaderGroup groups[3]{};
        groups[0].type = rhi::RayTracingShaderGroup::Type::General;
        groups[0].generalShaderIndex = 0;

        groups[1].type = rhi::RayTracingShaderGroup::Type::ProceduralHitGroup;
        groups[1].intersectionShaderIndex = 1;
        groups[1].closestHitShaderIndex = 2;

        groups[2].type = rhi::RayTracingShaderGroup::Type::General;
        groups[2].generalShaderIndex = 3;

        rhi::RayTracingPipelineDesc rtpd{};
        rtpd.layout = m_rtPipelineLayout;
        rtpd.stages = Span<const rhi::ProgrammableStage>(stages, 4);
        rtpd.groups = Span<const rhi::RayTracingShaderGroup>(groups, 3);
        rtpd.maxRecursionDepth = 1;
        rtpd.maxPayloadSize = 32;   // RayPayload: float3 + float + float2 + float2
        rtpd.maxAttributeSize = 16; // SphereAttribs: float3 Normal + float HitDist
        rtpd.label = u8"ProcRTPipeline";
        if (m_device->CreateRayTracingPipeline(rtpd, m_rtPipeline) != foundation::core::ErrorCode::Ok)
        {
            std::fprintf(stderr, "ERROR: CreateRayTracingPipeline failed\n");
            return foundation::core::ErrorCode::Unknown;
        }
    }

    std::printf("Procedural RT pipeline created successfully.\n");

    // ---- Build Shader Binding Table ----
    {
        foundation::core::u32 handleSize = m_device->shaderGroupHandleSize;
        foundation::core::u32 baseAlignment = m_device->shaderGroupBaseAlignment;
        foundation::core::u32 groupCount = 3;

        // Aligned handle stride (round up to base alignment).
        m_sbtAlignedStride = (handleSize + baseAlignment - 1) & ~(baseAlignment - 1);

        // Get shader group handles.
        foundation::core::u8 handleData[128]; // Enough for 3 handles (max ~32 bytes each).
        if (m_device->GetShaderGroupHandles(
                m_rtPipeline, 0, groupCount,
                Span<foundation::core::u8>(handleData, handleSize * groupCount)) !=
            foundation::core::ErrorCode::Ok)
        {
            std::fprintf(stderr, "ERROR: getShaderGroupHandles failed\n");
            return foundation::core::ErrorCode::Unknown;
        }

        // Create SBT buffer: 3 entries, each aligned to baseAlignment.
        foundation::core::u64 sbtSize =
            static_cast<foundation::core::u64>(m_sbtAlignedStride) * groupCount;
        rhi::BufferDesc sbd{};
        sbd.size = sbtSize;
        sbd.usage = rhi::BufferUsage::ShaderBindingTable;
        sbd.memory = rhi::MemoryLocation::CpuToGpu;
        sbd.label = u8"ProcSBT";
        if (m_device->CreateBuffer(sbd, m_sbtBuffer) != foundation::core::ErrorCode::Ok)
            return foundation::core::ErrorCode::Unknown;

        // Copy handles into SBT with proper alignment.
        auto* sbtPtr = static_cast<foundation::core::u8*>(m_sbtBuffer->Map());
        if (!sbtPtr)
        {
            std::fprintf(stderr, "ERROR: Failed to map SBT buffer\n");
            return foundation::core::ErrorCode::Unknown;
        }
        std::memset(sbtPtr, 0, static_cast<size_t>(sbtSize));

        for (foundation::core::u32 i = 0; i < groupCount; i++)
        {
            std::memcpy(sbtPtr + (i * m_sbtAlignedStride), handleData + (i * handleSize),
                        handleSize);
        }
        m_sbtBuffer->Unmap();

        std::printf(
            "SBT built: handleSize=%u, baseAlignment=%u, alignedStride=%u, totalSize=%llu\n",
            handleSize, baseAlignment, m_sbtAlignedStride,
            static_cast<unsigned long long>(sbtSize));
    }

    std::printf("Procedural RT sample ready.\n");

    return foundation::core::ErrorCode::Ok;
}

void ProceduralRTSample::OnRender()
{
    using foundation::core::Span;

    // Wait for previous frame.
    if (m_fenceVal > 0)
        m_fence->Wait(m_fenceVal, ~0ull);

    // Acquire next swap chain image.
    if (m_swapChain->AcquireNextImage() != foundation::core::ErrorCode::Ok)
        return;

    // Reset and create encoder.
    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != foundation::core::ErrorCode::Ok || !enc)
        return;

    // ---- Transition output texture to ShaderWrite for TraceRays ----
    enc->TransitionTexture(m_outputTexture, m_outputTextureState, rhi::ResourceState::ShaderWrite);

    // ---- Dispatch TraceRays ----
    if (auto* rtEnc = enc->AsRayTracingExt())
    {
        rtEnc->SetRayTracingPipeline(m_rtPipeline);
        rtEnc->SetBindGroup(0, m_rtBindGroup);

        // SBT layout: [0] = raygen, [1] = hit, [2] = miss.
        foundation::core::u64 raygenOffset = 0;
        foundation::core::u64 hitOffset = static_cast<foundation::core::u64>(1) * m_sbtAlignedStride;
        foundation::core::u64 missOffset = static_cast<foundation::core::u64>(2) * m_sbtAlignedStride;
        foundation::core::u64 stride = static_cast<foundation::core::u64>(m_sbtAlignedStride);

        rtEnc->TraceRays(m_sbtBuffer, raygenOffset, stride, m_sbtBuffer, missOffset, stride,
                         m_sbtBuffer, hitOffset, stride, m_width, m_height);
    }

    // ---- Transition: output texture ShaderWrite -> CopySrc, swapchain Present -> CopyDst ----
    {
        rhi::TextureBarrier texBarriers[2]{};
        texBarriers[0].texture = m_outputTexture;
        texBarriers[0].oldState = rhi::ResourceState::ShaderWrite;
        texBarriers[0].newState = rhi::ResourceState::CopySrc;

        texBarriers[1].texture = m_swapChain->CurrentTexture();
        texBarriers[1].oldState = rhi::ResourceState::Present;
        texBarriers[1].newState = rhi::ResourceState::CopyDst;

        rhi::BarrierGroup bg{};
        bg.textureBarriers = Span<const rhi::TextureBarrier>(texBarriers, 2);
        enc->Barrier(bg);
    }

    // ---- Copy RT output to swapchain ----
    m_outputTextureState = rhi::ResourceState::CopySrc;
    {
        rhi::TextureCopyRegion region{};
        region.extent = rhi::Extent3D{m_width, m_height, 1};
        enc->CopyTextureToTexture(m_outputTexture, m_swapChain->CurrentTexture(), region);
    }

    // ---- Transition swapchain CopyDst -> Present ----
    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::CopyDst,
                           rhi::ResourceState::Present);

    // Finish and submit.
    rhi::CommandBuffer* cb = enc->Finish();
    m_fenceVal++;
    rhi::CommandBuffer* cbs[1] = {cb};
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);

    // Present.
    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);
}

void ProceduralRTSample::OnResize(foundation::core::u32 w, foundation::core::u32 h)
{
    using foundation::core::Span;
    if (m_fence)
        m_fence->Wait(m_fenceVal, ~0ull);

    if (m_rtBindGroup)
        m_device->DestroyBindGroup(m_rtBindGroup);
    if (m_outputTextureView)
        m_device->DestroyTextureView(m_outputTextureView);
    if (m_outputTexture)
        m_device->DestroyTexture(m_outputTexture);
    m_rtBindGroup = nullptr;
    m_outputTextureView = nullptr;
    m_outputTexture = nullptr;

    rhi::TextureDesc td{};
    td.dimension = rhi::TextureDimension::Texture2D;
    td.format = rhi::TextureFormat::RGBA8Unorm;
    td.width = w;
    td.height = h;
    td.arrayLayerCount = 1;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc;
    td.label = u8"RTOutputTex";
    m_device->CreateTexture(td, m_outputTexture);

    rhi::TextureViewDesc tvd{};
    tvd.label = u8"RTOutputView";
    m_device->CreateTextureView(m_outputTexture, tvd, m_outputTextureView);

    rhi::BindGroupEntry bgEntries[2]{};
    bgEntries[0] = rhi::BindGroupEntry::TextureEntry(m_outputTextureView);
    bgEntries[1] = rhi::BindGroupEntry::AccelStructEntry(m_tlas);
    rhi::BindGroupDesc bgd{};
    bgd.layout = m_rtBindGroupLayout;
    bgd.entries = Span<const rhi::BindGroupEntry>(bgEntries, 2);
    bgd.label = u8"RTBindGroup";
    m_device->CreateBindGroup(bgd, m_rtBindGroup);

    m_outputTextureState = rhi::ResourceState::Undefined;
}

void ProceduralRTSample::OnShutdown()
{
    if (m_fence)
        m_fence->Wait(m_fenceVal, ~0ull);

    // RT bind group.
    if (m_rtBindGroup)
        m_device->DestroyBindGroup(m_rtBindGroup);
    if (m_rtBindGroupLayout)
        m_device->DestroyBindGroupLayout(m_rtBindGroupLayout);

    // RT output texture.
    if (m_outputTextureView)
        m_device->DestroyTextureView(m_outputTextureView);
    if (m_outputTexture)
        m_device->DestroyTexture(m_outputTexture);

    // RT resources.
    if (m_sbtBuffer)
        m_device->DestroyBuffer(m_sbtBuffer);
    if (m_rtPipeline)
        m_device->DestroyRayTracingPipeline(m_rtPipeline);
    if (m_rtPipelineLayout)
        m_device->DestroyPipelineLayout(m_rtPipelineLayout);
    if (m_instanceBuffer)
        m_device->DestroyBuffer(m_instanceBuffer);
    if (m_scratchBuffer)
        m_device->DestroyBuffer(m_scratchBuffer);
    if (m_tlas)
        m_device->DestroyAccelStruct(m_tlas);
    if (m_blas)
        m_device->DestroyAccelStruct(m_blas);
    if (m_aabbBuffer)
        m_device->DestroyBuffer(m_aabbBuffer);
    if (m_rtShaderModule)
        m_device->DestroyShaderModule(m_rtShaderModule);

    if (m_fence)
        m_device->DestroyFence(m_fence);
    if (m_pool)
        m_device->DestroyCommandPool(m_pool);

    if (m_compiler)
    {
        m_compiler->Destroy();
    }
}

int main(int argc, char** argv)
{
    ProceduralRTSample app;
    return app.Run(argc, argv);
}
