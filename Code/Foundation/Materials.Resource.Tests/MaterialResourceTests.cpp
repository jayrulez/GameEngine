// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Materials as resources: author a Material, capture it into a MaterialSource that
// references a ShaderSource by id, then build the Material through the ResourceManager
// with both factories registered. Verifies the cooked Material resolves the shader's
// name + properties + default uniforms, and that binding the shader mid-build recorded
// a material->shader dependency edge (so a shader reload propagates). Real DXC + Null RHI.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.rhi;
import foundation.rhi.null;
import foundation.shaders;
import foundation.shaders.system;
import foundation.shaders.resource;
import foundation.materials;
import foundation.materials.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::materials;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;

namespace
{
    constexpr const char8_t* kVtx =
        u8"float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    constexpr const char8_t* kFrag = u8"float4 main() : SV_Target { return float4(1,0,0,1); }\n";

    void RemoveTree()
    {
        FileDelete(u8"scratch_mat_res_db/lit_shader.rasset");
        FileDelete(u8"scratch_mat_res_db/lit_mat.rasset");
        RemoveDirectory(u8"scratch_mat_res_db");
    }
}

TEST_CASE("material resource: built via the manager; resolves shader + records the dependency")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    GlobalTypeRegistry().Register(shaders::ShaderSource::StaticType());
    RegisterSerializable<shaders::ShaderSource>();
    GlobalTypeRegistry().Register(shaders::ShaderResource::StaticType());
    GlobalTypeRegistry().Register(MaterialSource::StaticType());
    RegisterSerializable<MaterialSource>();
    GlobalTypeRegistry().Register(Material::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_mat_res_db", DefaultAllocator());

    Guid shaderId, matId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");

        auto* shaderInst =
            db.RootGroup()->CreateInstance(u8"lit_shader", shaders::ShaderSource::StaticType());
        shaderId = shaderInst->Id();
        shaders::ShaderSource ss;
        ss.name = String(u8"lit");
        ss.vertexSource = String(kVtx);
        ss.fragmentSource = String(kFrag);
        REQUIRE(shaderInst->WriteObject(ss).IsOk());

        // author a material in code, then capture it into a MaterialSource referencing the shader
        RefPtr<Material> authored = MaterialBuilder(u8"litMat")
                                        .Shader(u8"lit")
                                        .Color(u8"tint", Float4{0.25f, 0.5f, 0.75f, 1.0f})
                                        .Float(u8"roughness", 0.4f)
                                        .Texture(u8"albedoMap")
                                        .Build();

        MaterialSource ms;
        MaterialSource::FromMaterial(*authored, shaderId, ms);

        auto* matInst = db.RootGroup()->CreateInstance(u8"lit_mat", MaterialSource::StaticType());
        matId = matInst->Id();
        REQUIRE(matInst->WriteObject(ms).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::ShaderSystem system(*compiler, device);
    shaders::ShaderFactory shaderFactory(DefaultAllocator(), system);
    MaterialFactory materialFactory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&shaderFactory);
    manager.AddFactory(&materialFactory);

    Proxy<Material> mat = manager.Bind<Material>(matId);
    REQUIRE(mat);
    CHECK(mat->name == u8"litMat");
    CHECK(mat->shaderName == u8"lit"); // resolved from the bound ShaderResource
    CHECK(mat->depthShaderName.IsEmpty()); // no programs of its own: the built-in depth and pick
    CHECK(mat->pickShaderName.IsEmpty());
    CHECK(mat->PropertyCount() == 3);
    CHECK(mat->UniformDataSize() == 32); // float4 (16) + float (4), 16-byte rounded

    // default uniform data survived the round-trip (tint = 0.25,0.5,0.75,1 at offset 0)
    const Span<const u8> defaults = mat->DefaultUniformData();
    REQUIRE(defaults.Size() == 32);
    const f32* tint = reinterpret_cast<const f32*>(defaults.Data());
    CHECK(tint[0] == doctest::Approx(0.25f));
    CHECK(tint[2] == doctest::Approx(0.75f));
    CHECK(*reinterpret_cast<const f32*>(defaults.Data() + 16) ==
          doctest::Approx(0.4f)); // roughness

    // the factory's Bind of the shader recorded material -> shader
    const Span<const Guid> dependents = manager.Dependents(shaderId);
    bool found = false;
    for (const Guid& g : dependents)
    {
        if (g == matId)
        {
            found = true;
        }
    }
    CHECK(found);

    // reloading the shader is accepted (and transitively touches the material)
    CHECK(manager.Reload(shaderId));
    Proxy<Material> matAfter = manager.Bind<Material>(matId);
    REQUIRE(matAfter);
    CHECK(matAfter->shaderName == u8"lit");

    RemoveTree();
    compiler->Destroy();
}

