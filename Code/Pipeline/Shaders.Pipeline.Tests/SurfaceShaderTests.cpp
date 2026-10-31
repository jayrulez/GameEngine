// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Surface shaders: a `// surface:` line makes a shader's text
// a Surface() (and optionally a Vertex()) the engine wraps in its templates, as three programs
// (forward, depth, pick) with the template's lattices. Each compiles in every variant against the
// engine's library; an error in the author's text keeps the author's line; the directive's word
// and a commented-out Vertex() are read correctly.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.shaders;
import foundation.shaders.resource;
import shaders.pipeline;

using namespace foundation::core;
using namespace foundation::shaders;
using namespace pipeline;

namespace
{
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

    // Waves: a lit surface that moves its vertices and scrolls a texture.
    constexpr const char8_t* kWaves =
        u8"// surface: lit\n"                                                         // 1
        u8"properties\n"                                                              // 2
        u8"{\n"                                                                       // 3
        u8"    Color   Water  = (0.1, 0.4, 0.8, 1);\n"                                // 4
        u8"    float   Height = 0.2 [0, 1];\n"                                        // 5
        u8"    Texture Ripples;\n"                                                    // 6
        u8"    Sampler MainSampler;\n"                                                // 7
        u8"}\n"                                                                       // 8
        u8"void Surface(SurfaceInput input, inout SurfaceOutput surface) {\n"         // 9
        u8"    float r = Ripples.Sample(MainSampler, input.uv + Time() * 0.1).r;\n"    // 10
        u8"    surface.albedo = Water.rgb * (0.5 + 0.5 * r);\n"                       // 11
        u8"    surface.roughness = 0.2;\n"                                            // 12
        u8"}\n"                                                                       // 13
        u8"void Vertex(inout VertexInput v) {\n"                                      // 14
        u8"    v.position.y += sin(v.position.x * 2.0 + Time()) * Height;\n"          // 15
        u8"}\n";                                                                      // 16

    // A cut-out leaf: unlit, its alpha from a texture, a commented-out Vertex() example.
    constexpr const char8_t* kLeaf =
        u8"// surface: unlit\n"
        u8"properties\n"
        u8"{\n"
        u8"    float   AlphaCutoff = 0.4 [0, 1];\n"
        u8"    Texture Mask;\n"
        u8"    Sampler MainSampler;\n"
        u8"}\n"
        u8"// void Vertex(inout VertexInput v) { }   (none: an example)\n"
        u8"void Surface(SurfaceInput input, inout SurfaceOutput surface) {\n"
        u8"    surface.albedo = input.color.rgb;\n"
        u8"    surface.alpha = Mask.Sample(MainSampler, input.uv).a;\n"
        u8"}\n";

    ShaderCheckOptions LibraryOptions(String& library)
    {
        library = foundation::vfs::DataPath(foundation::vfs::FindDataRoot().AsView(), u8"Engine/Shaders");
        ShaderCheckOptions options;
        options.shaderLibrary = library.AsView();
        return options;
    }
}

TEST_CASE("surface shader: a lit surface with Vertex() is three programs, every variant compiling")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    String library;
    const ShaderCheck check = CheckShaderSource(kWaves, u8"Waves.hlsl", LibraryOptions(library));
    for (const ShaderDiagnostic& d : check.errors)
    {
        INFO(doctest::String(reinterpret_cast<const char*>(FormatShaderDiagnostic(d).CStr())));
        CHECK(false);
    }
    REQUIRE(check.IsOk());
    CHECK(check.surface == SurfaceModel::Lit);
    CHECK(check.hasVertex);
    REQUIRE(check.programs.Size() == 3u);
    CHECK(check.programs[0].suffix.IsEmpty());
    CHECK(check.programs[1].suffix == kDepthProgramSuffix);
    CHECK(check.programs[2].suffix == kPickProgramSuffix);
    CHECK(check.programs[0].fragmentMask == kSurfaceForwardFragmentMask);
    CHECK(check.programs[1].vertexMask == kSurfaceDepthVertexMask);
    CHECK(Has(check.programs[0].text.AsView(), u8"#define SURFACE_HAS_VERTEX 1"));
    CHECK(Has(check.programs[0].text.AsView(), u8"#line 1 \"Waves.hlsl\""));
    CHECK(check.block.properties.Size() == 4u);
}

