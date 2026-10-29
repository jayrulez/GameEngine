// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Source-side material cook: author a Material in code, capture it into a MaterialAsset, cook
// through MaterialAssetBuilder into an output content DB, and verify the cooked MaterialSource.
// On a built-in shader the authored layout is the product's; on a custom shader the shader's
// cooked property table lays the product out and the authored material supplies the values. No
// GPU/DXC - pure authoring data.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import foundation.materials;
import foundation.materials.resource;
import materials.pipeline;
import foundation.shaders;
import foundation.shaders.resource;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::materials;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_mat_edit_db/lit.rasset");
        RemoveDirectory(u8"scratch_mat_edit_db");
    }
}

TEST_CASE("material editor: cooks a MaterialAsset on a built-in shader -> MaterialSource")
{
    GlobalTypeRegistry().Register(MaterialSource::StaticType());
    RegisterSerializable<MaterialSource>();
    RegisterMaterialAsset();

    RemoveTree();

    NativeFileSystem outMount(u8"scratch_mat_edit_db", DefaultAllocator());
    Guid id;

    // --- cook: author a material, import into an asset, build into the output DB ---
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"lit", MaterialSource::StaticType());
        id = inst->Id();

        RefPtr<Material> authored = MaterialBuilder(u8"litMat")
                                        .Shader(u8"lit")
                                        .Transparent()
                                        .Color(u8"tint", Float4{0.1f, 0.2f, 0.3f, 1.0f})
                                        .Float(u8"metallic", 0.7f)
                                        .Texture(u8"albedoMap")
                                        .Build();

        MaterialAsset asset;
        MaterialImporter::Import(*authored, Guid{}, asset); // a built-in shader, by name

        MaterialAssetBuilder builder;
        REQUIRE(builder.AssetType() == &MaterialAsset::StaticType());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // --- verify: read back the cooked MaterialSource ---
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        RefPtr<ISerializable> object = outDb.ReadObject(id);
        MaterialSource* cooked = Cast<MaterialSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->name == u8"litMat");
        CHECK(cooked->shaderId.IsNil());
        CHECK(cooked->shaderName == u8"lit");
        CHECK(cooked->blendMode == BlendMode::AlphaBlend); // .Transparent()
        CHECK(cooked->depthMode == DepthMode::ReadOnly);
        REQUIRE(cooked->propertyNames.Size() == 3);
        CHECK(cooked->propertyNames[0] == u8"tint");
        CHECK(cooked->propertyNames[1] == u8"metallic");
        CHECK(cooked->propertyNames[2] == u8"albedoMap");
        CHECK(cooked->propertyTypes[2] == static_cast<u8>(MaterialPropertyType::Texture2D));
        REQUIRE(cooked->uniformDefaults.Size() == 32); // float4 + float, 16-byte rounded
        CHECK(*reinterpret_cast<const f32*>(cooked->uniformDefaults.Data() + 16) ==
              doctest::Approx(0.7f));
    }

    RemoveTree();
}