TEST_CASE("material: CreatePBR packs EmissiveColor at the shader's cbuffer offset")
{
    // The forward cbuffer: BaseColor(0..16), Metallic(16..20), Roughness(20..24), pads to 32,
    // EmissiveColor(32..48). The builder's std140-ish packing must land the same offsets.
    RefPtr<Material> pbr = CreatePBR(u8"m");
    const MaterialPropertyDef* emissive = pbr->FindProperty(u8"EmissiveColor");
    REQUIRE(emissive != nullptr);
    CHECK(emissive->offset == 32u);
    CHECK(emissive->size == 16u);
    // The straggler scalars pack sequentially into the row after EmissiveColor.
    const MaterialPropertyDef* occ = pbr->FindProperty(u8"OcclusionStrength");
    const MaterialPropertyDef* ns = pbr->FindProperty(u8"NormalScale");
    const MaterialPropertyDef* ac = pbr->FindProperty(u8"AlphaCutoff");
    REQUIRE(occ != nullptr);
    REQUIRE(ns != nullptr);
    REQUIRE(ac != nullptr);
    CHECK(occ->offset == 48u);
    CHECK(ns->offset == 52u);
    CHECK(ac->offset == 56u);
    // Black default: adding the field changes nothing visually.
    const Span<const u8> defaults = pbr->DefaultUniformData();
    REQUIRE(defaults.Size() >= 48u);
    f32 rgb[3];
    MemCopy(rgb, defaults.Data() + 32, sizeof(rgb));
    CHECK(rgb[0] == 0.0f);
    CHECK(rgb[1] == 0.0f);
    CHECK(rgb[2] == 0.0f);
}

TEST_CASE("material: a forward source missing the forward properties is STALE (refused, never upgraded)")
{
    // An asset authored BEFORE EmissiveColor existed: the old CreatePBR property set.
    RefPtr<Material> old = MaterialBuilder(u8"legacy")
                               .Shader(u8"forward")
                               .VertexLayout(VertexLayoutType::Mesh)
                               .Color(u8"BaseColor", Float4{0.5f, 0.25f, 0.125f, 1.0f})
                               .Float(u8"Metallic", 1.0f)
                               .Float(u8"Roughness", 0.25f)
                               .Texture(u8"AlbedoMap")
                               .Sampler(u8"MainSampler")
                               .Build();
    MaterialSource src;
    MaterialSource::FromMaterial(*old, Guid{}, src);
    src.shaderName = String(u8"forward");
    StringView missing;
    CHECK_FALSE(ForwardMaterialSourceIsComplete(src, &missing));
    CHECK(missing == StringView(u8"EmissiveColor")); // the refusal names what to fix
    CHECK(src.propertyNames.Size() == 5u);            // nothing appended

    // A current CreatePBR source carries the full table; non-forward shaders are untouched.
    RefPtr<Material> pbr = CreatePBR(u8"current");
    MaterialSource current;
    MaterialSource::FromMaterial(*pbr, Guid{}, current);
    current.shaderName = String(u8"forward");
    CHECK(ForwardMaterialSourceIsComplete(current));
    MaterialSource unlit;
    unlit.shaderName = String(u8"unlit");
    CHECK(ForwardMaterialSourceIsComplete(unlit));
}

