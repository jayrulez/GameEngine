// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The custom shader cook: one .hlsl file with both stages, its properties block and its variant
// lattices, through ShaderAssetBuilder into an output content DB. A good shader cooks every
// declared variant and writes the expanded source, both entries, both lattices and the property
// table; a broken one fails the cook with its file and line; a flag the file tests but no stage
// declares fails it; the toon sample's real shader, which includes the engine's vertex library,
// cooks against the data root.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.shaders;
import pipeline.core;
import foundation.shaders.resource;
import shaders.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::shaders;

namespace
{
    // A one-file shader: a properties block, a lattice per stage, VSMain and PSMain.
    constexpr const char8_t* kGood =
        u8"// variants(vertex): SKINNED\n"                                           // 1
        u8"// variants(fragment): ALPHA_TEST\n"                                      // 2
        u8"properties\n"                                                             // 3
        u8"{\n"                                                                      // 4
        u8"    Color   Tint     = (1, 0.5, 0);\n"                                    // 5
        u8"    float   Strength = 2 [0, 4];\n"                                       // 6
        u8"    Texture AlbedoMap;\n"                                                 // 7
        u8"    Sampler MainSampler;\n"                                               // 8
        u8"}\n"                                                                      // 9
        u8"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"     // 10
        u8"VSOut VSMain(uint id : SV_VertexID) {\n"                                  // 11
        u8"    VSOut o; o.uv = float2(id & 1, id >> 1);\n"                           // 12
        u8"#ifdef SKINNED\n"                                                         // 13
        u8"    o.uv *= 2.0;\n"                                                       // 14
        u8"#endif\n"                                                                 // 15
        u8"    o.pos = float4(o.uv, 0, 1); return o;\n"                              // 16
        u8"}\n"                                                                      // 17
        u8"float4 PSMain(VSOut input) : SV_Target0 {\n"                              // 18
        u8"    float4 c = AlbedoMap.Sample(MainSampler, input.uv) * Tint * Strength;\n" // 19
        u8"#ifdef ALPHA_TEST\n"                                                      // 20
        u8"    if (c.a < 0.5) discard;\n"                                            // 21
        u8"#endif\n"                                                                 // 22
        u8"    return c;\n"                                                          // 23
        u8"}\n";                                                                     // 24

    // The same shader with a mistake on line 19.
    constexpr const char8_t* kBroken =
        u8"properties\n"
        u8"{\n"
        u8"    float Strength = 2;\n"
        u8"}\n"
        u8"float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n"
        u8"float4 PSMain() : SV_Target0 { return float4(1, 1, 1, 1) * Strenght; }\n"; // 6

    // Tests GBUFFER, which neither stage declares.
    constexpr const char8_t* kUndeclared =
        u8"// variants(fragment): ALPHA_TEST\n"
        u8"float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n"
        u8"float4 PSMain() : SV_Target0 {\n"
        u8"#ifdef GBUFFER\n"
        u8"    return float4(0, 0, 0, 1);\n"
        u8"#endif\n"
        u8"    return float4(1, 1, 1, 1);\n"
        u8"}\n";

