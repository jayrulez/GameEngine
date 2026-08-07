// The data-driven material model: build a material with the fluent builder, verify
// uniform-buffer layout + declared properties, drive a MaterialSystem (bind-group
// layout inferred from the property list) against the Null RHI, and check instance
// overrides + dirty notification.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.shaders;
import draconic.materials;

using namespace draconic::foundation;
using namespace draconic::materials;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

TEST_CASE("material builder: lays out uniforms + declares properties")
{
    RefPtr<Material> mat =
        MaterialBuilder(u8"pbr")
            .Shader(u8"forward")
            .Flags(shaders::ShaderFlags::NormalMap)
            .Color(u8"baseColor", Float4{1, 0, 0, 1}) // float4 -> 16 bytes, offset 0
            .Float(u8"roughness", 0.5f)               // float  ->  4 bytes, offset 16
            .Texture(u8"albedoMap")
            .Texture(u8"normalMap")
            .Sampler(u8"linearSampler")
            .Build();

    REQUIRE(mat);
    CHECK(mat->IsValid());
    CHECK(mat->shaderName == u8"forward");
    CHECK(mat->shaderFlags == shaders::ShaderFlags::NormalMap);
    CHECK(mat->pipeline.shaderName == u8"forward"); // pipeline mirrors shader id
    CHECK(mat->PropertyCount() == 5);
    CHECK(mat->UniformDataSize() == 32); // 16 (float4) + 4 (float), 16-byte rounded (WebGPU validates the bound range against the padded cbuffer)

    CHECK(mat->GetPropertyIndex(u8"roughness") == 1);
    CHECK(mat->GetPropertyIndex(u8"missing") == -1);
    const MaterialPropertyDef* rough = mat->FindProperty(u8"roughness");
    REQUIRE(rough != nullptr);
    CHECK(rough->offset == 16);
    CHECK(rough->IsUniform());

    // default uniform data carries the seeded baseColor (red) at offset 0
    const Span<const u8> defaults = mat->DefaultUniformData();
    REQUIRE(defaults.Size() == 32);
    const f32* base = reinterpret_cast<const f32*>(defaults.Data());
    CHECK(base[0] == doctest::Approx(1.0f));
    CHECK(base[1] == doctest::Approx(0.0f));
}

TEST_CASE("material system: infers bind-group layout from properties + builds instance bind group")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    MaterialSystem system;
    REQUIRE(system.Initialize(device).IsOk());
    CHECK(system.DefaultSampler() != nullptr);
    CHECK(system.WhiteTexture() != nullptr);
    CHECK(system.NormalTexture() != nullptr);

    RefPtr<Material> mat = MaterialBuilder(u8"lit")
                               .Shader(u8"forward")
                               .Color(u8"tint", Float4{1, 1, 1, 1})
                               .Texture(u8"albedoMap")
                               .Sampler(u8"samp")
                               .Build();
    REQUIRE(mat);

    // layout is cached: same material -> same pointer
    rhi::BindGroupLayout* l0 = system.GetOrCreateLayout(*mat);
    rhi::BindGroupLayout* l1 = system.GetOrCreateLayout(*mat);
    REQUIRE(l0 != nullptr);
    CHECK(l0 == l1);

    MaterialInstance inst(mat.Get());
    rhi::BindGroup* bg = system.PrepareInstance(inst);
    REQUIRE(bg != nullptr);
    CHECK(inst.BindGroupLayout() == l0);
    CHECK_FALSE(inst.IsUniformDirty());
    CHECK_FALSE(inst.IsBindGroupDirty());
    CHECK(system.GetBindGroup(inst) == bg);
}

TEST_CASE("material instance: overrides notify the system + re-prep is driven by the dirty list")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    MaterialSystem system;
    REQUIRE(system.Initialize(device).IsOk());

    RefPtr<Material> mat = MaterialBuilder(u8"lit")
                               .Shader(u8"forward")
                               .Float(u8"roughness", 0.5f)
                               .Texture(u8"albedoMap")
                               .Build();
    REQUIRE(mat);

    MaterialInstance inst(mat.Get());
    REQUIRE(system.PrepareInstance(inst) != nullptr); // clears dirty + registers sink

    // overriding a uniform marks it dirty and enqueues exactly one dirty entry
    inst.SetFloat(u8"roughness", 0.9f);
    CHECK(inst.IsUniformDirty());
    inst.SetFloat(u8"roughness", 0.8f); // second set: still one enqueue

    system.PrepareDirtyInstances();
    CHECK_FALSE(inst.IsUniformDirty()); // drained + re-prepped

    // the override is the effective value (0.8), not the material default (0.5)
    const Span<const u8> data = inst.UniformData();
    REQUIRE(data.Size() == 16); // one float, 16-byte rounded
    CHECK(*reinterpret_cast<const f32*>(data.Data()) == doctest::Approx(0.8f));

    // resetting restores the default
    inst.ResetProperty(u8"roughness");
    system.PrepareDirtyInstances();
    CHECK(*reinterpret_cast<const f32*>(inst.UniformData().Data()) == doctest::Approx(0.5f));
}

TEST_CASE("pipeline config: content hash + equality distinguish render state")
{
    PipelineConfig a = PipelineConfig::ForOpaqueMesh(u8"forward");
    PipelineConfig b = PipelineConfig::ForOpaqueMesh(u8"forward");
    CHECK(a == b);
    CHECK(a.HashCode() == b.HashCode());

    PipelineConfig c = PipelineConfig::ForTransparentMesh(u8"forward");
    CHECK_FALSE(a == c); // blend/depth differ
    CHECK(a.HashCode() != c.HashCode());

    CHECK(VertexLayoutHelper::Stride(VertexLayoutType::Mesh) == 52); // Float4 tangent
    CHECK(VertexLayoutHelper::Attributes(VertexLayoutType::Mesh).Size() == 5);
}
