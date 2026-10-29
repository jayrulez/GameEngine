// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The properties block a custom shader declares its material parameters in: HLSL's cbuffer
// packing, the parse (every type, defaults, ranges, slots), every malformed line reported with
// its line, and the expanded source compiling with the author's line numbers intact.
#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <cstring>

import foundation.core;
import foundation.shaders;

using namespace foundation::core;
using namespace foundation::shaders;

namespace
{
    StringView View(const char* s) { return StringView(reinterpret_cast<const char8_t*>(s)); }

    bool Contains(StringView hay, StringView needle)
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

    // The single error a source yields, or an empty one.
    ShaderPropertyError OnlyError(const char* source)
    {
        const ShaderPropertiesBlock block = ParseShaderProperties(View(source));
        REQUIRE(block.errors.Size() == 1u);
        return block.errors[0];
    }
}

TEST_CASE("properties: HLSL packing - aligned to size, never across a 16-byte row")
{
    u32 cursor = 0;
    CHECK(PackCbufferMember(cursor, 12) == 0u); // float3
    CHECK(PackCbufferMember(cursor, 4) == 12u); // a float shares the float3's row
    CHECK(PackCbufferMember(cursor, 8) == 16u); // float2
    CHECK(PackCbufferMember(cursor, 4) == 24u);
    CHECK(PackCbufferMember(cursor, 8) == 32u); // a float2 at 28 would straddle: moves to 32
    CHECK(PackCbufferMember(cursor, 16) == 48u); // float4 starts a row
    CHECK(CbufferSize(cursor) == 64u);
    u32 odd = 4;
    CHECK(PackCbufferMember(odd, 12) == 4u); // a float3 fits the rest of the row
}

TEST_CASE("properties: every type, its default, ranges and its slot")
{
    const ShaderPropertiesBlock block = ParseShaderProperties(View(
        "// the toon\n"                                   // 1
        "properties\n"                                    // 2
        "{\n"                                             // 3
        "    Color       BaseColor = (1, 0.5, 0.25);\n"   // 4: alpha left out
        "    float       Bands     = 3 [1, 8];\n"         // 5
        "    float3      Light     = (0, 1, 0);\n"        // 6
        "    float       Rim;      // a comment\n"        // 7
        "    float2      Scroll    = (0.5, -1e-1);\n"     // 8
        "    ColorHdr    Glow;\n"                         // 9
        "    int         Steps     = 4 [0, 16];\n"        // 10
        "    Texture     AlbedoMap;\n"                    // 11
        "    TextureCube Env;\n"                          // 12
        "    Sampler     MainSampler;\n"                  // 13
        "}\n"                                             // 14
        "float4 PSMain() : SV_Target0 { return BaseColor; }\n"));
    REQUIRE(block.present);
    CHECK(block.IsOk());
    CHECK(block.firstLine == 2u);
    CHECK(block.lastLine == 14u);
    REQUIRE(block.properties.Size() == 10u);

    const ShaderProperty& base = block.properties[0];
    CHECK(base.type == ShaderPropertyType::Color);
    CHECK(base.value.y == doctest::Approx(0.5f));
    CHECK(base.value.w == 1.0f); // opaque when left out
    CHECK(base.offset == 0u);
    CHECK(base.size == 16u);
    CHECK(base.line == 4u);

    const ShaderProperty& bands = block.properties[1];
    CHECK(bands.value.x == 3.0f);
    CHECK(bands.hasRange);
    CHECK(bands.rangeMax == 8.0f);
    CHECK(bands.offset == 16u);

    CHECK(block.properties[2].offset == 20u); // float3: fits the rest of Bands' row (20..31)
    CHECK(block.properties[3].offset == 32u); // Rim, no default: zero
    CHECK(block.properties[3].value.x == 0.0f);
    CHECK(block.properties[4].offset == 36u); // float2 within its row
    CHECK(block.properties[4].value.y == doctest::Approx(-0.1f));
    CHECK(block.properties[5].type == ShaderPropertyType::ColorHdr);
    CHECK(block.properties[5].offset == 48u); // at 44 it would cross a row
    CHECK(block.properties[5].value.w == 1.0f); // black at intensity 1 by default
    CHECK(block.properties[6].offset == 64u);   // int
    CHECK(block.uniformSize == 80u);

    CHECK(block.properties[7].binding == 0u); // AlbedoMap: t0
    CHECK(block.properties[8].binding == 1u); // Env: t1
    CHECK(block.properties[9].binding == 0u); // MainSampler: s0
    CHECK(block.properties[9].size == 0u);
}