TEST_CASE("material source: a stored Float4 colour reads as the template's colour type")
{
    // A material stored before colour properties existed: its colours are Float4.
    RefPtr<Material> pbr = CreatePBR(u8"old");
    MaterialSource src;
    MaterialSource::FromMaterial(*pbr, Guid{}, src);
    src.shaderName = String(u8"forward");
    usize baseColor = 0, emissive = 0, metallic = 0;
    for (usize i = 0; i < src.propertyNames.Size(); ++i)
    {
        if (src.propertyNames[i] == u8"BaseColor") baseColor = i;
        if (src.propertyNames[i] == u8"EmissiveColor") emissive = i;
        if (src.propertyNames[i] == u8"Metallic") metallic = i;
        if (src.propertyTypes[i] == static_cast<u8>(MaterialPropertyType::Color) ||
            src.propertyTypes[i] == static_cast<u8>(MaterialPropertyType::ColorHdr))
        {
            src.propertyTypes[i] = static_cast<u8>(MaterialPropertyType::Float4);
        }
    }

    src.AdoptTemplateColorTypes();
    CHECK(src.propertyTypes[baseColor] == static_cast<u8>(MaterialPropertyType::Color));
    CHECK(src.propertyTypes[emissive] == static_cast<u8>(MaterialPropertyType::ColorHdr));
    CHECK(src.propertyTypes[metallic] == static_cast<u8>(MaterialPropertyType::Float));
    src.AdoptTemplateColorTypes(); // idempotent
    CHECK(src.propertyTypes[baseColor] == static_cast<u8>(MaterialPropertyType::Color));

    // A custom shader's Float4 stays a Float4: nothing says it is a colour.
    MaterialSource custom;
    MaterialSource::FromMaterial(*pbr, Guid{}, custom);
    custom.shaderName = String(u8"my_custom_shader");
    custom.propertyTypes[baseColor] = static_cast<u8>(MaterialPropertyType::Float4);
    custom.AdoptTemplateColorTypes();
    CHECK(custom.propertyTypes[baseColor] == static_cast<u8>(MaterialPropertyType::Float4));
}

TEST_CASE("material source: sampler address modes round-trip")
{
    using namespace foundation::materials;
    NativeFileSystem mount(u8"scratch_mat_sampler_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        MaterialSource source;
        source.name = u8"wrapped";
        source.shaderName = u8"forward";
        source.samplerU = 2; // rhi::AddressMode::ClampToEdge
        source.samplerV = 1; // rhi::AddressMode::MirrorRepeat
        auto* inst = db.RootGroup()->CreateInstance(u8"wrapped", MaterialSource::StaticType());
        id = inst->Id();
        REQUIRE(inst->WriteObject(source).IsOk());
    }
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        RefPtr<ISerializable> object = db.ReadObject(id);
        auto* read = Cast<MaterialSource>(object.Get());
        REQUIRE(read != nullptr);
        CHECK(read->samplerU == 2); // the v2 envelope round-trips the modes
        CHECK(read->samplerV == 1);
    }

    FileDelete(u8"scratch_mat_sampler_db/wrapped.rasset");
    RemoveDirectory(u8"scratch_mat_sampler_db");
}

TEST_CASE("material resource: the retyped render-state enum fields round-trip byte-identically")
{
    // u8 -> BlendMode/DepthMode/CullModeConfig/VertexLayoutType (Fable Q2). Each enum serializes as
    // its underlying u8, so the wire is unchanged and old bytes load into the enum. These four are
    // serialized unconditionally (before the version-gated sampler fields), so version is irrelevant.
    MaterialSource out;
    out.blendMode = BlendMode::Additive;               // != default Opaque
    out.depthMode = DepthMode::WriteOnly;              // != default ReadWrite
    out.cullMode = CullModeConfig::Front;              // != default Back
    out.vertexLayout = VertexLayoutType::SkinnedMesh;  // != default Mesh

    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        out.Serialize(writer);
    }
    MaterialSource in;
    {
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        in.Serialize(reader);
    }

    CHECK(in.blendMode == BlendMode::Additive);
    CHECK(in.depthMode == DepthMode::WriteOnly);
    CHECK(in.cullMode == CullModeConfig::Front);
    CHECK(in.vertexLayout == VertexLayoutType::SkinnedMesh);
}