// A material on a custom shader takes its layout from the shader's cooked property table: the
// shader's order and offsets, each value from the authored material by name where the type still
// matches, else the shader's default; a property or texture the shader no longer has is dropped.
// The material reads the shader's content, so a shader edit re-cooks it.
TEST_CASE("material editor: a material on a custom shader is laid out by the shader's table")
{
    GlobalTypeRegistry().Register(MaterialSource::StaticType());
    RegisterSerializable<MaterialSource>();
    GlobalTypeRegistry().Register(foundation::shaders::ShaderSource::StaticType());
    RegisterSerializable<foundation::shaders::ShaderSource>();
    RegisterMaterialAsset();
    FileDelete(u8"scratch_mat_shader_db/toon.rasset");
    FileDelete(u8"scratch_mat_shader_db/mat.rasset");

    NativeFileSystem mount(u8"scratch_mat_shader_db", DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");

    // The cooked shader: Bands moved after a new RimColor, Tint retyped to float, ShadowColor
    // unchanged, and AlbedoMap still a texture.
    const foundation::shaders::ShaderPropertiesBlock block =
        foundation::shaders::ParseShaderProperties(u8"properties {\n"
                                                   u8"  Color ShadowColor = (0, 0, 1, 1);\n"
                                                   u8"  Color RimColor = (1, 1, 0, 1);\n"
                                                   u8"  float Bands = 3;\n"
                                                   u8"  float Tint = 0.25;\n"
                                                   u8"  int Steps = 7;\n"
                                                   u8"  Texture AlbedoMap;\n"
                                                   u8"  Sampler MainSampler;\n"
                                                   u8"}\n");
    REQUIRE(block.IsOk());
    foundation::shaders::ShaderSource shader;
    shader.name = String(u8"toon");
    shader.SetProperties(block);
    auto* shaderInst = db.RootGroup()->CreateInstance(
        u8"toon", foundation::shaders::ShaderSource::StaticType());
    REQUIRE(shaderInst->WriteObject(shader).IsOk());

    // The authored material, from an older version of the shader.
    RefPtr<Material> authored = MaterialBuilder(u8"toonMat")
                                    .Shader(u8"toon")
                                    .Color(u8"Tint", Float4{0.5f, 0.5f, 0.5f, 1.0f}) // now a float
                                    .Color(u8"ShadowColor", Float4{0.2f, 0.1f, 0.3f, 1.0f})
                                    .Float(u8"Bands", 5.0f)
                                    .Float(u8"Steps", 2.0f) // now an int, the same size
                                    .Float(u8"Gone", 1.0f)  // no longer declared
                                    .Texture(u8"AlbedoMap")
                                    .Texture(u8"Removed")
                                    .Build();
    MaterialAsset asset;
    MaterialImporter::Import(*authored, shaderInst->Id(), asset);
    const Guid albedo{0x1ull, 0x2ull};
    const Guid removed{0x3ull, 0x4ull};
    asset.source.textureSlots.PushBack(String(u8"AlbedoMap"));
    asset.source.textureIds.PushBack(albedo);
    asset.source.textureSlots.PushBack(String(u8"Removed"));
    asset.source.textureIds.PushBack(removed);

    MaterialAssetBuilder builder;
    pipeline::AssetDependencies deps;
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    builder.ScanDependencies(asset, ctx, deps);
    REQUIRE(deps.reads.Size() == 1u);
    CHECK(deps.reads[0] == shaderInst->Id()); // a shader edit re-cooks the material

    auto* matInst = db.RootGroup()->CreateInstance(u8"mat", MaterialSource::StaticType());
    ctx.db = &db;
    ctx.output = matInst;
    REQUIRE(builder.Build(asset, ctx).IsOk());

    RefPtr<ISerializable> object = db.ReadObject(matInst->Id());
    const MaterialSource* cooked = Cast<MaterialSource>(object.Get());
    REQUIRE(cooked != nullptr);
    REQUIRE(cooked->propertyNames.Size() == 7u);
    CHECK(cooked->propertyNames[0] == u8"ShadowColor");
    CHECK(cooked->propertyNames[1] == u8"RimColor");
    CHECK(cooked->propertyNames[2] == u8"Bands");
    CHECK(cooked->propertyNames[3] == u8"Tint");
    CHECK(cooked->propertyTypes[3] == static_cast<u8>(MaterialPropertyType::Float));
    CHECK(cooked->propertyOffsets[2] == 32u);
    REQUIRE(cooked->uniformDefaults.Size() == block.uniformSize);
    const auto at = [&](u32 offset)
    { return *reinterpret_cast<const f32*>(cooked->uniformDefaults.Data() + offset); };
    CHECK(at(0) == doctest::Approx(0.2f));  // ShadowColor: the authored value
    CHECK(at(8) == doctest::Approx(0.3f));
    CHECK(at(16) == doctest::Approx(1.0f)); // RimColor: new, the shader's default
    CHECK(at(32) == doctest::Approx(5.0f)); // Bands: the authored value at its new offset
    CHECK(at(36) == doctest::Approx(0.25f)); // Tint: retyped, the shader's default
    // Steps: retyped float -> int at the same size, so the shader's default, written as an int.
    CHECK(*reinterpret_cast<const i32*>(cooked->uniformDefaults.Data() + 40) == 7);
    REQUIRE(cooked->textureSlots.Size() == 1u);
    CHECK(cooked->textureSlots[0] == u8"AlbedoMap");
    CHECK(cooked->textureIds[0] == albedo);

    FileDelete(u8"scratch_mat_shader_db/toon.rasset");
    FileDelete(u8"scratch_mat_shader_db/mat.rasset");
    RemoveDirectory(u8"scratch_mat_shader_db");
}

TEST_CASE("material editor: MaterialAsset exposes MaterialSource as a NESTED property (recurse)")
{
    // MaterialSource derives Object (non-copyable), so MaterialAsset wraps it as a Nested
    // property - the reflection-track "living proof". Tooling reaches it via address and
    // recurses into the source's own reflected scalars rather than reading it by value.
    const PropertyInfo* srcProp = FindProperty(MaterialAsset::StaticType(), "source");
    REQUIRE(srcProp != nullptr);
    CHECK(IsNested(*srcProp));
    CHECK(srcProp->type == &MaterialSource::StaticType());
    CHECK(PropertyCount(MaterialSource::StaticType()) == 10u); // the reflected scalar surface

    MaterialAsset asset;
    asset.source.blendMode = BlendMode::Masked;
    asset.source.shaderFlags = 7u;
    Instance assetInst = Instance::From(&asset);

    // A nested member is never read by value: get is empty.
    CHECK(GetProperty(*srcProp, assetInst).IsEmpty());

    // Recurse: address -> the live MaterialSource in place -> read its reflected scalars.
    void* srcAddr = srcProp->address(assetInst);
    REQUIRE(srcAddr != nullptr);
    const Instance srcInst(srcAddr, srcProp->type);
    const PropertyInfo* blendProp = FindProperty(*srcProp->type, "blendMode");
    const PropertyInfo* flagsProp = FindProperty(*srcProp->type, "shaderFlags");
    REQUIRE(blendProp != nullptr);
    REQUIRE(flagsProp != nullptr);
    CHECK(GetProperty(*blendProp, srcInst).Get<BlendMode>() == BlendMode::Masked);
    CHECK(GetProperty(*flagsProp, srcInst).Get<u32>() == 7u);
}

TEST_CASE("material editor: retyped render-state fields reflect as NAMED enums (dropdown-ready)")
{
    RegisterMaterialAsset(); // registers the render-state enums (RegisterMaterialsTypeReflection)

    const PropertyInfo* blendProp = FindProperty(MaterialSource::StaticType(), "blendMode");
    REQUIRE(blendProp != nullptr);
    REQUIRE(blendProp->type != nullptr);
    CHECK(IsEnum(*blendProp->type)); // tooling can render a name dropdown, not a raw int
    CHECK(Enumerators(*blendProp->type).Size() == 6u); // BlendMode's six named values

    const PropertyInfo* cullProp = FindProperty(MaterialSource::StaticType(), "cullMode");
    REQUIRE(cullProp != nullptr);
    CHECK(IsEnum(*cullProp->type));
    CHECK(Enumerators(*cullProp->type).Size() == 3u); // None/Back/Front
}

// The layout the cook applies, called directly (the editor previews a shader's material and
// re-lays a material out when its shader is switched): with nothing authored, every property takes
// the shader's default; the render state comes from the authored material.
TEST_CASE("material editor: a shader's table lays out a fresh material with its defaults")
{
    const foundation::shaders::ShaderPropertiesBlock block =
        foundation::shaders::ParseShaderProperties(u8"properties {\n"
                                                   u8"  Color BaseColor = (1, 0.5, 0.25, 1);\n"
                                                   u8"  float Roughness = 0.7;\n"
                                                   u8"  int Steps = 4;\n"
                                                   u8"  Texture AlbedoMap;\n"
                                                   u8"  Sampler MainSampler;\n"
                                                   u8"}\n");
    REQUIRE(block.IsOk());
    foundation::shaders::ShaderSource shader;
    shader.name = String(u8"lava");
    shader.SetProperties(block);

    MaterialSource authored;
    authored.name = String(u8"preview");
    authored.blendMode = BlendMode::AlphaBlend;
    MaterialSource out;
    LayOutMaterialByShader(authored, shader, out);

    CHECK(out.name == u8"preview");
    CHECK(out.shaderName == u8"lava"); // drawn with the shader's name
    CHECK(out.blendMode == BlendMode::AlphaBlend);
    REQUIRE(out.propertyNames.Size() == 5u);
    CHECK(out.propertyNames[3] == u8"AlbedoMap");
    CHECK(out.textureSlots.IsEmpty()); // nothing bound: the slot takes the system default
    REQUIRE(out.uniformDefaults.Size() == block.uniformSize);
    const auto at = [&](u32 offset)
    { return *reinterpret_cast<const f32*>(out.uniformDefaults.Data() + offset); };
    CHECK(at(4) == doctest::Approx(0.5f));
    CHECK(at(16) == doctest::Approx(0.7f));
    CHECK(*reinterpret_cast<const i32*>(out.uniformDefaults.Data() + 20) == 4);
}

// A material moved to another shader keeps what still fits: here a PBR material moved to the
// built-in Unlit keeps its BaseColor and its albedo texture; what Unlit lacks is dropped.
TEST_CASE("material editor: values carry onto another shader's layout by name, type and size")
{
    MaterialAsset pbr;
    MaterialImporter::Import(*CreatePBR(u8"m"), Guid{}, pbr);
    MaterialAsset unlit;
    MaterialImporter::Import(*CreateUnlit(u8"m"), Guid{}, unlit);

    const Float4 red{1.0f, 0.0f, 0.0f, 1.0f};
    const isize base = [&]
    {
        for (usize i = 0; i < pbr.source.propertyNames.Size(); ++i)
        {
            if (pbr.source.propertyNames[i] == u8"BaseColor")
            {
                return static_cast<isize>(i);
            }
        }
        return isize{-1};
    }();
    REQUIRE(base >= 0);
    MemCopy(pbr.source.uniformDefaults.Data() + pbr.source.propertyOffsets[static_cast<usize>(base)],
            &red, sizeof(red));
    const Guid albedo{0x5ull, 0x6ull};
    pbr.source.textureSlots.PushBack(String(u8"AlbedoMap"));
    pbr.source.textureIds.PushBack(albedo);
    pbr.source.textureSlots.PushBack(String(u8"NormalMap")); // Unlit has none
    pbr.source.textureIds.PushBack(Guid{0x7ull, 0x8ull});

    CarryMaterialValues(pbr.source, unlit.source);

    usize at = 0;
    bool found = false;
    for (usize i = 0; i < unlit.source.propertyNames.Size(); ++i)
    {
        if (unlit.source.propertyNames[i] == u8"BaseColor")
        {
            at = unlit.source.propertyOffsets[i];
            found = true;
        }
    }
    REQUIRE(found);
    Float4 carried{};
    MemCopy(&carried, unlit.source.uniformDefaults.Data() + at, sizeof(carried));
    CHECK(carried.x == 1.0f);
    CHECK(carried.y == 0.0f);
    REQUIRE(unlit.source.textureSlots.Size() == 1u);
    CHECK(unlit.source.textureSlots[0] == u8"AlbedoMap");
    CHECK(unlit.source.textureIds[0] == albedo);
}

// Phong takes the names PBR gives the colour, its map, the emission and the normal map, so a PBR
// material put on the built-in Phong keeps its look's makings; what Phong lacks (metal, roughness)
// is dropped and its own (specular, shininess) start at their defaults.
TEST_CASE("material editor: a PBR material put on the built-in Phong keeps its colour and its maps")
{
    MaterialAsset pbr;
    MaterialImporter::Import(*CreatePBR(u8"m", Float4{0.2f, 0.4f, 0.6f, 1.0f}), Guid{}, pbr);
    const Guid albedo{0x5ull, 0x6ull};
    const Guid normal{0x7ull, 0x8ull};
    const Guid packed{0x9ull, 0xAull};
    pbr.source.textureSlots.PushBack(String(u8"AlbedoMap"));
    pbr.source.textureIds.PushBack(albedo);
    pbr.source.textureSlots.PushBack(String(u8"NormalMap"));
    pbr.source.textureIds.PushBack(normal);
    pbr.source.textureSlots.PushBack(String(u8"MetallicRoughnessMap")); // Phong has none
    pbr.source.textureIds.PushBack(packed);

    SetMaterialBuiltInShader(pbr.source, BuiltinShading::Phong);
    CHECK(pbr.source.shaderName == u8"phong");
    const auto offsetOf = [&](StringView name) -> isize
    {
        for (usize i = 0; i < pbr.source.propertyNames.Size(); ++i)
            if (pbr.source.propertyNames[i].AsView() == name)
                return static_cast<isize>(pbr.source.propertyOffsets[i]);
        return -1;
    };
    REQUIRE(offsetOf(u8"BaseColor") >= 0);
    CHECK(offsetOf(u8"Metallic") < 0);
    Float4 carried{};
    MemCopy(&carried, pbr.source.uniformDefaults.Data() + offsetOf(u8"BaseColor"), sizeof(carried));
    CHECK(carried.y == doctest::Approx(0.4f));
    REQUIRE(pbr.source.textureSlots.Size() == 2u);
    CHECK(pbr.source.textureSlots[0] == u8"AlbedoMap");
    CHECK(pbr.source.textureIds[0] == albedo);
    CHECK(pbr.source.textureSlots[1] == u8"NormalMap");
    CHECK(pbr.source.textureIds[1] == normal);
}

// The effect groups follow the shader's vertex variants (Specs/wind-for-every-shader.md): a
// material moved between two shaders that declare WIND keeps its Wind group, values and all; one
// moved to a shader that does not, loses it.
namespace
{
    foundation::shaders::ShaderEffect WindTable()
    {
        const foundation::shaders::ShaderEffectBlock block = foundation::shaders::ParseShaderEffect(
            u8"effect Wind : WIND\n"
            u8"{\n"
            u8"    float Strength = 0 [0, 2] (gate);\n"
            u8"    float Speed = 1 [0, 20];\n"
            u8"    float Height = 1 [0.01, 100];\n"
            u8"}\n");
        REQUIRE(block.IsOk());
        return block.effect;
    }

    f32 WindStrengthOf(const MaterialSource& source)
    {
        f32 v = -1.0f;
        if (!source.effects.IsEmpty() && source.effects[0].values.Size() >= sizeof(f32))
        {
            MemCopy(&v, source.effects[0].values.Data(), sizeof(v));
        }
        return v;
    }
}

TEST_CASE("material editor: a material keeps its Wind group on a shader that declares WIND, and loses it on one that does not")
{
    RefPtr<Material> card = CreatePBR(u8"card");
    card->AddEffectGroup(WindTable());
    card->SetDefaultFloat(u8"Wind.Strength", 0.4f);
    MaterialAsset asset;
    MaterialImporter::Import(*card, Guid{}, asset);
    REQUIRE(asset.source.effects.Size() == 1u);

    // Built-in to built-in: Phong's vertex stage declares WIND as PBR's does.
    SetMaterialBuiltInShader(asset.source, BuiltinShading::Phong);
    REQUIRE(asset.source.effects.Size() == 1u);
    CHECK(asset.source.effects[0].name == u8"Wind");
    CHECK(WindStrengthOf(asset.source) == doctest::Approx(0.4f));

    // To a custom shader whose vertex stage declares WIND (a surface shader's, a low-level one
    // that lists it): kept.
    const foundation::shaders::ShaderPropertiesBlock block = foundation::shaders::ParseShaderProperties(
        u8"properties {\n  Color BaseColor = (1, 1, 1, 1);\n}\n");
    REQUIRE(block.IsOk());
    foundation::shaders::ShaderSource sways;
    sways.name = String(u8"sways");
    sways.SetProperties(block);
    sways.vertexVariantMask = static_cast<u32>(foundation::shaders::ShaderFlags::Skinned |
                                               foundation::shaders::ShaderFlags::Wind);
    SetMaterialShader(asset.source, Guid{0x1ull, 0x2ull}, sways);
    REQUIRE(asset.source.effects.Size() == 1u);
    CHECK(WindStrengthOf(asset.source) == doctest::Approx(0.4f));

    // To one that does not: dropped.
    foundation::shaders::ShaderSource still;
    still.name = String(u8"still");
    still.SetProperties(block);
    still.vertexVariantMask = static_cast<u32>(foundation::shaders::ShaderFlags::Skinned);
    SetMaterialShader(asset.source, Guid{0x3ull, 0x4ull}, still);
    CHECK(asset.source.effects.IsEmpty());

    // Back on a built-in: no group to carry, so none (the factory gives it the effect's
    // defaults where it has the shader system).
    SetMaterialBuiltInShader(asset.source, BuiltinShading::Pbr);
    CHECK(asset.source.effects.IsEmpty());
}