TEST_CASE("properties: a source without a block is a shader without parameters")
{
    const ShaderPropertiesBlock block =
        ParseShaderProperties(View("// properties are not declared here\nfloat4 x;\n"));
    CHECK_FALSE(block.present);
    CHECK(block.IsOk());
    String out;
    (void)ExpandShaderProperties(View("float4 x;\n"), u8"A.hlsl", out);
    CHECK(out.AsView() == View("float4 x;\n"));
}

TEST_CASE("properties: every malformed line is reported with its line")
{
    CHECK(OnlyError("properties {\n  Colour Tint;\n}\n").line == 2u);
    CHECK(Contains(OnlyError("properties {\n  Colour Tint;\n}\n").message.AsView(),
                   u8"unknown property type 'Colour'"));
    CHECK(OnlyError("properties {\n  float;\n}\n").line == 2u);                      // no name
    CHECK(OnlyError("properties {\n  float A;\n  float A;\n}\n").line == 3u);         // twice
    CHECK(OnlyError("properties {\n  float2 A = 1;\n}\n").line == 2u);                // count
    CHECK(OnlyError("properties {\n  Texture A = 1;\n}\n").line == 2u);               // resource
    CHECK(OnlyError("properties {\n  Color A [0, 1];\n}\n").line == 2u);              // range type
    CHECK(OnlyError("properties {\n  float A [2, 1];\n}\n").line == 2u);              // range order
    CHECK(OnlyError("properties {\n  float A = 1\n}\n").line == 2u);                  // no ;
    CHECK(OnlyError("properties {\n  float A; float B;\n}\n").line == 2u);            // two
    CHECK(OnlyError("properties {\n  float A = (1, 2;\n}\n").line == 2u);             // no )
    CHECK(OnlyError("\nproperties {\n  float A;\n").line == 2u);                       // no }
    CHECK(OnlyError("properties\n").line == 1u);                                       // no {
}