// The one conversion from a cooked source to a runtime material (the factory and the editor's
// previews): its layout, defaults, render state and sampler addressing, under the shader name
// given, with no texture bound.
TEST_CASE("material source: MaterialFromSource carries layout, defaults and render state")
{
    MaterialSource src;
    src.name = String(u8"lava");
    src.shaderName = String(u8"ignored"); // the caller names the shader it resolved
    src.blendMode = BlendMode::AlphaBlend;
    src.cullMode = CullModeConfig::None;
    src.samplerU = static_cast<u8>(foundation::rhi::AddressMode::MirrorRepeat);
    src.propertyNames.PushBack(String(u8"Glow"));
    src.propertyTypes.PushBack(static_cast<u8>(MaterialPropertyType::Float));
    src.propertyBindings.PushBack(0u);
    src.propertyOffsets.PushBack(4u);
    src.propertySizes.PushBack(4u);
    src.uniformDefaults.Resize(16);
    for (u8& b : src.uniformDefaults)
    {
        b = 0;
    }
    const f32 glow = 4.0f;
    MemCopy(src.uniformDefaults.Data() + 4, &glow, sizeof(glow));

    RefPtr<Material> material = MaterialFromSource(DefaultAllocator(), src, u8"lava_shader");
    REQUIRE(material.Get() != nullptr);
    CHECK(material->name == u8"lava");
    CHECK(material->shaderName == u8"lava_shader");
    CHECK(material->pipeline.shaderName == u8"lava_shader");
    CHECK(material->pipeline.blendMode == BlendMode::AlphaBlend);
    CHECK(material->pipeline.cullMode == CullModeConfig::None);
    CHECK(material->samplerU == foundation::rhi::AddressMode::MirrorRepeat);
    const MaterialPropertyDef* def = material->FindProperty(u8"Glow");
    REQUIRE(def != nullptr);
    CHECK(def->offset == 4u);
    REQUIRE(material->DefaultUniformData().Size() >= 8u);
    CHECK(*reinterpret_cast<const f32*>(material->DefaultUniformData().Data() + 4) == 4.0f);
}

// A material on a shader with its own depth and pick programs draws the depth-like passes with
// them: always when the shader moves its vertices, else only when the material is masked.
TEST_CASE("material resource: a material takes its shader's depth and pick programs where needed")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    GlobalTypeRegistry().Register(shaders::ShaderSource::StaticType());
    RegisterSerializable<shaders::ShaderSource>();
    GlobalTypeRegistry().Register(shaders::ShaderResource::StaticType());
    GlobalTypeRegistry().Register(MaterialSource::StaticType());
    RegisterSerializable<MaterialSource>();
    GlobalTypeRegistry().Register(Material::StaticType());
    RemoveTree();
    FileDelete(u8"scratch_mat_res_db/opaque_mat.rasset");
    FileDelete(u8"scratch_mat_res_db/masked_mat.rasset");
    NativeFileSystem mount(u8"scratch_mat_res_db", DefaultAllocator());

    const auto run = [&](bool always) -> Array<String>
    {
        Guid shaderId, opaqueId, maskedId;
        {
            foundation::content::ContentDatabase db(DefaultAllocator(), mount,
                                                    BinarySerializerFactory(), u8".rasset");
            auto* shaderInst =
                db.RootGroup()->CreateInstance(u8"lit_shader", shaders::ShaderSource::StaticType());
            shaderId = shaderInst->Id();
            shaders::ShaderSource ss;
            ss.name = String(u8"lit");
            ss.vertexSource = String(kVtx);
            ss.fragmentSource = String(kFrag);
            ss.AddProgram(shaders::kDepthProgramSuffix, kVtx, u8"main", shaders::ShaderFlags::None,
                          kFrag, u8"main", shaders::ShaderFlags::None);
            ss.AddProgram(shaders::kPickProgramSuffix, kVtx, u8"main", shaders::ShaderFlags::None,
                          kFrag, u8"main", shaders::ShaderFlags::None);
            ss.depthProgramsAlways = always;
            REQUIRE(shaderInst->WriteObject(ss).IsOk());
            RefPtr<Material> authored = MaterialBuilder(u8"m").Shader(u8"lit").Float(u8"x", 1.0f).Build();
            for (int masked = 0; masked < 2; ++masked)
            {
                MaterialSource ms;
                MaterialSource::FromMaterial(*authored, shaderId, ms);
                ms.blendMode = masked ? BlendMode::Masked : BlendMode::Opaque;
                auto* inst = db.RootGroup()->CreateInstance(masked ? u8"masked_mat" : u8"opaque_mat",
                                                            MaterialSource::StaticType());
                (masked ? maskedId : opaqueId) = inst->Id();
                REQUIRE(inst->WriteObject(ms).IsOk());
            }
        }
        foundation::content::ContentDatabase db(DefaultAllocator(), mount,
                                                BinarySerializerFactory(), u8".rasset");
        rhi::null::NullDevice device{DefaultAllocator()};
        shaders::ShaderSystem system(*compiler, device);
        shaders::ShaderFactory shaderFactory(DefaultAllocator(), system);
        MaterialFactory materialFactory;
        ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&shaderFactory);
        manager.AddFactory(&materialFactory);
        Proxy<Material> opaque = manager.Bind<Material>(opaqueId);
        Proxy<Material> masked = manager.Bind<Material>(maskedId);
        REQUIRE(opaque);
        REQUIRE(masked);
        Array<String> names;
        names.PushBack(String(opaque->depthShaderName.AsView()));
        names.PushBack(String(masked->depthShaderName.AsView()));
        names.PushBack(String(masked->pickShaderName.AsView()));
        FileDelete(u8"scratch_mat_res_db/opaque_mat.rasset");
        FileDelete(u8"scratch_mat_res_db/masked_mat.rasset");
        RemoveTree();
        return names;
    };

    const Array<String> onlyMasked = run(false);
    CHECK(onlyMasked[0].IsEmpty()); // opaque, still: the built-in depth
    CHECK(onlyMasked[1] == u8"lit.depth");
    CHECK(onlyMasked[2] == u8"lit.pick");
    const Array<String> always = run(true);
    CHECK(always[0] == u8"lit.depth"); // it moves its vertices: its own depth, opaque too
    compiler->Destroy();
}