    // Whether DXC can be loaded here (the cook compiles with it).
    bool HaveCompiler()
    {
        Compiler* compiler = nullptr;
        if (!createCompiler(CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
        {
            return false;
        }
        compiler->Destroy();
        return true;
    }

    // Collects error lines, so a test can read what the cook reported. One sink for the whole run,
    // registered once and cleared per test: a failed REQUIRE ends a test without unwinding, so a
    // sink on the test's stack would stay registered after it is gone.
    class ErrorSink final : public ILogSink
    {
    public:
        void Write(LogLevel level, StringView, StringView message) noexcept override
        {
            if (level == LogLevel::Error)
            {
                text += message;
                text += u8"\n";
            }
        }
        [[nodiscard]] bool Has(StringView needle) const
        {
            const StringView hay = text.AsView();
            for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
            {
                if (hay.SubStr(i, needle.Size()) == needle)
                {
                    return true;
                }
            }
            return false;
        }
        String text;
    };

    ErrorSink& ErrorLinesSink()
    {
        static ErrorSink sink;
        static const bool registered = [] { GlobalLogger().AddSink(&sink); return true; }();
        (void)registered;
        return sink;
    }

    // A test's view of the error lines logged since it began.
    struct ErrorLines
    {
        ErrorLines() : sink(ErrorLinesSink()) { sink.text.Clear(); }
        [[nodiscard]] bool Has(StringView needle) const { return sink.Has(needle); }
        ErrorSink& sink;
        String& text = sink.text;
    };

    // One cook of `source` saved as `file`: the builder's status, and the product on success.
    struct CookBed
    {
        CookBed()
        {
            GlobalTypeRegistry().Register(ShaderSource::StaticType());
            RegisterSerializable<ShaderSource>();
            RegisterShaderAsset();
            (void)CreateDirectory(u8"scratch_shader_cook");
            (void)CreateDirectory(u8"scratch_shader_cook_db");
        }
        ~CookBed()
        {
            FileDelete(u8"scratch_shader_cook/shader.hlsl");
            FileDelete(u8"scratch_shader_cook_db/shader.rasset");
            RemoveDirectory(u8"scratch_shader_cook");
            RemoveDirectory(u8"scratch_shader_cook_db");
        }

        Status Cook(StringView source, StringView dataRoot = {}, StringView name = u8"custom")
        {
            NativeFileSystem src(u8"scratch_shader_cook", DefaultAllocator());
            REQUIRE(src.Save(u8"shader.hlsl", Span<const byte>(reinterpret_cast<const byte*>(
                                                                   source.Data()),
                                                               source.Size()))
                        .IsOk());
            NativeFileSystem outMount(u8"scratch_shader_cook_db", DefaultAllocator());
            foundation::content::ContentDatabase outDb(DefaultAllocator(), outMount,
                                                       BinarySerializerFactory(), u8".rasset");
            auto* inst = outDb.RootGroup()->CreateInstance(u8"shader", ShaderSource::StaticType());
            id = inst->Id();

            ShaderAsset asset;
            ShaderImporter::Import(name, u8"shader.hlsl", asset);
            ShaderAssetBuilder builder(dataRoot);
            pipeline::AssetBuildContext ctx{DefaultAllocator()};
            ctx.sources = &src;
            ctx.output = inst;
            return builder.Build(asset, ctx);
        }

        RefPtr<ISerializable> Product()
        {
            NativeFileSystem outMount(u8"scratch_shader_cook_db", DefaultAllocator());
            foundation::content::ContentDatabase outDb(DefaultAllocator(), outMount,
                                                       BinarySerializerFactory(), u8".rasset");
            return outDb.ReadObject(id);
        }

        Guid id;
    };

    bool Has(StringView hay, StringView needle)
    {
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            if (hay.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("shader cook: a one-file shader cooks its variants, entries, lattices and table")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    CookBed bed;
    REQUIRE(bed.Cook(kGood).IsOk());
    RefPtr<ISerializable> object = bed.Product();
    const ShaderSource* source = Cast<ShaderSource>(object.Get());
    REQUIRE(source != nullptr);
    CHECK(source->name == u8"custom");
    CHECK(source->vertexEntryPoint == u8"VSMain");
    CHECK(source->fragmentEntryPoint == u8"PSMain");
    CHECK(source->vertexVariantMask == static_cast<u32>(ShaderFlags::Skinned));
    CHECK(source->fragmentVariantMask == static_cast<u32>(ShaderFlags::AlphaTest));
    // The expanded source: the block replaced by its declarations, line numbers kept.
    CHECK_FALSE(Has(source->fragmentSource.AsView(), u8"properties\n{"));
    CHECK(Has(source->fragmentSource.AsView(), u8"cbuffer Material : register(b0, space2)"));
    CHECK(Has(source->fragmentSource.AsView(), u8"#line 10 \"shader.hlsl\""));

    const Array<ShaderProperty> table = source->Properties();
    REQUIRE(table.Size() == 4u);
    CHECK(table[0].name == u8"Tint");
    CHECK(table[0].type == ShaderPropertyType::Color);
    CHECK(table[0].value.w == 1.0f);
    CHECK(table[1].name == u8"Strength");
    CHECK(table[1].offset == 16u);
    CHECK(table[1].hasRange);
    CHECK(table[1].rangeMax == 4.0f);
    CHECK(table[2].type == ShaderPropertyType::Texture);
    CHECK(table[3].type == ShaderPropertyType::Sampler);
    CHECK(source->uniformSize == 32u);
}

TEST_CASE("shader cook: a compile error fails the cook with the file and the author's line")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    CookBed bed;
    ErrorLines errors;
    CHECK_FALSE(bed.Cook(kBroken).IsOk());
    INFO(doctest::String(reinterpret_cast<const char*>(errors.text.CStr())));
    CHECK(errors.Has(u8"shader.hlsl:6:"));
    CHECK(errors.Has(u8"Strenght"));
}

TEST_CASE("shader cook: a malformed properties block fails with its line")
{
    CookBed bed;
    ErrorLines errors;
    CHECK_FALSE(bed.Cook(u8"properties\n{\n    Colour Tint;\n}\n").IsOk());
    INFO(doctest::String(reinterpret_cast<const char*>(errors.text.CStr())));
    CHECK(errors.Has(u8"shader.hlsl:3: unknown property type 'Colour'"));
}

TEST_CASE("shader cook: a flag the file tests but no stage declares fails the cook")
{
    CookBed bed;
    ErrorLines errors;
    CHECK_FALSE(bed.Cook(kUndeclared).IsOk());
    INFO(doctest::String(reinterpret_cast<const char*>(errors.text.CStr())));
    CHECK(errors.Has(u8"tests GBUFFER but no stage declares it"));
}

TEST_CASE("shader cook: a missing source file is an error, not a crash")
{
    RegisterShaderAsset();
    NativeFileSystem outMount(u8"scratch_shader_cook_db", DefaultAllocator());
    foundation::content::ContentDatabase outDb(DefaultAllocator(), outMount,
                                               BinarySerializerFactory(), u8".rasset");
    auto* inst = outDb.RootGroup()->CreateInstance(u8"missing", ShaderSource::StaticType());
    ShaderAsset asset;
    ShaderImporter::Import(u8"missing", u8"does_not_exist.hlsl", asset);
    ShaderAssetBuilder builder;
    NativeFileSystem srcMount(u8"scratch_shader_cook", DefaultAllocator());
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &srcMount;
    ctx.output = inst;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());
    FileDelete(u8"scratch_shader_cook_db/missing.rasset");
    RemoveDirectory(u8"scratch_shader_cook_db");
}

// The toon sample's shader: a low-level shader that includes the engine's forward vertex library
// (under VSMain) and declares the frame's sets itself. It cooks against the data root, every
// variant of both stages.
TEST_CASE("shader cook: the toon sample's shader cooks against the engine's shader library")
{
    const String dataRoot = FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const Result<Array<byte>> toon =
        ReadFile(DataPath(dataRoot.AsView(), u8"Samples/Shaders/toon/toon.hlsl"));
    REQUIRE(toon.HasValue());
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    CookBed bed;
    ErrorLines errors;
    const Status cooked = bed.Cook(
        StringView(reinterpret_cast<const char8_t*>(toon.Value().Data()), toon.Value().Size()),
        dataRoot.AsView());
    INFO(doctest::String(reinterpret_cast<const char*>(errors.text.CStr())));
    REQUIRE(cooked.IsOk());
    RefPtr<ISerializable> object = bed.Product();
    const ShaderSource* source = Cast<ShaderSource>(object.Get());
    REQUIRE(source != nullptr);
    CHECK(source->vertexVariantMask ==
          static_cast<u32>(ShaderFlags::Skinned | ShaderFlags::Instanced));
    CHECK(source->fragmentVariantMask ==
          static_cast<u32>(ShaderFlags::AlphaTest | ShaderFlags::GBuffer | ShaderFlags::Dither));
    CHECK(source->Properties().Size() > 0u);
}

// One name keys a shader in the shader system and a dist's pack, so a project shader named like an
// engine shader would replace the engine's everywhere. The cook refuses the name.
TEST_CASE("shader cook: a project shader cannot take an engine shader's name")
{
    const String dataRoot = FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    CookBed bed;
    ErrorLines errors;
    CHECK_FALSE(bed.Cook(kGood, dataRoot.AsView(), u8"forward").IsOk());
    INFO(doctest::String(reinterpret_cast<const char*>(errors.text.CStr())));
    CHECK(errors.Has(u8"the name 'forward' is the engine's shader"));
}