// The expanded source is what the compiler sees: the block becomes the set-2 declarations, and
// `#line` keeps every later line's number, so a mistake after the block is reported where its
// author sees it.
TEST_CASE("properties: the expanded source compiles, and keeps the author's line numbers")
{
    Compiler* compiler = nullptr;
    if (!createCompiler(CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    const auto compile = [&](const char* source, String& messages) -> bool
    {
        String expanded;
        const ShaderPropertiesBlock block =
            ExpandShaderProperties(View(source), u8"Toon.hlsl", expanded);
        REQUIRE(block.IsOk());
        CompileOptions options{};
        options.sourceName = u8"Toon.hlsl";
        CompileResult result{};
        (void)compiler->compile(reinterpret_cast<const u8*>(expanded.Data()), expanded.Size(),
                                ShaderStage::Fragment, u8"PSMain", ShaderTarget::SPIRV, options,
                                result);
        messages = String(StringView(reinterpret_cast<const char8_t*>(result.messages != nullptr
                                                                          ? result.messages
                                                                          : "")));
        const bool ok = result.success;
        compiler->freeResult(result);
        return ok;
    };

    String messages;
    CHECK(compile("properties\n"                                                     // 1
                  "{\n"                                                              // 2
                  "    Color   Tint = (1, 0, 0);\n"                                  // 3
                  "    float   Strength = 2;\n"                                      // 4
                  "    Texture AlbedoMap;\n"                                         // 5
                  "    Sampler MainSampler;\n"                                       // 6
                  "}\n"                                                              // 7
                  "float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {\n"            // 8
                  "    return AlbedoMap.Sample(MainSampler, uv) * Tint * Strength;\n" // 9
                  "}\n",
                  messages));
    // A block whose generated declarations take a different number of lines than it does (eight
    // lines become five), so a missing #line would shift the reported line.
    CHECK(compile("properties\n"                               // 1
                  "{\n"                                        // 2
                  "    float Strength = 2;\n"                  // 3
                  "    // a comment\n"                         // 4
                  "\n"                                         // 5
                  "    float Other = 1;\n"                     // 6
                  "    Color Tint;\n"                          // 7
                  "}\n"                                        // 8
                  "float4 PSMain() : SV_Target0 {\n"           // 9
                  "    return Tint * Other * Strenght;\n"       // 10: the mistake
                  "}\n",
                  messages) == false);
    INFO(doctest::String(reinterpret_cast<const char*>(messages.CStr())));
    CHECK(Contains(messages.AsView(), u8"Toon.hlsl:10:"));
    compiler->Destroy();
}

// --- Effect blocks (Specs/wind-for-every-shader.md): the properties grammar in an include, for
// a vertex effect's own parameters, with a header naming the effect and its variant flag. ---

namespace
{
    ShaderPropertyError OnlyEffectError(const char* source)
    {
        const ShaderEffectBlock block = ParseShaderEffect(View(source));
        REQUIRE(block.present);
        REQUIRE(block.errors.Size() == 1u);
        return block.errors[0];
    }
}

TEST_CASE("effects: the block's name, flag, table, offsets and gate")
{
    const ShaderEffectBlock block = ParseShaderEffect(View(
        "// wind\n"                                    // 1
        "effect Wind : WIND\n"                         // 2
        "{\n"                                          // 3
        "    float  Strength  = 0 [0, 2] (gate);\n"    // 4
        "    float  Speed     = 1 [0, 20];\n"          // 5
        "    float  Height    = 1 [0.01, 100]; // m\n" // 6
        "    float3 Direction = (1, 0, 0);\n"          // 7
        "}\n"                                          // 8
        "float Sway() { return Strength; }\n"));
    REQUIRE(block.present);
    CHECK(block.IsOk());
    CHECK(block.firstLine == 2u);
    CHECK(block.lastLine == 8u);
    CHECK(block.effect.name.AsView() == View("Wind"));
    CHECK(block.effect.flag == ShaderFlags::Wind);
    REQUIRE(block.effect.properties.Size() == 4u);
    CHECK(block.effect.gateIndex == 0u);
    REQUIRE(block.effect.Gate() != nullptr);
    CHECK(block.effect.Gate()->name.AsView() == View("Strength"));

    const ShaderProperty& strength = block.effect.properties[0];
    CHECK(strength.offset == 0u);
    CHECK(strength.size == 4u);
    CHECK(strength.hasRange);
    CHECK(strength.rangeMax == 2.0f);
    CHECK(strength.line == 4u);
    CHECK(block.effect.properties[1].offset == 4u);
    CHECK(block.effect.properties[1].value.x == 1.0f);
    CHECK(block.effect.properties[2].offset == 8u);
    CHECK(block.effect.properties[2].rangeMin == doctest::Approx(0.01f));
    CHECK(block.effect.properties[3].offset == 16u); // a float3 at 12 would cross a row
    CHECK(block.effect.uniformSize == 32u);
    CHECK(block.effect.FindProperty(u8"Speed") == &block.effect.properties[1]);
    CHECK(block.effect.FindProperty(u8"WindSpeed") == nullptr);
}

TEST_CASE("effects: a source without an effect block has none, and several are found in order")
{
    CHECK_FALSE(ParseShaderEffect(View("// an effect is described here\nfloat x;\n")).present);
    CHECK_FALSE(HasShaderEffect(View("float effects;\n  effective = 1;\n")));
    CHECK(HasShaderEffect(View("\n  effect A : WIND {\n}\n")));

    const Array<ShaderEffectBlock> blocks = ParseShaderEffects(View(
        "effect Wind : WIND {\n"             // 1
        "    float Strength = 0 (gate);\n"   // 2
        "}\n"                                // 3
        "float x;\n"                         // 4
        "effect Fade : DITHER\n"             // 5
        "{\n"                                // 6
        "    float Amount (gate);\n"         // 7
        "    Color Tint;\n"                  // 8
        "}\n"));
    REQUIRE(blocks.Size() == 2u);
    CHECK(blocks[0].IsOk());
    CHECK(blocks[0].effect.name.AsView() == View("Wind"));
    CHECK(blocks[1].IsOk());
    CHECK(blocks[1].firstLine == 5u);
    CHECK(blocks[1].effect.flag == ShaderFlags::Dither);
    CHECK(blocks[1].effect.properties[1].offset == 16u);
    CHECK(blocks[1].effect.properties[1].value.w == 1.0f); // a colour's default, as in properties
}

TEST_CASE("effects: every malformed header and body is reported with its line")
{
    // The header: a name, a colon, one of the engine's flags.
    CHECK(OnlyEffectError("effect Wind : WINDY {\n  float S = 0 (gate);\n}\n").line == 1u);
    const ShaderPropertyError unknownFlag =
        OnlyEffectError("effect Wind : WINDY {\n  float S (gate);\n}\n");
    CHECK(Contains(unknownFlag.message.AsView(), u8"unknown variant flag 'WINDY'"));
    CHECK(Contains(unknownFlag.message.AsView(), u8"(SKINNED, ")); // the flags it may be
    CHECK(OnlyEffectError("effect Wind WIND {\n  float S (gate);\n}\n").line == 1u); // no colon
    CHECK(Contains(OnlyEffectError("effect Wind WIND {\n  float S (gate);\n}\n").message.AsView(),
                   u8"colon"));
    CHECK(OnlyEffectError("effect : WIND {\n  float S (gate);\n}\n").line == 1u); // no name
    CHECK(Contains(OnlyEffectError("effect : WIND {\n  float S (gate);\n}\n").message.AsView(),
                   u8"needs a name"));
    CHECK(OnlyEffectError("effect Wind : WIND extra {\n  float S (gate);\n}\n").line == 1u);

    // Uniforms only: a texture, a cube or a sampler is an error at its line.
    CHECK(OnlyEffectError("effect W : WIND {\n  float S (gate);\n  Texture T;\n}\n").line == 3u);
    CHECK(Contains(
        OnlyEffectError("effect W : WIND {\n  float S (gate);\n  Texture T;\n}\n").message.AsView(),
        u8"uniforms only"));
    CHECK(OnlyEffectError("effect W : WIND {\n  float S (gate);\n  Sampler T;\n}\n").line == 3u);
    CHECK(OnlyEffectError("effect W : WIND {\n  TextureCube T;\n  float S (gate);\n}\n").line ==
          2u);

    // One gate, a float: none, two, or a gate of another type.
    CHECK(OnlyEffectError("effect W : WIND {\n  float S = 1;\n}\n").line == 1u);
    CHECK(Contains(OnlyEffectError("effect W : WIND {\n  float S = 1;\n}\n").message.AsView(),
                   u8"(gate)"));
    CHECK(OnlyEffectError("effect W : WIND {\n  float A (gate);\n  float B = 1 [0, 1] (gate);\n}\n")
              .line == 3u);
    CHECK(OnlyEffectError("effect W : WIND {\n  int S = 1 (gate);\n}\n").line == 2u);
    CHECK(Contains(OnlyEffectError("effect W : WIND {\n  float2 S (gate);\n}\n").message.AsView(),
                   u8"is a float"));
    CHECK(OnlyEffectError("effect W : WIND {\n  float S (gat);\n}\n").line == 2u);

    // The properties grammar's errors hold in an effect too.
    CHECK(OnlyEffectError("effect W : WIND {\n  float S (gate)\n  float B;\n}\n").line == 2u);
    CHECK(OnlyEffectError("effect W : WIND\n").line == 1u); // no {
}

TEST_CASE("effects: a properties block knows no gate, and keeps its errors as they were")
{
    // `(gate)` belongs to an effect; in a properties block it is the missing `;` it always was.
    CHECK(OnlyError("properties {\n  float A = 0 (gate);\n}\n").line == 2u);
    CHECK(Contains(OnlyError("properties {\n  float A = 0 (gate);\n}\n").message.AsView(),
                   u8"ends with ;"));
    CHECK(Contains(OnlyError("properties\n").message.AsView(), u8"properties needs a {"));
    CHECK(Contains(OnlyError("\nproperties {\n  float A;\n").message.AsView(),
                   u8"the properties block has no closing }"));
    // A properties block still takes its textures; an effect line is not a properties line.
    const ShaderPropertiesBlock block = ParseShaderProperties(View(
        "effect W : WIND {\n  float S (gate);\n}\nproperties {\n  Texture T;\n}\n"));
    CHECK(block.IsOk());
    CHECK(block.firstLine == 4u);
    REQUIRE(block.properties.Size() == 1u);
}

TEST_CASE("effects: the generated cbuffer binds at b1 of the material's set, members as declared")
{
    const ShaderEffectBlock block = ParseShaderEffect(View("effect Wind : WIND\n"
                                                           "{\n"
                                                           "    float Strength = 0 [0, 2] (gate);\n"
                                                           "    float Speed = 1 [0, 20];\n"
                                                           "    float Height = 1 [0.01, 100];\n"
                                                           "}\n"));
    REQUIRE(block.IsOk());
    const String declaration = GenerateEffectDeclaration(block.effect);
    CHECK(declaration.AsView() == View("cbuffer Wind : register(b1, space2) {\n"
                                       "    float Strength; // offset 0\n"
                                       "    float Speed; // offset 4\n"
                                       "    float Height; // offset 8\n"
                                       "};\n"));

    // The expansion replaces the block and keeps the later lines' numbers.
    String expanded;
    const Array<ShaderEffectBlock> blocks = ExpandShaderEffects(
        View("// head\n"
             "effect Wind : WIND\n"
             "{\n"
             "    float Strength = 0 (gate);\n"
             "}\n"
             "float Sway() { return Strength; }\n"),
        u8"wind.hlsli", expanded);
    REQUIRE(blocks.Size() == 1u);
    CHECK(expanded.AsView() == View("// head\n"
                                    "cbuffer Wind : register(b1, space2) {\n"
                                    "    float Strength; // offset 0\n"
                                    "};\n"
                                    "#line 6 \"wind.hlsli\"\n"
                                    "float Sway() { return Strength; }\n"));

    // A block with errors becomes an #error at each error's line, so a compile reaching it
    // fails naming the file and line.
    String broken;
    const Array<ShaderEffectBlock> bad = ExpandShaderEffects(
        View("effect Wind : WIND {\n  float S (gate);\n  Texture T;\n}\nfloat x;\n"),
        u8"wind.hlsli", broken);
    REQUIRE(bad.Size() == 1u);
    CHECK_FALSE(bad[0].IsOk());
    CHECK(Contains(broken.AsView(), u8"#line 3 \"wind.hlsli\"\n#error effect: 'Texture'"));
    CHECK(Contains(broken.AsView(), u8"#line 5 \"wind.hlsli\"\nfloat x;\n"));

    // A source without a block is returned as it is.
    String same;
    CHECK(ExpandShaderEffects(View("float x;\n"), u8"a.hlsli", same).IsEmpty());
    CHECK(same.AsView() == View("float x;\n"));
}

TEST_CASE("effects: a library holds each effect once, by name and by flag")
{
    ShaderEffectLibrary library;
    String error;
    const ShaderEffect wind =
        ParseShaderEffect(View("effect Wind : WIND {\n float S (gate);\n}\n")).effect;
    REQUIRE(library.Add(wind, error));
    CHECK(library.Find(u8"Wind") != nullptr);
    CHECK(library.ForFlag(ShaderFlags::Wind) != nullptr);
    CHECK(library.ForFlag(ShaderFlags::Dither) == nullptr);

    CHECK_FALSE(library.Add(wind, error)); // the same name
    CHECK(Contains(error.AsView(), u8"declared twice"));
    const ShaderEffect gust =
        ParseShaderEffect(View("effect Gust : WIND {\n float S (gate);\n}\n")).effect;
    CHECK_FALSE(library.Add(gust, error)); // the same flag
    CHECK(Contains(error.AsView(), u8"share a variant flag"));
    CHECK(library.Size() == 1u);
}