// --- Effect groups (Specs/wind-for-every-shader.md) ----------------------------------------

namespace
{
    f32 FloatAt(const Array<u8>& bytes, u32 offset)
    {
        f32 v = -1.0f;
        if (static_cast<usize>(offset) + sizeof(f32) <= bytes.Size())
        {
            MemCopy(&v, bytes.Data() + offset, sizeof(v));
        }
        return v;
    }

    isize IndexOfName(const Array<String>& names, StringView name)
    {
        for (usize i = 0; i < names.Size(); ++i)
        {
            if (names[i].AsView() == name)
            {
                return static_cast<isize>(i);
            }
        }
        return -1;
    }

    // A PBR material as data version 3 stored it: the wind in the buffer's spare lanes.
    void LegacyPbrSource(MaterialSource& out, f32 strength, f32 speed, f32 height)
    {
        RefPtr<Material> old = MaterialBuilder(u8"grass")
                                   .Shader(u8"forward")
                                   .VertexLayout(VertexLayoutType::Mesh)
                                   .Color(u8"BaseColor", Float4{0.2f, 0.6f, 0.2f, 1.0f})
                                   .Float(u8"Metallic", 0.0f)
                                   .Float(u8"Roughness", 0.8f)
                                   .Float(u8"WindStrength", strength)
                                   .Float(u8"WindSpeed", speed)
                                   .ColorHdr(u8"EmissiveColor", Float4{0, 0, 0, 1})
                                   .Float(u8"OcclusionStrength", 1.0f)
                                   .Float(u8"NormalScale", 1.0f)
                                   .Float(u8"AlphaCutoff", 0.5f)
                                   .Float(u8"WindHeight", height)
                                   .Texture(u8"AlbedoMap")
                                   .Texture(u8"NormalMap")
                                   .Texture(u8"MetallicRoughnessMap")
                                   .Texture(u8"OcclusionMap")
                                   .Texture(u8"EmissiveMap")
                                   .Sampler(u8"MainSampler")
                                   .Build();
        MaterialSource::FromMaterial(*old, Guid{}, out);
        out.shaderName = String(u8"forward");
    }

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
}

