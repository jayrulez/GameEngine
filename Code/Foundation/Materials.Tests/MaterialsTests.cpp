// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The data-driven material model: build a material with the fluent builder, verify
// uniform-buffer layout and declared properties, drive a MaterialSystem (bind-group
// layout inferred from the property list) against the Null RHI, and check instance
// overrides and dirty notification.
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

TEST_CASE("material templates: the Phong builtin lays its cbuffer out as phong.ps.hlsl reads it")
{
    RefPtr<Material> phong = BuiltinMaterialTemplate(u8"phong");
    REQUIRE(phong);
    CHECK(phong->shaderName == StringView(u8"phong"));
    // The order phong.ps.hlsl's Material cbuffer declares, in 64 bytes.
    const struct
    {
        StringView name;
        MaterialPropertyType type;
        u32 offset;
    } kUniforms[] = {
        {u8"BaseColor", MaterialPropertyType::Color, 0u},
        {u8"SpecularColor", MaterialPropertyType::Color, 16u},
        {u8"EmissiveColor", MaterialPropertyType::ColorHdr, 32u},
        {u8"Shininess", MaterialPropertyType::Float, 48u},
        {u8"NormalScale", MaterialPropertyType::Float, 52u},
        {u8"AlphaCutoff", MaterialPropertyType::Float, 56u},
    };
    for (const auto& uniform : kUniforms)
    {
        const MaterialPropertyDef* found = phong->FindProperty(uniform.name);
        REQUIRE(found != nullptr);
        CHECK(found->type == uniform.type);
        CHECK(found->offset == uniform.offset);
    }
    // Its maps in their slots, t0 to t3, then the sampler.
    const StringView kMaps[] = {u8"AlbedoMap", u8"SpecularMap", u8"NormalMap", u8"EmissiveMap"};
    for (const StringView map : kMaps)
    {
        REQUIRE(phong->FindProperty(map) != nullptr);
        CHECK(phong->FindProperty(map)->type == MaterialPropertyType::Texture2D);
    }
    CHECK(phong->FindProperty(u8"MainSampler")->type == MaterialPropertyType::Sampler);
    // No wind property: the wind is an effect group, which a Phong material carries when its
    // shader's table is at hand (MaterialBuilder::Effect, the factory), never a property.
    CHECK(phong->FindProperty(u8"WindStrength") == nullptr);
    CHECK(phong->EffectGroupCount() == 0u);
}

