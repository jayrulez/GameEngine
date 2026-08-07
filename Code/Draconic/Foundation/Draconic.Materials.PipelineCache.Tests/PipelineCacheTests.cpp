// The render-side PSO cache: build a pipeline from a PipelineConfig pulling variants
// from the ShaderSystem, verify it caches, and verify the version-polling hot-reload
// path - invalidating the shader makes GetPipeline rebuild and retire the stale PSO.
// Real DXC + Null RHI.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.shaders;
import draconic.shaders.system;
import draconic.materials;
import draconic.materials.pipelinecache;

using namespace draconic::foundation;
using namespace draconic::materials;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

namespace
{
    constexpr const char8_t* kVtx =
        u8"float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    constexpr const char8_t* kFrag = u8"float4 main() : SV_Target { return float4(1,0,0,1); }\n";
}

TEST_CASE("pso cache: builds + caches a pipeline; shader reload rebuilds via version poll")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::ShaderSystem shaderSystem(*compiler, device);
    shaderSystem.RegisterSource(u8"forward", shaders::ShaderStage::Vertex, kVtx);
    shaderSystem.RegisterSource(u8"forward", shaders::ShaderStage::Fragment, kFrag);

    rhi::PipelineLayout* layout = nullptr;
    REQUIRE(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{}, layout).IsOk());

    PipelineStateCache cache(shaderSystem, device);
    const PipelineConfig config = PipelineConfig::ForOpaqueMesh(u8"forward");

    rhi::RenderPipeline* p0 = cache.GetPipeline(config, layout);
    REQUIRE(p0 != nullptr);
    CHECK(cache.Size() == 1);

    // same request -> same pipeline (cache hit, no rebuild)
    rhi::RenderPipeline* p1 = cache.GetPipeline(config, layout);
    CHECK(p1 == p0);
    CHECK(cache.Size() == 1);
    CHECK(cache.RetiredCount() == 0);

    // reload the shader: version bumps -> next GetPipeline rebuilds + retires the old PSO
    shaderSystem.InvalidateShader(u8"forward");
    rhi::RenderPipeline* p2 = cache.GetPipeline(config, layout);
    REQUIRE(p2 != nullptr);
    CHECK(p2 != p0);                  // rebuilt against the new shader
    CHECK(cache.Size() == 1);         // same key, replaced in place
    CHECK(cache.RetiredCount() == 1); // stale pipeline retired, awaiting GPU-safe free

    cache.ReleaseRetired();
    CHECK(cache.RetiredCount() == 0);

    // a distinct render state is a distinct cache entry
    rhi::RenderPipeline* pT =
        cache.GetPipeline(PipelineConfig::ForTransparentMesh(u8"forward"), layout);
    REQUIRE(pT != nullptr);
    CHECK(cache.Size() == 2);

    cache.Clear();
    CHECK(cache.Size() == 0);

    device.DestroyPipelineLayout(layout);
    compiler->Destroy();
}

TEST_CASE("pso cache: depth-only config builds without a fragment shader")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::ShaderSystem shaderSystem(*compiler, device);
    shaderSystem.RegisterSource(u8"shadow", shaders::ShaderStage::Vertex, kVtx);
    // deliberately no fragment source registered

    rhi::PipelineLayout* layout = nullptr;
    REQUIRE(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{}, layout).IsOk());

    PipelineStateCache cache(shaderSystem, device);
    PipelineConfig config = PipelineConfig::ForOpaqueMesh(u8"shadow");
    config.depthOnly = true;
    config.vertexLayout = VertexLayoutType::PositionOnly;

    rhi::RenderPipeline* p = cache.GetPipeline(config, layout);
    CHECK(p != nullptr); // vertex-only pipeline, no fragment fetched

    cache.Clear();
    device.DestroyPipelineLayout(layout);
    compiler->Destroy();
}