TEST_CASE("material source: a version-3 PBR material's wind lanes migrate into a Wind group")
{
    MaterialSource src;
    LegacyPbrSource(src, 0.3f, 2.0f, 0.7f);
    REQUIRE(IndexOfName(src.propertyNames, u8"WindStrength") == 3);
    const usize before = src.propertyNames.Size();
    const Array<u8> bytes = src.uniformDefaults;

    src.MigrateWindLanes();
    CHECK(IndexOfName(src.propertyNames, u8"WindStrength") < 0);
    CHECK(IndexOfName(src.propertyNames, u8"WindSpeed") < 0);
    CHECK(IndexOfName(src.propertyNames, u8"WindHeight") < 0);
    CHECK(src.propertyNames.Size() == before - 3u);
    CHECK(src.propertyTypes.Size() == src.propertyNames.Size());
    CHECK(src.propertyOffsets.Size() == src.propertyNames.Size());
    CHECK(src.propertySizes.Size() == src.propertyNames.Size());
    CHECK(src.propertyBindings.Size() == src.propertyNames.Size());
    // The rest unmoved, the buffer's bytes as they were (the lanes are padding now).
    const isize emissive = IndexOfName(src.propertyNames, u8"EmissiveColor");
    REQUIRE(emissive >= 0);
    CHECK(src.propertyOffsets[static_cast<usize>(emissive)] == 32u);
    CHECK(src.uniformDefaults.Size() == bytes.Size());
    // Laid out as today's CreatePBR is: the same table, so it is complete and draws as one.
    RefPtr<Material> pbr = CreatePBR(u8"today");
    MaterialSource today;
    MaterialSource::FromMaterial(*pbr, Guid{}, today);
    REQUIRE(today.propertyNames.Size() == src.propertyNames.Size());
    for (usize i = 0; i < today.propertyNames.Size(); ++i)
    {
        CHECK(src.propertyNames[i] == today.propertyNames[i]);
        CHECK(src.propertyOffsets[i] == today.propertyOffsets[i]);
        CHECK(src.propertyBindings[i] == today.propertyBindings[i]);
    }
    CHECK(ForwardMaterialSourceIsComplete(src));

    // The Wind group, holding the lanes' values.
    REQUIRE(src.effects.Size() == 1u);
    const MaterialEffectGroupSource& wind = src.effects[0];
    CHECK(wind.name == u8"Wind");
    CHECK(wind.flag == static_cast<u32>(shaders::ShaderFlags::Wind));
    CHECK(wind.gateIndex == 0u);
    CHECK(wind.uniformSize == 16u);
    REQUIRE(wind.propertyNames.Size() == 3u);
    CHECK(wind.propertyNames[0] == u8"Strength");
    CHECK(wind.propertyNames[1] == u8"Speed");
    CHECK(wind.propertyNames[2] == u8"Height");
    CHECK(FloatAt(wind.values, 0) == doctest::Approx(0.3f));
    CHECK(FloatAt(wind.values, 4) == doctest::Approx(2.0f));
    CHECK(FloatAt(wind.values, 8) == doctest::Approx(0.7f));

    // Idempotent: nothing left to migrate, the group kept once.
    src.MigrateWindLanes();
    CHECK(src.effects.Size() == 1u);
    CHECK(src.propertyNames.Size() == before - 3u);

    // A custom shader's material keeps properties of those names: they are its own.
    MaterialSource custom;
    LegacyPbrSource(custom, 0.3f, 2.0f, 0.7f);
    custom.shaderId = Guid{1u, 2u}; // any custom shader
    custom.MigrateWindLanes();
    CHECK(IndexOfName(custom.propertyNames, u8"WindStrength") == 3);
    CHECK(custom.effects.IsEmpty());
}

TEST_CASE("material source: reading migrates a stored PBR material; the group round-trips")
{
    MaterialSource legacy;
    LegacyPbrSource(legacy, 0.0f, 0.0f, 1.0f); // what every stored material holds: wind off
    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        legacy.Serialize(writer);
    }
    MaterialSource read;
    {
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        read.Serialize(reader);
    }
    CHECK(IndexOfName(read.propertyNames, u8"WindStrength") < 0);
    REQUIRE(read.effects.Size() == 1u);
    CHECK(FloatAt(read.effects[0].values, 0) == doctest::Approx(0.0f));
    CHECK(FloatAt(read.effects[0].values, 8) == doctest::Approx(1.0f));

    // And the runtime material carries it, off.
    RefPtr<Material> material = MaterialFromSource(DefaultAllocator(), read, u8"forward");
    REQUIRE(material->EffectGroupCount() == 1u);
    CHECK(material->GetEffectGroup(0).name == u8"Wind");
    CHECK(material->GetEffectGroup(0).flag == shaders::ShaderFlags::Wind);
    CHECK_FALSE(material->GetEffectGroup(0).IsActive());

    // A material's groups round-trip through FromMaterial and the stream.
    material->SetDefaultFloat(u8"Wind.Strength", 0.5f);
    MaterialSource again;
    MaterialSource::FromMaterial(*material, Guid{}, again);
    MemoryStream second;
    {
        BinarySerializer writer(second, SerializeMode::Write);
        again.Serialize(writer);
    }
    MaterialSource back;
    {
        (void)second.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(second, SerializeMode::Read);
        back.Serialize(reader);
    }
    REQUIRE(back.effects.Size() == 1u);
    CHECK(FloatAt(back.effects[0].values, 0) == doctest::Approx(0.5f));
    CHECK(back.effects[0].propertyOffsets.Size() == 3u);
}