TEST_CASE("material builtins: each known by its shader, its short name and its label")
{
    CHECK(kBuiltinShadingCount == 3u);
    CHECK(FindBuiltinShadingByShader(u8"forward")->shading == BuiltinShading::Pbr);
    CHECK(FindBuiltinShadingByShader(u8"unlit")->shading == BuiltinShading::Unlit);
    CHECK(FindBuiltinShadingByShader(u8"phong")->shading == BuiltinShading::Phong);
    CHECK(FindBuiltinShadingByShader(u8"toon") == nullptr);
    CHECK(FindBuiltinShadingByShortName(u8"phong")->shaderName == StringView(u8"phong"));
    CHECK(FindBuiltinShadingByShortName(u8"pbr")->shaderName == StringView(u8"forward"));
    CHECK(FindBuiltinShadingByShortName(u8"forward") == nullptr);
    for (usize i = 0; i < kBuiltinShadingCount; ++i)
    {
        // Each made from its own builtin.
        RefPtr<Material> made = CreateBuiltin(kBuiltinShadings[i].shading, u8"m");
        CHECK(made->shaderName == kBuiltinShadings[i].shaderName);
        CHECK(static_cast<usize>(kBuiltinShadings[i].shading) == i); // the table is in the enum's order
    }
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

// A material built in code and a shader's properties block lay the same list out the same way:
// HLSL's cbuffer packing, the offsets the shader reads. A float after a float3 shares its row,
// and a float2 never straddles one.
TEST_CASE("material builder: lays uniforms out as a properties block (HLSL packing)")
{
    RefPtr<Material> mat = MaterialBuilder(u8"packed")
                               .Shader(u8"custom")
                               .Float3(u8"Direction", Float3{0, 1, 0})
                               .Float(u8"Strength", 2.0f)
                               .Float2(u8"Scroll", Float2{1, 0})
                               .Float(u8"A", 0.0f)
                               .Float2(u8"Wobble", Float2{0, 0})
                               .Color(u8"Tint", Float4{1, 1, 1, 1})
                               .Build();
    const shaders::ShaderPropertiesBlock block = shaders::ParseShaderProperties(
        u8"properties {\n"
        u8"  float3 Direction;\n  float Strength;\n  float2 Scroll;\n  float A;\n"
        u8"  float2 Wobble;\n  Color Tint;\n}\n");
    REQUIRE(block.IsOk());
    REQUIRE(block.properties.Size() == mat->PropertyCount());
    for (const shaders::ShaderProperty& p : block.properties)
    {
        CAPTURE(p.name);
        const MaterialPropertyDef* def = mat->FindProperty(p.name.AsView());
        REQUIRE(def != nullptr);
        CHECK(def->offset == p.offset);
        CHECK(def->size == p.size);
    }
    CHECK(mat->FindProperty(u8"Strength")->offset == 12u); // in the float3's last lane
    CHECK(mat->FindProperty(u8"Wobble")->offset == 32u);   // at 28 it would straddle a row
    CHECK(mat->UniformDataSize() == block.uniformSize);
}

// --- Effect groups --------------------------------------------------------------------------

namespace
{
    // An effect's table, as the shader system serves it from an effect block.
    shaders::ShaderEffect WindTable()
    {
        const shaders::ShaderEffectBlock block = shaders::ParseShaderEffect(
            u8"effect Wind : WIND\n"
            u8"{\n"
            u8"    float Strength = 0 [0, 2] (gate);\n"
            u8"    float Speed = 1 [0, 20];\n"
            u8"    float Height = 1 [0.01, 100];\n"
            u8"}\n");
        REQUIRE(block.IsOk());
        return block.effect;
    }

    f32 FloatAt(Span<const u8> bytes, u32 offset)
    {
        f32 v = -1.0f;
        if (static_cast<usize>(offset) + sizeof(f32) <= bytes.Size())
        {
            MemCopy(&v, bytes.Data() + offset, sizeof(v));
        }
        return v;
    }

    // The Null device, remembering the last layout and bind group it was asked for (the Null
    // objects keep nothing).
    class RecordingDevice final : public rhi::null::NullDevice
    {
    public:
        using rhi::null::NullDevice::NullDevice;
        Array<rhi::BindGroupLayoutEntry> layoutEntries;
        Array<rhi::BindGroupEntry> groupEntries;

        Status CreateBindGroupLayout(const rhi::BindGroupLayoutDesc& desc, rhi::BindGroupLayout*& out) override
        {
            layoutEntries.Clear();
            for (const rhi::BindGroupLayoutEntry& e : desc.entries)
            {
                layoutEntries.PushBack(e);
            }
            return rhi::null::NullDevice::CreateBindGroupLayout(desc, out);
        }
        Status CreateBindGroup(const rhi::BindGroupDesc& desc, rhi::BindGroup*& out) override
        {
            groupEntries.Clear();
            for (const rhi::BindGroupEntry& e : desc.entries)
            {
                groupEntries.PushBack(e);
            }
            return rhi::null::NullDevice::CreateBindGroup(desc, out);
        }
    };
}

TEST_CASE("material effects: a group from the effect's table, addressed as Effect.Property")
{
    RefPtr<Material> mat = MaterialBuilder(u8"card")
                               .Shader(u8"forward")
                               .Color(u8"BaseColor", Float4{1, 1, 1, 1})
                               .Effect(WindTable())
                               .Build();
    REQUIRE(mat);
    CHECK(mat->PropertyCount() == 1u);    // the group is not a property
    CHECK(mat->UniformDataSize() == 16u); // nor in the shader's buffer
    REQUIRE(mat->EffectGroupCount() == 1u);
    const MaterialEffectGroup& wind = mat->GetEffectGroup(0);
    CHECK(wind.name == u8"Wind");
    CHECK(wind.flag == shaders::ShaderFlags::Wind);
    CHECK(wind.uniformSize == 16u);
    const Span<const u8> defaults{wind.defaults.Data(), wind.defaults.Size()};
    CHECK(FloatAt(defaults, 0) == doctest::Approx(0.0f)); // the table's defaults
    CHECK(FloatAt(defaults, 4) == doctest::Approx(1.0f));
    CHECK(FloatAt(defaults, 8) == doctest::Approx(1.0f));
    CHECK_FALSE(wind.IsActive());

    mat->SetDefaultFloat(u8"Wind.Strength", 0.25f);
    CHECK(wind.IsActive());
    usize group = 99;
    const MaterialPropertyDef* height = mat->FindEffectProperty(u8"Wind.Height", &group);
    REQUIRE(height != nullptr);
    CHECK(group == 0u);
    CHECK(height->offset == 8u);
    CHECK(mat->FindEffectProperty(u8"Wind.Gust") == nullptr);
    CHECK(mat->FindEffectProperty(u8"Fog.Strength") == nullptr);
    CHECK(mat->FindEffectProperty(u8"BaseColor") == nullptr); // a property's name has no dot

    // Adopting the effects a shader declares: kept (values and all) while it declares the flag,
    // dropped when it does not.
    const shaders::ShaderEffect table = WindTable();
    mat->AdoptDeclaredEffects(Span<const shaders::ShaderEffect>(&table, 1),
                              shaders::ShaderFlags::Skinned | shaders::ShaderFlags::Wind);
    REQUIRE(mat->EffectGroupCount() == 1u);
    CHECK(FloatAt(Span<const u8>{mat->GetEffectGroup(0).defaults.Data(), 16}, 0) == doctest::Approx(0.25f));
    mat->AdoptDeclaredEffects(Span<const shaders::ShaderEffect>(&table, 1), shaders::ShaderFlags::Skinned);
    CHECK(mat->EffectGroupCount() == 0u);
    // And a shader that declares it gives a material without the group one, at the defaults.
    mat->AdoptDeclaredEffects(Span<const shaders::ShaderEffect>(&table, 1), shaders::ShaderFlags::Wind);
    REQUIRE(mat->EffectGroupCount() == 1u);
    CHECK_FALSE(mat->GetEffectGroup(0).IsActive());
}

TEST_CASE("material effects: an instance overrides a group's values apart from the material's")
{
    RefPtr<Material> mat = MaterialBuilder(u8"card")
                               .Shader(u8"forward")
                               .Float(u8"Roughness", 0.5f)
                               .Effect(WindTable())
                               .Build();
    REQUIRE(mat);
    MaterialInstance inst(mat.Get());
    CHECK(FloatAt(inst.EffectData(0), 4) == doctest::Approx(1.0f));

    inst.ClearUniformDirty();
    inst.SetFloat(u8"Wind.Speed", 6.0f);
    CHECK(inst.IsUniformDirty());
    CHECK(FloatAt(inst.EffectData(0), 4) == doctest::Approx(6.0f));
    const MaterialEffectGroup& wind = mat->GetEffectGroup(0);
    CHECK(FloatAt(Span<const u8>{wind.defaults.Data(), wind.defaults.Size()}, 4) ==
          doctest::Approx(1.0f)); // the material untouched
    CHECK(FloatAt(inst.UniformData(), 0) == doctest::Approx(0.5f)); // nor the shader's buffer

    inst.SetFloat4(u8"Wind.Speed", Float4{9, 9, 9, 9}); // wider than the parameter: refused
    CHECK(FloatAt(inst.EffectData(0), 4) == doctest::Approx(6.0f));
    inst.SetFloat(u8"WindSpeed", 3.0f); // the old name reaches nothing
    CHECK(FloatAt(inst.EffectData(0), 4) == doctest::Approx(6.0f));

    inst.ResetProperty(u8"Wind.Speed");
    CHECK(FloatAt(inst.EffectData(0), 4) == doctest::Approx(1.0f));
    inst.SetFloat(u8"Wind.Strength", 0.4f);
    inst.SetFloat(u8"Wind.Height", 2.0f);
    inst.ResetEffects();
    CHECK(FloatAt(inst.EffectData(0), 0) == doctest::Approx(0.0f));
    CHECK(FloatAt(inst.EffectData(0), 8) == doctest::Approx(1.0f));
}

TEST_CASE("material system: an effect group takes binding 1 of the material's set, in the vertex stage")
{
    RecordingDevice device{DefaultAllocator()};
    MaterialSystem system;
    REQUIRE(system.Initialize(device).IsOk());

    RefPtr<Material> plain = MaterialBuilder(u8"plain")
                                 .Shader(u8"forward")
                                 .Color(u8"BaseColor", Float4{1, 1, 1, 1})
                                 .Texture(u8"AlbedoMap")
                                 .Sampler(u8"MainSampler")
                                 .Build();
    RefPtr<Material> card = MaterialBuilder(u8"card")
                                .Shader(u8"forward")
                                .Color(u8"BaseColor", Float4{1, 1, 1, 1})
                                .Texture(u8"AlbedoMap")
                                .Sampler(u8"MainSampler")
                                .Effect(WindTable())
                                .Build();
    REQUIRE(plain);
    REQUIRE(card);

    rhi::BindGroupLayout* plainLayout = system.GetOrCreateLayout(*plain);
    REQUIRE(plainLayout != nullptr);
    CHECK(device.layoutEntries.Size() == 3u); // b0, t0, s0
    rhi::BindGroupLayout* cardLayout = system.GetOrCreateLayout(*card);
    REQUIRE(cardLayout != nullptr);
    CHECK(cardLayout != plainLayout); // the group makes it another layout
    REQUIRE(device.layoutEntries.Size() == 4u);
    CHECK(device.layoutEntries[0].binding == 0u);
    CHECK(device.layoutEntries[0].type == rhi::BindingType::UniformBuffer);
    // Right after the material's buffer: the bind group pushes the effect's buffer at the same
    // position (WebGPU matches entries by position).
    CHECK(device.layoutEntries[1].binding == 1u);
    CHECK(device.layoutEntries[1].type == rhi::BindingType::UniformBuffer);
    CHECK(device.layoutEntries[1].visibility == rhi::ShaderStage::Vertex);

    card->SetDefaultFloat(u8"Wind.Strength", 0.3f);
    MaterialInstance inst(card.Get());
    inst.SetFloat(u8"Wind.Speed", 4.0f);
    REQUIRE(system.PrepareInstance(inst) != nullptr);
    REQUIRE(device.groupEntries.Size() == 4u);
    CHECK(device.groupEntries[0].buffer != nullptr);
    rhi::Buffer* windBuffer = device.groupEntries[1].buffer;
    REQUIRE(windBuffer != nullptr);
    CHECK(device.groupEntries[1].bufferSize == 16u);
    CHECK(windBuffer != device.groupEntries[0].buffer);
    // The buffer holds the instance's values, by the effect's offsets.
    const Span<const u8> written{static_cast<const u8*>(windBuffer->Map()), 16};
    CHECK(FloatAt(written, 0) == doctest::Approx(0.3f));
    CHECK(FloatAt(written, 4) == doctest::Approx(4.0f));
    CHECK(FloatAt(written, 8) == doctest::Approx(1.0f));

    // A later override rewrites it.
    inst.SetFloat(u8"Wind.Height", 0.5f);
    system.PrepareDirtyInstances();
    CHECK(FloatAt(Span<const u8>{static_cast<const u8*>(windBuffer->Map()), 16}, 8) == doctest::Approx(0.5f));

    // Handed over for retirement with the uniform buffer, then gone from the system.
    Array<rhi::Buffer*> detached;
    system.DetachEffectBuffers(&inst, detached);
    REQUIRE(detached.Size() == 1u);
    CHECK(detached[0] == windBuffer);
    Array<rhi::Buffer*> again;
    system.DetachEffectBuffers(&inst, again);
    CHECK(again.IsEmpty());
    device.DestroyBuffer(windBuffer);
}

