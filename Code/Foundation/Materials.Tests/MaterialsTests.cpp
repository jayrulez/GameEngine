// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The data-driven material model: build a material with the fluent builder, verify
// uniform-buffer layout + declared properties, drive a MaterialSystem (bind-group
// layout inferred from the property list) against the Null RHI, and check instance
// overrides + dirty notification.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.shaders;
import foundation.materials;

using namespace foundation::core;
using namespace foundation::materials;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

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

TEST_CASE("material colours: authored sRGB, decoded to linear for the GPU; Float4 untouched")
{
    RefPtr<Material> mat = MaterialBuilder(u8"colours")
                               .Shader(u8"forward")
                               .Color(u8"Tint", Float4{0.5f, 0.25f, 1.0f, 0.5f})
                               .ColorHdr(u8"Glow", Float4{0.5f, 0.0f, 1.0f, 3.0f})
                               .Float4(u8"Params", Float4{0.5f, 0.5f, 0.5f, 0.5f})
                               .Build();
    REQUIRE(mat);
    CHECK(mat->FindProperty(u8"Tint")->type == MaterialPropertyType::Color);
    CHECK(mat->FindProperty(u8"Glow")->type == MaterialPropertyType::ColorHdr);
    CHECK(mat->FindProperty(u8"Tint")->IsColor());
    CHECK_FALSE(mat->FindProperty(u8"Params")->IsColor());

    // The authored bytes keep the colour as entered...
    const Span<const u8> authored = mat->DefaultUniformData();
    Float4 tint;
    MemCopy(&tint, authored.Data() + mat->FindProperty(u8"Tint")->offset, sizeof(tint));
    CHECK(tint.x == doctest::Approx(0.5f));

    // ...and the GPU's are linear: a Color decoded (alpha as is), a ColorHdr decoded and scaled
    // by its intensity (w = 1), a plain Float4 copied.
    Array<u8> gpu;
    gpu.Resize(authored.Size());
    EncodeUniformsForGpu(*mat, authored, gpu.Data());
    const auto at = [&](StringView n)
    {
        Float4 v;
        MemCopy(&v, gpu.Data() + mat->FindProperty(n)->offset, sizeof(v));
        return v;
    };
    CHECK(at(u8"Tint").x == doctest::Approx(SrgbToLinear(0.5f)));
    CHECK(at(u8"Tint").y == doctest::Approx(SrgbToLinear(0.25f)));
    CHECK(at(u8"Tint").z == doctest::Approx(1.0f));
    CHECK(at(u8"Tint").w == doctest::Approx(0.5f));
    CHECK(at(u8"Glow").x == doctest::Approx(SrgbToLinear(0.5f) * 3.0f));
    CHECK(at(u8"Glow").z == doctest::Approx(3.0f));
    CHECK(at(u8"Glow").w == doctest::Approx(1.0f));
    CHECK(at(u8"Params").x == doctest::Approx(0.5f));
    CHECK(at(u8"Params").w == doctest::Approx(0.5f));
}

TEST_CASE("material templates: the builtin shaders' colours are colour properties")
{
    RefPtr<Material> pbr = BuiltinMaterialTemplate(u8"forward");
    REQUIRE(pbr);
    CHECK(pbr->FindProperty(u8"BaseColor")->type == MaterialPropertyType::Color);
    CHECK(pbr->FindProperty(u8"EmissiveColor")->type == MaterialPropertyType::ColorHdr);
    RefPtr<Material> unlit = BuiltinMaterialTemplate(u8"unlit");
    REQUIRE(unlit);
    CHECK(unlit->FindProperty(u8"BaseColor")->type == MaterialPropertyType::Color);
    CHECK_FALSE(BuiltinMaterialTemplate(u8"my_custom_shader"));
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

TEST_CASE("material instance: a value wider than its property is refused, a narrower one fits")
{
    RefPtr<Material> mat = MaterialBuilder(u8"lit")
                               .Shader(u8"forward")
                               .Float(u8"roughness", 0.5f)
                               .Float(u8"metallic", 0.25f)
                               .Float4(u8"tint", Float4{1, 1, 1, 1})
                               .Build();
    REQUIRE(mat);
    MaterialInstance inst(mat.Get());
    const auto at = [&](StringView name)
    {
        f32 v = 0.0f;
        MemCopy(&v, inst.UniformData().Data() + mat->FindProperty(name)->offset, sizeof(v));
        return v;
    };
    // A Float4 into a float would write past it, over the next property: refused.
    inst.SetFloat4(u8"roughness", Float4{0.9f, 0.9f, 0.9f, 0.9f});
    CHECK(at(u8"roughness") == doctest::Approx(0.5f));
    CHECK(at(u8"metallic") == doctest::Approx(0.25f));
    // A float into a Float4 sets its first component.
    inst.SetFloat(u8"tint", 0.5f);
    CHECK(at(u8"tint") == doctest::Approx(0.5f));
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