TEST_CASE("material resource: with the shader system, the factory gives a material the effects its shader declares")
{
    GlobalTypeRegistry().Register(MaterialSource::StaticType());
    RegisterSerializable<MaterialSource>();
    GlobalTypeRegistry().Register(Material::StaticType());

    NativeFileSystem mount(u8"scratch_mat_effect_db", DefaultAllocator());
    Guid swaysId, stillId;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        // A PBR material saved without a group (made where no table was at hand).
        RefPtr<Material> pbr = CreatePBR(u8"sways");
        MaterialSource sways;
        MaterialSource::FromMaterial(*pbr, Guid{}, sways);
        sways.shaderName = String(u8"forward");
        auto* a = db.RootGroup()->CreateInstance(u8"sways", MaterialSource::StaticType());
        swaysId = a->Id();
        REQUIRE(a->WriteObject(sways).IsOk());
        // A material carrying a group on a shader that does not declare WIND.
        RefPtr<Material> still = CreateUnlit(u8"still", Float4{1, 1, 1, 1}, u8"still_shader");
        still->AddEffectGroup(WindTable());
        MaterialSource stillSource;
        MaterialSource::FromMaterial(*still, Guid{}, stillSource);
        auto* b = db.RootGroup()->CreateInstance(u8"still", MaterialSource::StaticType());
        stillId = b->Id();
        REQUIRE(b->WriteObject(stillSource).IsOk());
    }

    // A game's shader system: the pack's lattices and effect tables, no compiler.
    shaders::CookedShaderPack pack;
    String error;
    REQUIRE(pack.AddEffect(WindTable(), error));
    pack.AddDeclaredMask(u8"forward", shaders::ShaderStage::Vertex,
                         shaders::ShaderFlags::Skinned | shaders::ShaderFlags::Wind);
    pack.AddDeclaredMask(u8"still_shader", shaders::ShaderStage::Vertex, shaders::ShaderFlags::Skinned);
    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::ShaderSystem system(device);
    system.SetCookedPack(&pack);
    CHECK(system.DeclaredVariants(u8"forward", shaders::ShaderStage::Vertex) ==
          (shaders::ShaderFlags::Skinned | shaders::ShaderFlags::Wind));

    {
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        MaterialFactory factory(DefaultAllocator(), &system);
        ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        Proxy<Material> sways = manager.Bind<Material>(swaysId);
        REQUIRE(sways);
        REQUIRE(sways->EffectGroupCount() == 1u);
        CHECK(sways->GetEffectGroup(0).name == u8"Wind");
        CHECK_FALSE(sways->GetEffectGroup(0).IsActive()); // the effect's defaults: off
        Proxy<Material> still = manager.Bind<Material>(stillId);
        REQUIRE(still);
        CHECK(still->EffectGroupCount() == 0u); // its shader cannot sway
    }
    {
        // Without the shader system a material keeps what its source carries.
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        MaterialFactory factory;
        ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        Proxy<Material> still = manager.Bind<Material>(stillId);
        REQUIRE(still);
        CHECK(still->EffectGroupCount() == 1u);
        Proxy<Material> sways = manager.Bind<Material>(swaysId);
        REQUIRE(sways);
        CHECK(sways->EffectGroupCount() == 0u);
    }

    FileDelete(u8"scratch_mat_effect_db/sways.rasset");
    FileDelete(u8"scratch_mat_effect_db/still.rasset");
    RemoveDirectory(u8"scratch_mat_effect_db");
}