TEST_CASE("surface shader: an unlit cut-out uses its AlphaCutoff; a commented Vertex() is none")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    String library;
    const ShaderCheck check = CheckShaderSource(kLeaf, u8"Leaf.hlsl", LibraryOptions(library));
    for (const ShaderDiagnostic& d : check.errors)
    {
        INFO(doctest::String(reinterpret_cast<const char*>(FormatShaderDiagnostic(d).CStr())));
        CHECK(false);
    }
    REQUIRE(check.IsOk());
    CHECK(check.surface == SurfaceModel::Unlit);
    CHECK_FALSE(check.hasVertex);
    CHECK(Has(check.programs[1].text.AsView(), u8"#define SURFACE_HAS_ALPHA_CUTOFF 1"));
    CHECK_FALSE(Has(check.programs[1].text.AsView(), u8"SURFACE_HAS_VERTEX"));
}

TEST_CASE("surface shader: an error in Surface() keeps the author's line")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    String library;
    ShaderCheckOptions options = LibraryOptions(library);
    options.depth = ShaderCheckDepth::BaseVariants;
    const ShaderCheck check = CheckShaderSource(
        u8"// surface: lit\n"
        u8"properties\n{\n    float Glow = 1;\n}\n"
        u8"void Surface(SurfaceInput input, inout SurfaceOutput surface) {\n" // 6
        u8"    surface.emissive = Glw;\n"                                     // 7
        u8"}\n",
        u8"Lava.hlsl", options);
    REQUIRE_FALSE(check.errors.IsEmpty());
    CHECK(check.errors[0].file == u8"Lava.hlsl");
    CHECK(check.errors[0].line == 7u);
}

TEST_CASE("surface shader: the directive's word, variants lines and a missing Surface()")
{
    const ShaderCheck bad = CheckShaderSource(u8"// surface: shiny\n", u8"a.hlsl");
    REQUIRE(bad.errors.Size() == 1u);
    CHECK(bad.errors[0].line == 1u);
    CHECK(Has(bad.errors[0].message.AsView(), u8"'shiny' is not a surface model"));

    const ShaderCheck variants =
        CheckShaderSource(u8"// surface: lit\n// variants(fragment): GBUFFER\n", u8"b.hlsl");
    REQUIRE(variants.errors.Size() == 1u);
    CHECK(Has(variants.errors[0].message.AsView(), u8"variants are the engine's"));

    // A low-level shader stays one program of its own.
    const ShaderCheck low = CheckShaderSource(u8"properties\n{\n    float A = 1;\n}\n",
                                              u8"c.hlsl", ShaderCheckOptions{});
    CHECK(low.surface == SurfaceModel::None);
}

// A property's name is a global in the generated code: one the engine's shaders already declare
// is reported at the property's line, not in the engine's file.
TEST_CASE("surface shader: a property named like an engine declaration is reported at the property")
{
    if (!HaveCompiler())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    String library;
    ShaderCheckOptions options = LibraryOptions(library);
    options.depth = ShaderCheckDepth::BaseVariants;
    const ShaderCheck check = CheckShaderSource(
        u8"// surface: lit\n"
        u8"properties\n{\n    Color Tint = (1, 1, 1, 1);\n}\n"    // Tint: line 4
        u8"void Surface(SurfaceInput input, inout SurfaceOutput surface) { surface.albedo = Tint.rgb; }\n",
        u8"Paint.hlsl", options);
    REQUIRE_FALSE(check.errors.IsEmpty());
    CHECK(check.errors[0].file == u8"Paint.hlsl");
    CHECK(check.errors[0].line == 4u);
    CHECK(Has(check.errors[0].message.AsView(), u8"property 'Tint' takes a name the engine's shaders"));
}
