// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <cstring>

import foundation.core;
import foundation.shaders;
import foundation.vfs; // FindDataRoot: the engine corpus lives under <data root>/Shaders

using namespace foundation::core;
using namespace foundation::shaders;

namespace
{
    constexpr const char* kVertexHlsl = "float4 main(uint id : SV_VertexID) : SV_Position {\n"
                                        "    return float4(0.0, 0.0, 0.0, 1.0);\n"
                                        "}\n";
}

TEST_CASE("shaders: DXC compiles HLSL to SPIR-V")
{
    Compiler* compiler = nullptr;
    if (!createCompiler(CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping shader compilation test");
        return;
    }

    const auto* source = reinterpret_cast<const u8*>(kVertexHlsl);
    const usize sourceSize = std::strlen(kVertexHlsl);

    CompileResult result{};
    const Status status = compiler->compile(source, sourceSize, ShaderStage::Vertex, u8"main",
                                            ShaderTarget::SPIRV, CompileOptions{}, result);

    CHECK(status.IsOk());
    CHECK(result.success);
    CHECK(result.bytecode != nullptr);
    CHECK(result.bytecodeSize > 0u);
    // SPIR-V magic number (0x07230203) in the first word.
    if (result.bytecode != nullptr && result.bytecodeSize >= 4)
    {
        u32 magic = 0;
        std::memcpy(&magic, result.bytecode, 4);
        CHECK(magic == 0x07230203u);
    }

    compiler->freeResult(result);
    compiler->Destroy();
}

TEST_CASE("shaders: a compile error is reported, not a crash")
{
    Compiler* compiler = nullptr;
    if (!createCompiler(CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        return;
    }

    const char* bad = "this is not valid hlsl @#$";
    CompileResult result{};
    (void)compiler->compile(reinterpret_cast<const u8*>(bad), std::strlen(bad), ShaderStage::Vertex,
                            u8"main", ShaderTarget::SPIRV, CompileOptions{}, result);
    CHECK_FALSE(result.success);

    compiler->freeResult(result); // frees the error `messages` buffer too
    compiler->Destroy();
}

// --- WGSL cook (WgslTranslator: HLSL -> SPIR-V -> naga -> tint) -------------

namespace
{
    // StringView has StartsWith/EndsWith but no substring search; a small scan suffices for tests.
    bool Contains(StringView hay, StringView needle)
    {
        if (needle.Size() > hay.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            if (hay.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    }

    StringView Hlsl(const char* s) { return StringView(reinterpret_cast<const char8_t*>(s)); }

    // A self-contained PS: texture(t0) + sampler(s0) + cbuffer(b0), sampling via SampleLevel
    // (uniformity-safe) so it is browser-conformant. Exercises the binding-shift survival check.
    constexpr const char* kCleanPs =
        "Texture2D Tex : register(t0, space0);\n"
        "SamplerState Samp : register(s0, space0);\n"
        "cbuffer C : register(b0, space0) { float4 Tint; };\n"
        "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
        "    return Tex.SampleLevel(Samp, uv, 0) * Tint;\n"
        "}\n";

    // A PS that samples with implicit derivatives inside a per-pixel branch: legal HLSL, but a WGSL
    // uniformity error that naga only warns on and tint (Chrome/Dawn) rejects.
    constexpr const char* kNonUniformPs =
        "Texture2D Tex : register(t0, space0);\n"
        "SamplerState Samp : register(s0, space0);\n"
        "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
        "    float4 c = float4(0,0,0,1);\n"
        "    if (uv.x > 0.5) { c = Tex.Sample(Samp, uv); }\n"
        "    return c;\n"
        "}\n";

    Compiler* MakeCompiler()
    {
        Compiler* c = nullptr;
        if (!createCompiler(CompilerDesc{}, c).IsOk())
        {
            return nullptr;
        }
        return c;
    }
}

// --- Engine-shader cook (enumerate corpus -> pack) -------------------------

TEST_CASE("cook: the engine corpus cooks to a SPIR-V pack (lint clean, variants expanded)")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping cook test");
        return;
    }
    // The corpus is found the way every executable finds engine data: the discovered data root.
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String shaderDir = foundation::vfs::DataPath(dataRoot.AsView(), u8"Engine/Shaders");

    // SPIR-V only: in-process (no naga/tint spawns), so this stays fast while still exercising
    // enumeration + the drift-lint + every shader compiling. The WGSL path is covered above.
    const CookedShaderFormat formats[] = {CookedShaderFormat::SpirV};
    ShaderCookOptions opts;
    opts.shaderDir = shaderDir.AsView();
    opts.scratchDir = u8".test-scratch";
    opts.formats = Span<const CookedShaderFormat>(formats, 1);

    CookedShaderPack pack;
    const ShaderCookReport report = CookEngineShaders(*compiler, opts, pack);

    for (usize i = 0; i < report.errors.Size(); ++i)
    {
        MESSAGE("cook error: ", reinterpret_cast<const char*>(report.errors[i].CStr()));
    }
    CHECK(report.success); // all shaders compile + no undeclared-flag drift
    CHECK(report.filesCooked > 40u);
    CHECK(report.variantsCooked >= report.filesCooked);

    // forward.ps declares `ALPHA_TEST GBUFFER` -> its power set must be in the pack.
    CHECK(pack.Find(u8"forward", ShaderStage::Fragment, ShaderFlags::None,
                    CookedShaderFormat::SpirV) != nullptr);
    CHECK(pack.Find(u8"forward", ShaderStage::Fragment, ShaderFlags::GBuffer,
                    CookedShaderFormat::SpirV) != nullptr);
    CHECK(pack.Find(u8"forward", ShaderStage::Fragment,
                    ShaderFlags::AlphaTest | ShaderFlags::GBuffer,
                    CookedShaderFormat::SpirV) != nullptr);
    // The builtin Phong material's (`ALPHA_TEST GBUFFER DITHER`, the forward vertex path).
    CHECK(pack.Find(u8"phong", ShaderStage::Fragment, ShaderFlags::GBuffer,
                    CookedShaderFormat::SpirV) != nullptr);
    CHECK(pack.Find(u8"phong", ShaderStage::Fragment, ShaderFlags::AlphaTest | ShaderFlags::GBuffer,
                    CookedShaderFormat::SpirV) != nullptr);
    CHECK(pack.Find(u8"phong", ShaderStage::Vertex, ShaderFlags::Skinned,
                    CookedShaderFormat::SpirV) != nullptr);
    // Every corpus stage records the corpus's entry point, `main`.
    CHECK(pack.EntryPoint(ShaderNameHash(u8"forward"), ShaderStage::Fragment) == kCorpusEntryPoint);
    // A single-variant shader has only its None variant.
    const Array<byte>* tonemapBlob =
        pack.Find(u8"tonemap", ShaderStage::Fragment, ShaderFlags::None, CookedShaderFormat::SpirV);
    REQUIRE(tonemapBlob != nullptr);

    // The SpirV bucket serves BOTH Vulkan and native WebGPU (wgpu-native/naga), and naga
    // rejects SPIR-V 1.4+ - a 1.4+ pack panics wgpu-native at pipeline creation. The cook
    // targets vulkan1.1, so the emitted version word must be <= 1.3 (0x00010300).
    REQUIRE(tonemapBlob->Size() >= 8u);
    u32 spirvVersion = 0;
    std::memcpy(&spirvVersion, tonemapBlob->Data() + 4, 4);
    CHECK(spirvVersion <= 0x00010300u);

    compiler->Destroy();
}

// A project's shader cooks into a dist pack the way the corpus does: each stage under its own
// entry point and lattice, every variant, for every requested format. The toon sample's shader
// (a properties block, VSMain from the engine's vertex library, PSMain) is the real case; WGSL
// passing naga and tint is the web build's proof.
TEST_CASE("cook: a one-file project shader cooks both stages into the pack")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    const String dataRoot = foundation::vfs::FindDataRoot();
    REQUIRE_FALSE(dataRoot.IsEmpty());
    const String shaderDir = foundation::vfs::DataPath(dataRoot.AsView(), u8"Engine/Shaders");
    const Result<Array<byte>> bytes = ReadFile(
        foundation::vfs::DataPath(dataRoot.AsView(), u8"Samples/Shaders/toon/toon.hlsl").AsView());
    REQUIRE(bytes.HasValue());
    const StringView text(reinterpret_cast<const char8_t*>(bytes.Value().Data()),
                          bytes.Value().Size());
    String expanded;
    REQUIRE(ExpandShaderProperties(text, u8"toon.hlsl", expanded).IsOk());

    (void)CreateDirectory(u8".test-scratch");
    WgslTranslator translator(*compiler, u8".test-scratch");
    Array<CookedShaderFormat> formats;
    formats.PushBack(CookedShaderFormat::SpirV);
    if (translator.HasNaga() && translator.HasTint())
    {
        formats.PushBack(CookedShaderFormat::Wgsl);
    }
    ShaderCookOptions opts;
    opts.shaderDir = shaderDir.AsView();
    opts.scratchDir = u8".test-scratch";
    opts.formats = Span<const CookedShaderFormat>(formats.Data(), formats.Size());
    const StringView includePaths[] = {shaderDir.AsView()};

    CookedShaderPack pack;
    ShaderCookReport report;
    const ShaderFlags vertexMask =
        ParseStageVariantDirective(text, ShaderStage::Vertex).mask;
    const ShaderFlags fragmentMask =
        ParseStageVariantDirective(text, ShaderStage::Fragment).mask;
    const bool vertexOk =
        CookShaderStage(*compiler, translator, u8"toon", ShaderStage::Vertex, expanded.AsView(),
                        u8"VSMain", vertexMask, u8"toon.hlsl", opts,
                        Span<const StringView>(includePaths, 1), pack, report);
    const bool fragmentOk =
        CookShaderStage(*compiler, translator, u8"toon", ShaderStage::Fragment, expanded.AsView(),
                        u8"PSMain", fragmentMask, u8"toon.hlsl", opts,
                        Span<const StringView>(includePaths, 1), pack, report);
    for (const String& e : report.errors)
    {
        MESSAGE(doctest::String(reinterpret_cast<const char*>(e.CStr())));
    }
    CHECK(vertexOk);
    CHECK(fragmentOk);

    const u64 toon = ShaderNameHash(u8"toon");
    CHECK(pack.EntryPoint(toon, ShaderStage::Vertex) == StringView(u8"VSMain"));
    CHECK(pack.EntryPoint(toon, ShaderStage::Fragment) == StringView(u8"PSMain"));
    CHECK(pack.DeclaredMask(toon, ShaderStage::Fragment) == fragmentMask);
    // Every variant of every stage, in every format: 4 vertex + 8 fragment per format.
    CHECK(report.variantsCooked == 12u * formats.Size());
    for (const CookedShaderFormat format : formats)
    {
        CHECK(pack.Find(toon, ShaderStage::Vertex, ShaderFlags::Skinned | ShaderFlags::Instanced,
                        format) != nullptr);
        CHECK(pack.Find(toon, ShaderStage::Fragment,
                        ShaderFlags::GBuffer | ShaderFlags::AlphaTest | ShaderFlags::Dither,
                        format) != nullptr);
    }
    if (formats.Size() == 1u)
    {
        MESSAGE("naga or tint not vendored for this host; the WGSL half is skipped");
    }
    compiler->Destroy();
}

TEST_CASE("cook: drift-lint fails a shader that #ifdefs an undeclared flag")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        return;
    }
    // Point the cook at a scratch dir holding one bad shader (declares nothing, #ifdef's GBUFFER).
    (void)CreateDirectory(u8".test-scratch");
    (void)CreateDirectory(u8".test-scratch/badshaders");
    const char* bad = "// variants:\n"
                      "#ifdef GBUFFER\n"
                      "#endif\n"
                      "float4 main() : SV_Target0 { return 0; }\n";
    const Span<const byte> bytes(reinterpret_cast<const byte*>(bad), std::strlen(bad));
    REQUIRE(WriteFile(u8".test-scratch/badshaders/drift.ps.hlsl", bytes).IsOk());

    const CookedShaderFormat formats[] = {CookedShaderFormat::SpirV};
    ShaderCookOptions opts;
    opts.shaderDir = u8".test-scratch/badshaders";
    opts.scratchDir = u8".test-scratch";
    opts.formats = Span<const CookedShaderFormat>(formats, 1);

    CookedShaderPack pack;
    const ShaderCookReport report = CookEngineShaders(*compiler, opts, pack);
    CHECK_FALSE(report.success);
    CHECK_FALSE(report.errors.IsEmpty());

    (void)FileDelete(u8".test-scratch/badshaders/drift.ps.hlsl");
    compiler->Destroy();
}

// --- Include resolution through a resolver (no disk) -------------------------

namespace
{
    // Serves includes from memory and records what the preprocessor asked for.
    class MemoryIncludeResolver final : public IShaderIncludeResolver
    {
    public:
        Array<String> requested;
        bool LoadInclude(StringView path, String& outSource) override
        {
            requested.PushBack(String(path));
            if (path == u8"tint.hlsli")
            {
                outSource = String(u8"#include \"nested/inner.hlsli\"\n"
                                   u8"float4 Tint() { return Inner(); }\n");
                return true;
            }
            if (path == u8"nested/inner.hlsli")
            {
                outSource = String(u8"float4 Inner() { return float4(0, 0, 1, 1); }\n");
                return true;
            }
            return false;
        }
    };
}

TEST_CASE("shaders: includes resolve through CompileOptions::includeResolver, never the disk")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping include resolver test");
        return;
    }
    const char* src = "#include \"tint.hlsli\"\n"
                      "float4 main() : SV_Target { return Tint(); }\n";

    MemoryIncludeResolver resolver;
    CompileOptions opts;
    opts.includeResolver = &resolver;
    // A bogus -I must be ignored (the resolver owns includes): no disk lookup can succeed.
    const StringView bogus[] = {u8"/definitely/not/a/dir"};
    opts.includePaths = Span<const StringView>(bogus, 1);

    CompileResult result{};
    const Status status =
        compiler->compile(reinterpret_cast<const u8*>(src), std::strlen(src), ShaderStage::Fragment,
                          u8"main", ShaderTarget::SPIRV, opts, result);
    if (!result.success)
    {
        MESSAGE("compile messages: ", result.messages != nullptr ? result.messages : "");
    }
    CHECK(status.IsOk());
    CHECK(result.success);
    CHECK(result.bytecodeSize > 0u);
    // The first-level include arrives bare; the nested one carries its own directory.
    bool sawTint = false, sawInner = false;
    for (const String& r : resolver.requested)
    {
        sawTint = sawTint || r.AsView() == u8"tint.hlsli";
        sawInner = sawInner || r.AsView() == u8"nested/inner.hlsli";
    }
    CHECK(sawTint);
    CHECK(sawInner);
    compiler->freeResult(result);

    // A resolver miss is a compile error (reported), not a crash or a silent disk fallback.
    const char* missing = "#include \"absent.hlsli\"\nfloat4 main() : SV_Target { return 0; }\n";
    CompileResult bad{};
    (void)compiler->compile(reinterpret_cast<const u8*>(missing), std::strlen(missing),
                            ShaderStage::Fragment, u8"main", ShaderTarget::SPIRV, opts, bad);
    CHECK_FALSE(bad.success);
    CHECK(bad.messages != nullptr);
    compiler->freeResult(bad);
    compiler->Destroy();
}

// --- Cooked shader pack (serialize / lookup) -------------------------------

TEST_CASE("pack: add, serialize, reload, and look up cooked blobs")
{
    const byte vsSpv[] = {byte{1}, byte{2}, byte{3}, byte{4}};
    const byte psWgsl[] = {byte{'w'}, byte{'g'}, byte{'s'}, byte{'l'}};

    CookedShaderPack pack;
    pack.Add(u8"forward", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
             Span<const byte>(vsSpv, 4));
    pack.Add(u8"forward", ShaderStage::Fragment, ShaderFlags::GBuffer, CookedShaderFormat::Wgsl,
             Span<const byte>(psWgsl, 4));
    pack.AddDeclaredMask(u8"forward", ShaderStage::Fragment,
                         ShaderFlags::AlphaTest | ShaderFlags::GBuffer);
    pack.AddEntryPoint(u8"forward", ShaderStage::Vertex, u8"main");
    pack.AddEntryPoint(u8"forward", ShaderStage::Fragment, u8"PSMain");
    CHECK(pack.Count() == 2u);

    MemoryStream buffer;
    REQUIRE(pack.Write(buffer).IsOk());

    // Reload from the serialized bytes (rewind the stream to the start first).
    REQUIRE(buffer.Seek(0, SeekOrigin::Begin) == 0);
    CookedShaderPack loaded;
    REQUIRE(loaded.Read(buffer).IsOk());
    CHECK(loaded.Count() == 2u);

    const Array<byte>* a =
        loaded.Find(u8"forward", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV);
    REQUIRE(a != nullptr);
    CHECK(a->Size() == 4u);
    CHECK((*a)[0] == byte{1});

    const Array<byte>* b = loaded.Find(u8"forward", ShaderStage::Fragment, ShaderFlags::GBuffer,
                                       CookedShaderFormat::Wgsl);
    REQUIRE(b != nullptr);
    CHECK((*b)[0] == byte{'w'});

    // Wrong variant / format / stage => miss (a cook-coverage bug at runtime).
    CHECK(loaded.Find(u8"forward", ShaderStage::Fragment, ShaderFlags::None,
                      CookedShaderFormat::Wgsl) == nullptr);
    CHECK(loaded.Find(u8"nope", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV) ==
          nullptr);

    Array<String> names;
    loaded.CollectNames(names);
    CHECK(names.Size() == 1u); // one distinct name "forward"

    // Declared mask survives the round-trip (the dist runtime canonicalizes with it).
    CHECK(loaded.DeclaredMask(ShaderNameHash(u8"forward"), ShaderStage::Fragment) ==
          (ShaderFlags::AlphaTest | ShaderFlags::GBuffer));
    CHECK(loaded.DeclaredMask(ShaderNameHash(u8"forward"), ShaderStage::Vertex) ==
          ShaderFlags::None);

    // Each stage's entry point survives too: the runtime gives it to the module it creates.
    CHECK(loaded.EntryPoint(ShaderNameHash(u8"forward"), ShaderStage::Vertex) ==
          StringView(u8"main"));
    CHECK(loaded.EntryPoint(ShaderNameHash(u8"forward"), ShaderStage::Fragment) ==
          StringView(u8"PSMain"));
    CHECK(loaded.EntryPoint(ShaderNameHash(u8"nope"), ShaderStage::Vertex).IsEmpty());
}

// A version 2 pack has no entry points: read as one, every stage would run as `main`, so it is
// refused and the export cooks it again.
TEST_CASE("pack: a pack from before entry points is refused")
{
    MemoryStream buffer;
    {
        BinaryWriter w(buffer);
        w.Write<u32>(0x4b505344u); // "DSPK"
        w.Write<u32>(2u);
        w.Write<u32>(0u); // names
        w.Write<u32>(0u); // declared masks
        w.Write<u32>(0u); // entries
    }
    REQUIRE(buffer.Seek(0, SeekOrigin::Begin) == 0);
    CookedShaderPack loaded;
    CHECK_FALSE(loaded.Read(buffer).IsOk());
}

// --- Variant model (directive parse / canonicalize / power-set / drift-lint) ---

TEST_CASE("variants: directive parse maps flag names to a mask")
{
    const VariantDirective d =
        ParseVariantDirective(Hlsl("// variants: SKINNED INSTANCED\nfloat4 main(){}\n"));
    CHECK(d.present);
    CHECK(HasFlag(d.mask, ShaderFlags::Skinned));
    CHECK(HasFlag(d.mask, ShaderFlags::Instanced));
    CHECK_FALSE(HasFlag(d.mask, ShaderFlags::GBuffer));

    const VariantDirective none = ParseVariantDirective(Hlsl("float4 main(){ return 0; }\n"));
    CHECK_FALSE(none.present);
    CHECK(none.mask == ShaderFlags::None);
}

// One file, two stages, two lattices: each stage takes its tagged line; a stage with none takes
// the untagged form (the corpus's), else no variants.
TEST_CASE("variants: a one-file shader declares each stage's variants")
{
    const StringView oneFile = u8"// variants(vertex): SKINNED INSTANCED\n"
                               u8"// variants(fragment): GBUFFER ALPHA_TEST\n";
    CHECK(ParseStageVariantDirective(oneFile, ShaderStage::Vertex).mask ==
          (ShaderFlags::Skinned | ShaderFlags::Instanced));
    CHECK(ParseStageVariantDirective(oneFile, ShaderStage::Fragment).mask ==
          (ShaderFlags::GBuffer | ShaderFlags::AlphaTest));
    CHECK_FALSE(ParseStageVariantDirective(oneFile, ShaderStage::Compute).present);
    // The untagged corpus form is not mistaken for a tagged one, and serves every stage.
    CHECK_FALSE(ParseVariantDirective(oneFile).present);
    const StringView corpus = u8"// variants: WIND\n";
    CHECK(ParseStageVariantDirective(corpus, ShaderStage::Vertex).mask == ShaderFlags::Wind);
    CHECK(ParseStageVariantDirective(corpus, ShaderStage::Fragment).mask == ShaderFlags::Wind);
}

TEST_CASE("variants: the preserve-interface directive is a per-stage flag")
{
    CHECK(ParsePreserveInterfaceDirective(Hlsl("// variants: HOLES\n// preserve-interface\nvoid main(){}\n")));
    CHECK_FALSE(ParsePreserveInterfaceDirective(Hlsl("// variants: HOLES\nvoid main(){}\n")));
}

namespace
{
    // Count the `OpDecorate <id> Location <n>` instructions in a SPIR-V blob: one per stage
    // interface variable (inputs AND outputs) that survived compilation.
    u32 CountLocationDecorations(const u8* words, usize bytes)
    {
        constexpr u32 kOpDecorate = 71;
        constexpr u32 kDecorationLocation = 30;
        u32 count = 0;
        usize w = 5; // past the 5-word header
        const usize n = bytes / 4;
        while (w < n)
        {
            u32 word = 0;
            std::memcpy(&word, words + w * 4, 4);
            const u32 wordCount = word >> 16;
            const u32 opcode = word & 0xFFFFu;
            if (wordCount == 0)
            {
                break;
            }
            if (opcode == kOpDecorate && wordCount >= 4 && w + 2 < n)
            {
                u32 decoration = 0;
                std::memcpy(&decoration, words + (w + 2) * 4, 4);
                if (decoration == kDecorationLocation)
                {
                    ++count;
                }
            }
            w += wordCount;
        }
        return count;
    }
} // namespace

TEST_CASE("shaders: preserveInterface keeps an unread fragment input in the SPIR-V interface")
{
    // A fragment stage declaring two inputs and reading one: -O3 strips the unread one (the read
    // input + the colour output = two Locations), preserveInterface keeps it (three) - the exact
    // pairing the Vulkan validation wants when the vertex stage is another family's full output list.
    Compiler* compiler = nullptr;
    if (!createCompiler(CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping preserve-interface test");
        return;
    }
    const char* ps = "struct In { float4 pos : SV_Position; float3 a : TEXCOORD0; float2 b : TEXCOORD1; };\n"
                     "float4 main(In i) : SV_Target0 { return float4(i.b, 0, 1); }\n";
    const auto* source = reinterpret_cast<const u8*>(ps);
    const usize size = std::strlen(ps);
    u32 locations[2] = {0, 0};
    for (u32 pass = 0; pass < 2; ++pass)
    {
        CompileOptions opts{};
        opts.preserveInterface = (pass == 1);
        CompileResult result{};
        const Status status = compiler->compile(source, size, ShaderStage::Fragment, u8"main",
                                                ShaderTarget::SPIRV, opts, result);
        REQUIRE(status.IsOk());
        REQUIRE(result.success);
        locations[pass] = CountLocationDecorations(result.bytecode, result.bytecodeSize);
        compiler->freeResult(result);
    }
    compiler->Destroy();
    CHECK(locations[0] == 2u); // stripped: TEXCOORD1 + SV_Target0
    CHECK(locations[1] == 3u); // preserved: TEXCOORD0 too
}

TEST_CASE("variants: canonicalize keeps only declared bits")
{
    const ShaderFlags requested = ShaderFlags::Skinned | ShaderFlags::GBuffer;
    const ShaderFlags declared = ShaderFlags::Skinned; // a VS that only branches on SKINNED
    CHECK(CanonicalizeFlags(requested, declared) == ShaderFlags::Skinned);
    CHECK(CanonicalizeFlags(requested, ShaderFlags::None) == ShaderFlags::None);
}

TEST_CASE("variants: power set enumerates 2^k variants")
{
    Array<ShaderFlags> out;
    EnumerateVariants(ShaderFlags::Skinned | ShaderFlags::Instanced, out);
    CHECK(out.Size() == 4u); // {None, S, I, S|I}
    bool none = false, both = false;
    for (usize i = 0; i < out.Size(); ++i)
    {
        if (out[i] == ShaderFlags::None)
            none = true;
        if (out[i] == (ShaderFlags::Skinned | ShaderFlags::Instanced))
            both = true;
    }
    CHECK(none);
    CHECK(both);

    Array<ShaderFlags> single;
    EnumerateVariants(ShaderFlags::None, single);
    CHECK(single.Size() == 1u); // just None
}

TEST_CASE("variants: drift-lint flags an undeclared #ifdef")
{
    const StringView src = Hlsl("#ifdef GBUFFER\nfloat x;\n#endif\n");
    Array<StringView> undeclared;
    FindUndeclaredFlagUses(src, ShaderFlags::None, undeclared);
    CHECK(undeclared.Size() == 1u);
    CHECK(undeclared[0] == u8"GBUFFER");

    Array<StringView> clean;
    FindUndeclaredFlagUses(src, ShaderFlags::GBuffer, clean); // declared -> no complaint
    CHECK(clean.IsEmpty());
}

// A compile error names the file its author knows, with the line, so an editor or a cook log can
// point at it; without a name DXC reports a placeholder.
TEST_CASE("shaders: a compile error names the source's file and line")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    constexpr const char* kBroken = "float4 main() : SV_Target0 {\n"
                                    "    return float4(1, 0, 0, 1) * Sped;\n"
                                    "}\n";
    CompileOptions options{};
    options.sourceName = u8"Toon.hlsl";
    CompileResult result{};
    (void)compiler->compile(reinterpret_cast<const u8*>(kBroken), std::strlen(kBroken),
                            ShaderStage::Fragment, u8"main", ShaderTarget::SPIRV, options, result);
    CHECK_FALSE(result.success);
    REQUIRE(result.messages != nullptr);
    const StringView messages(reinterpret_cast<const char8_t*>(result.messages));
    CAPTURE(messages);
    CHECK(Contains(messages, u8"Toon.hlsl:2:"));
    CHECK(Contains(messages, u8"Sped"));
    compiler->freeResult(result);
    compiler->Destroy();
}

TEST_CASE("wgsl cook: HLSL translates to WGSL with the binding shifts preserved")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping WGSL cook test");
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    WgslTranslator translator(*compiler, u8".test-scratch");
    if (!translator.HasNaga())
    {
        MESSAGE("naga not vendored for this host; skipping WGSL cook test");
        compiler->Destroy();
        return;
    }

    const WgslCookResult r = translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment, u8"main");
    CHECK(r.success);
    CHECK(r.failedStage == WgslCookStage::Ok);
    // Standard() shift scheme: SRV t0 -> +100, Sampler s0 -> +300, CBV b0 -> 0.
    CHECK(Contains(r.wgsl.AsView(), u8"@binding(100)"));
    CHECK(Contains(r.wgsl.AsView(), u8"@binding(300)"));
    CHECK(Contains(r.wgsl.AsView(), u8"@binding(0)"));

    compiler->Destroy();
}

namespace
{
    // One file, both stages, each its own entry point: what a one-file custom shader is.
    constexpr const char* kTwoEntryHlsl =
        "cbuffer C : register(b0, space0) { float4 Tint; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);\n"
        "    return o;\n"
        "}\n"
        "float4 PSMain(VSOut input) : SV_Target0 {\n"
        "    return float4(input.uv, 0.0, 1.0) * Tint;\n"
        "}\n";

    // The names of a SPIR-V module's OpEntryPoint instructions (opcode 15: execution model, id,
    // then the name as a nul-terminated literal string).
    Array<String> SpirvEntryPointNames(const void* code, usize size)
    {
        Array<String> names;
        const usize count = size / 4;
        Array<u32> words;
        words.Resize(count);
        std::memcpy(words.Data(), code, count * 4);
        for (usize i = 5; i < count;)
        {
            const u32 wordCount = words[i] >> 16;
            const u32 opcode = words[i] & 0xFFFFu;
            if (wordCount == 0)
            {
                break;
            }
            if (opcode == 15u && wordCount > 3)
            {
                const char* name = reinterpret_cast<const char*>(&words[i + 3]);
                names.PushBack(String(StringView(reinterpret_cast<const char8_t*>(name),
                                                 std::strlen(name))));
            }
            i += wordCount;
        }
        return names;
    }
}

// A custom shader is one file whose stages are named functions, not a `main` per file. The name
// survives every target: SPIR-V carries it as the entry point, DXIL compiles from it, and the WGSL
// cook keeps it (naga takes SPIR-V's entry name), so a pipeline can ask each backend for the name
// the source declares.
TEST_CASE("shaders: one file's VSMain and PSMain keep their names in SPIR-V, DXIL and WGSL")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    const StringView source = Hlsl(kTwoEntryHlsl);
    const struct
    {
        ShaderStage stage;
        StringView entry;
    } stages[2] = {{ShaderStage::Vertex, u8"VSMain"}, {ShaderStage::Fragment, u8"PSMain"}};

    for (const auto& s : stages)
    {
        CAPTURE(s.entry);
        CompileResult spirv{};
        REQUIRE(compiler
                    ->compile(reinterpret_cast<const u8*>(source.Data()), source.Size(), s.stage,
                              s.entry, ShaderTarget::SPIRV, CompileOptions{}, spirv)
                    .IsOk());
        REQUIRE(spirv.success);
        const Array<String> names = SpirvEntryPointNames(spirv.bytecode, spirv.bytecodeSize);
        REQUIRE(names.Size() == 1u);
        CHECK(names[0].AsView() == s.entry);
        compiler->freeResult(spirv);

        CompileResult dxil{};
        (void)compiler->compile(reinterpret_cast<const u8*>(source.Data()), source.Size(), s.stage,
                                s.entry, ShaderTarget::DXIL, CompileOptions{}, dxil);
        CHECK(dxil.success);
        compiler->freeResult(dxil);
    }

    (void)CreateDirectory(u8".test-scratch");
    WgslTranslator translator(*compiler, u8".test-scratch");
    if (translator.HasNaga() && translator.HasTint())
    {
        for (const auto& s : stages)
        {
            CAPTURE(s.entry);
            const WgslCookResult r = translator.Translate(source, s.stage, s.entry);
            CHECK_MESSAGE(r.success, r.error); // naga translated it and tint accepted it
            String fn(u8"fn ");
            fn += s.entry;
            fn += u8"(";
            CHECK(Contains(r.wgsl.AsView(), fn.AsView()));
            CHECK_FALSE(Contains(r.wgsl.AsView(), u8"fn main("));
        }
    }
    else
    {
        MESSAGE("naga or tint not vendored for this host; the WGSL half is skipped");
    }
    compiler->Destroy();
}

TEST_CASE("wgsl cook: tint rejects a WGSL uniformity violation")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    WgslTranslator translator(*compiler, u8".test-scratch");
    if (!translator.HasNaga())
    {
        compiler->Destroy();
        return;
    }

    const WgslCookResult r = 
        translator.Translate(Hlsl(kNonUniformPs), ShaderStage::Fragment, u8"main");
    if (translator.HasTint())
    {
        // The conformance gate: tint must reject the non-uniform sample at the Validate stage.
        CHECK_FALSE(r.success);
        CHECK(r.failedStage == WgslCookStage::Validate);
        CHECK_FALSE(r.error.IsEmpty());
    }
    else
    {
        // No oracle available: validation is requested but cannot run, which is a HARD
        // failure - a silent skip would ship unvalidated WGSL, the exact class of output
        // browsers reject at runtime. Skipping is an explicit opt-out only.
        MESSAGE("tint not vendored; expecting the validation-unavailable failure");
        CHECK_FALSE(r.success);
        CHECK(r.failedStage == WgslCookStage::Validate);
    }

    compiler->Destroy();
}

TEST_CASE("wgsl cook: missing tint fails validation unless explicitly opted out")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    WgslTranslator translator(*compiler, u8".test-scratch");
    if (!translator.HasNaga())
    {
        compiler->Destroy();
        return;
    }
    translator.SetTintPath(u8""); // simulate a host with no vendored tint

    // Validation requested (the default) but unavailable: loud failure at Validate.
    const WgslCookResult failed = 
        translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment, u8"main");
    CHECK_FALSE(failed.success);
    CHECK(failed.failedStage == WgslCookStage::Validate);
    CHECK_FALSE(failed.error.IsEmpty());

    // The explicit opt-out cooks unvalidated WGSL - the caller owns the risk.
    translator.SetValidateWithTint(false);
    const WgslCookResult optedOut = 
        translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment, u8"main");
    CHECK(optedOut.success);
    CHECK_FALSE(optedOut.wgsl.IsEmpty());

    compiler->Destroy();
}

TEST_CASE("wgsl cook: a missing naga binary is reported, not a crash")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    WgslTranslator translator(*compiler, u8".test-scratch");
    translator.SetNagaPath(u8"scratch_no_such_naga_zzz");

    const WgslCookResult r = translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment, u8"main");
    CHECK_FALSE(r.success);
    CHECK(r.failedStage == WgslCookStage::Translate);
    CHECK_FALSE(r.error.IsEmpty());

    compiler->Destroy();
}

TEST_CASE("cook: drift-lint sees through #include - the .hlsli holds the #ifdef")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        return;
    }
    // The stage file is directive + include only (the real corpus shape: forward.vs.hlsl);
    // the undeclared #ifdef hides in the shared .hlsli. Linting only the stage file's own
    // text would pass this cook and the dist would silently strip the flag.
    (void)CreateDirectory(u8".test-scratch");
    (void)CreateDirectory(u8".test-scratch/inclint");
    const char* stage = "// variants:\n"
                        "#include \"inclint_common.hlsli\"\n";
    const char* inc = "#ifdef GBUFFER\n"
                      "#endif\n"
                      "float4 main() : SV_Target0 { return 0; }\n";
    REQUIRE(WriteFile(u8".test-scratch/inclint/hidden.ps.hlsl",
                      Span<const byte>(reinterpret_cast<const byte*>(stage), std::strlen(stage)))
                .IsOk());
    REQUIRE(WriteFile(u8".test-scratch/inclint/inclint_common.hlsli",
                      Span<const byte>(reinterpret_cast<const byte*>(inc), std::strlen(inc)))
                .IsOk());

    const CookedShaderFormat formats[] = {CookedShaderFormat::SpirV};
    ShaderCookOptions opts;
    opts.shaderDir = u8".test-scratch/inclint";
    opts.scratchDir = u8".test-scratch";
    opts.formats = Span<const CookedShaderFormat>(formats, 1);

    CookedShaderPack pack;
    const ShaderCookReport report = CookEngineShaders(*compiler, opts, pack);
    CHECK_FALSE(report.success);
    CHECK_FALSE(report.errors.IsEmpty());

    (void)FileDelete(u8".test-scratch/inclint/hidden.ps.hlsl");
    (void)FileDelete(u8".test-scratch/inclint/inclint_common.hlsli");
    compiler->Destroy();
}

TEST_CASE("variants: drift-lint ignores comments that merely mention a flag")
{
    // "user-defined" + a whole-word flag name in a COMMENT must NOT trip the substring
    // heuristic and fail the cook; only real #if/#ifdef/#ifndef/#elif lines count.
    const char8_t* src = u8"// GBUFFER is a user-defined marker, see docs\n"
                         u8"/* undefined behavior when SKINNED */\n"
                         u8"#  ifdef ALPHA_TEST\n"
                         u8"#endif\n"
                         u8"float4 main() : SV_Target0 { return 0; }\n";
    Array<StringView> undeclared;
    FindUndeclaredFlagUses(StringView(src), ShaderFlags::None, undeclared);
    REQUIRE(undeclared.Size() == 1u); // only the real conditional (whitespace after '#' is legal)
    CHECK(undeclared[0] == u8"ALPHA_TEST");
}

TEST_CASE("pack: re-adding a variant overwrites in place (no orphaned duplicates)")
{
    const byte one[] = {byte{1}};
    const byte two[] = {byte{2}};
    CookedShaderPack pack;
    pack.Add(u8"dup", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
             Span<const byte>(one, 1));
    pack.Add(u8"dup", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
             Span<const byte>(two, 1));
    CHECK(pack.Count() == 1u);
    const Array<byte>* blob =
        pack.Find(u8"dup", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV);
    REQUIRE(blob != nullptr);
    CHECK((*blob)[0] == byte{2});
}

TEST_CASE("pack: a corrupt/truncated stream fails cleanly (count guards)")
{
    // A valid header followed by a garbage entry count must not attempt a giant Resize -
    // the counts are bounded by the bytes actually remaining in the stream.
    CookedShaderPack valid;
    const byte blob[] = {byte{9}};
    valid.Add(u8"x", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
              Span<const byte>(blob, 1));
    MemoryStream buffer;
    REQUIRE(valid.Write(buffer).IsOk());

    // Corrupt the name-count field (bytes 8..11, after magic+version) to a huge value.
    REQUIRE(buffer.Seek(8, SeekOrigin::Begin) == 8);
    const u32 huge = 0x7FFFFFFFu;
    REQUIRE(buffer.Write(&huge, sizeof(huge)) == sizeof(huge));
    REQUIRE(buffer.Seek(0, SeekOrigin::Begin) == 0);

    CookedShaderPack loaded;
    CHECK_FALSE(loaded.Read(buffer).IsOk());

    // Truncated stream: only the first 16 bytes of a valid pack.
    MemoryStream full;
    REQUIRE(valid.Write(full).IsOk());
    REQUIRE(full.Seek(0, SeekOrigin::Begin) == 0);
    byte head[16];
    REQUIRE(full.Read(head, 16) == 16);
    MemoryStream truncated;
    REQUIRE(truncated.Write(head, 16) == 16);
    REQUIRE(truncated.Seek(0, SeekOrigin::Begin) == 0);
    CookedShaderPack fromTruncated;
    CHECK_FALSE(fromTruncated.Read(truncated).IsOk());
}

// --- Effect blocks through the compiler, the cook and the pack -----------------------------
// (Specs/wind-for-every-shader.md: an include's `effect` block is expanded by the include
// handler on the cook's disk route and the editor's resolver route alike.)

namespace
{
    constexpr const char* kWindEffect = "// a vertex effect's parameters\n"     // 1
                                        "effect Wind : WIND\n"                  // 2
                                        "{\n"                                   // 3
                                        "    float Strength = 0 [0, 2] (gate);\n" // 4
                                        "    float Speed = 1 [0, 20];\n"        // 5
                                        "    float Height = 1 [0.01, 100];\n"   // 6
                                        "}\n"                                   // 7
                                        "float WindAmount() { return Strength * Speed; }\n";

    // An effect whose third line declares a texture, which an effect may not.
    constexpr const char* kBadEffect = "effect Wind : WIND\n"
                                       "{\n"
                                       "    Texture Gusts;\n"
                                       "    float Strength (gate);\n"
                                       "}\n";

    // Reads the effect's members by their declared names.
    constexpr const char* kReadsWind = "#include \"fx_wind.hlsli\"\n"
                                       "float4 main() : SV_Target {\n"
                                       "    return float4(Strength, Speed, Height, WindAmount());\n"
                                       "}\n";

    void WriteScratch(StringView path, const char* text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text),
                                                 std::strlen(text)))
                    .IsOk());
    }

    // Compiles `source` as a fragment stage; returns success and the messages.
    bool CompileFragment(Compiler& compiler, const char* source, const CompileOptions& options,
                         String& messages)
    {
        CompileResult result{};
        (void)compiler.compile(reinterpret_cast<const u8*>(source), std::strlen(source),
                               ShaderStage::Fragment, u8"main", ShaderTarget::SPIRV, options,
                               result);
        messages = String(StringView(reinterpret_cast<const char8_t*>(
            result.messages != nullptr ? result.messages : "")));
        const bool ok = result.success;
        compiler.freeResult(result);
        return ok;
    }

    // Serves the effect headers from memory.
    class EffectIncludeResolver final : public IShaderIncludeResolver
    {
    public:
        bool LoadInclude(StringView path, String& outSource) override
        {
            if (path == u8"fx_wind.hlsli")
            {
                outSource = String(Hlsl(kWindEffect));
                return true;
            }
            if (path == u8"fx_bad.hlsli")
            {
                outSource = String(Hlsl(kBadEffect));
                return true;
            }
            return false;
        }
    };
}

TEST_CASE("effects: an include's effect block compiles from disk, beside its includer or on -I")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    (void)CreateDirectory(u8".test-scratch/fxdisk");
    (void)CreateDirectory(u8".test-scratch/fxdisk/sub");
    WriteScratch(u8".test-scratch/fxdisk/fx_wind.hlsli", kWindEffect);
    // Only beside its includer (sub/), never on the search path: found relative to the includer
    // first, as DXC's own handler did, and its effect expanded there too.
    WriteScratch(u8".test-scratch/fxdisk/sub/fx_sibling.hlsli",
                 "effect Fade : DITHER\n{\n    float Amount = 1 (gate);\n}\n");
    WriteScratch(u8".test-scratch/fxdisk/sub/fx_nested.hlsli",
                 "#include \"fx_sibling.hlsli\"\nfloat FadeAmount() { return Amount; }\n");

    const StringView includePaths[] = {u8".test-scratch/fxdisk"};
    CompileOptions options;
    options.includePaths = Span<const StringView>(includePaths, 1);
    options.sourceName = u8"reads_wind.hlsl";

    String messages;
    CHECK(CompileFragment(*compiler, kReadsWind, options, messages));
    INFO(doctest::String(reinterpret_cast<const char*>(messages.CStr())));

    constexpr const char* kNested = "#include \"sub/fx_nested.hlsli\"\n"
                                    "float4 main() : SV_Target { return FadeAmount(); }\n";
    CHECK(CompileFragment(*compiler, kNested, options, messages));

    // An include without an effect block is served as it was read.
    WriteScratch(u8".test-scratch/fxdisk/fx_plain.hlsli", "float Plain() { return 2; }\n");
    constexpr const char* kPlain = "#include \"fx_plain.hlsli\"\n"
                                   "float4 main() : SV_Target { return Plain(); }\n";
    CHECK(CompileFragment(*compiler, kPlain, options, messages));

    // A missing include is still a reported error.
    constexpr const char* kMissing = "#include \"fx_absent.hlsli\"\n"
                                     "float4 main() : SV_Target { return 0; }\n";
    CHECK_FALSE(CompileFragment(*compiler, kMissing, options, messages));
    compiler->Destroy();
}

TEST_CASE("effects: an include's effect block compiles through a resolver")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    EffectIncludeResolver resolver;
    CompileOptions options;
    options.includeResolver = &resolver;
    String messages;
    CHECK(CompileFragment(*compiler, kReadsWind, options, messages));
    INFO(doctest::String(reinterpret_cast<const char*>(messages.CStr())));
    compiler->Destroy();
}

TEST_CASE("effects: an effect block in the source itself is expanded too")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    constexpr const char* kTop = "effect Wind : WIND\n"                  // 1
                                 "{\n"                                   // 2
                                 "    float Strength = 0 (gate);\n"      // 3
                                 "}\n"                                   // 4
                                 "float4 main() : SV_Target {\n"         // 5
                                 "    return Strength * Sped;\n"         // 6: the mistake
                                 "}\n";
    CompileOptions options;
    options.sourceName = u8"Top.hlsl";
    String messages;
    CHECK_FALSE(CompileFragment(*compiler, kTop, options, messages));
    INFO(doctest::String(reinterpret_cast<const char*>(messages.CStr())));
    CHECK(Contains(messages.AsView(), u8"Top.hlsl:6:")); // the line kept after the block
    CHECK(Contains(messages.AsView(), u8"Sped"));
    CHECK_FALSE(Contains(messages.AsView(), u8"'Strength'")); // the block's member is declared
    compiler->Destroy();
}

TEST_CASE("effects: a malformed effect in an include fails the compile naming its file and line")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    constexpr const char* kReadsBad = "#include \"fx_bad.hlsli\"\n"
                                      "float4 main() : SV_Target { return Strength; }\n";
    // Disk.
    (void)CreateDirectory(u8".test-scratch");
    (void)CreateDirectory(u8".test-scratch/fxdisk");
    WriteScratch(u8".test-scratch/fxdisk/fx_bad.hlsli", kBadEffect);
    const StringView includePaths[] = {u8".test-scratch/fxdisk"};
    CompileOptions disk;
    disk.includePaths = Span<const StringView>(includePaths, 1);
    String messages;
    CHECK_FALSE(CompileFragment(*compiler, kReadsBad, disk, messages));
    INFO(doctest::String(reinterpret_cast<const char*>(messages.CStr())));
    CHECK(Contains(messages.AsView(), u8"fx_bad.hlsli:3:"));
    CHECK(Contains(messages.AsView(), u8"uniforms only"));

    // A resolver.
    EffectIncludeResolver resolver;
    CompileOptions resolved;
    resolved.includeResolver = &resolver;
    String resolvedMessages;
    CHECK_FALSE(CompileFragment(*compiler, kReadsBad, resolved, resolvedMessages));
    INFO(doctest::String(reinterpret_cast<const char*>(resolvedMessages.CStr())));
    CHECK(Contains(resolvedMessages.AsView(), u8"fx_bad.hlsli:3:"));
    CHECK(Contains(resolvedMessages.AsView(), u8"uniforms only"));
    compiler->Destroy();
}

TEST_CASE("cook: the corpus's effect blocks are stored in the pack, and a stage reading one cooks")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    (void)CreateDirectory(u8".test-scratch/fxcook");
    (void)CreateDirectory(u8".test-scratch/fxcook/surface");
    WriteScratch(u8".test-scratch/fxcook/fx_wind.hlsli", kWindEffect);
    // An include in a subfolder is scanned too (the corpus's surface/ templates live there).
    WriteScratch(u8".test-scratch/fxcook/surface/fx_fade.hlsli",
                 "effect Fade : DITHER\n{\n    float Amount = 1 (gate);\n}\n");
    WriteScratch(u8".test-scratch/fxcook/fxsway.vs.hlsl",
                 "// variants: WIND\n"
                 "#include \"fx_wind.hlsli\"\n"
                 "float4 main(uint id : SV_VertexID) : SV_Position {\n"
                 "#ifdef WIND\n"
                 "    return float4(Strength, Speed, Height, 1);\n"
                 "#else\n"
                 "    return float4(0, 0, 0, 1);\n"
                 "#endif\n"
                 "}\n");

    const CookedShaderFormat formats[] = {CookedShaderFormat::SpirV};
    ShaderCookOptions opts;
    opts.shaderDir = u8".test-scratch/fxcook";
    opts.scratchDir = u8".test-scratch";
    opts.formats = Span<const CookedShaderFormat>(formats, 1);

    CookedShaderPack pack;
    const ShaderCookReport report = CookEngineShaders(*compiler, opts, pack);
    for (const String& e : report.errors)
    {
        MESSAGE(doctest::String(reinterpret_cast<const char*>(e.CStr())));
    }
    CHECK(report.success);
    CHECK(report.variantsCooked == 2u); // None and WIND
    CHECK(pack.Find(u8"fxsway", ShaderStage::Vertex, ShaderFlags::Wind,
                    CookedShaderFormat::SpirV) != nullptr);

    REQUIRE(pack.Effects().Size() == 2u);
    const ShaderEffect* wind = pack.Effects().Find(u8"Wind");
    REQUIRE(wind != nullptr);
    CHECK(wind->flag == ShaderFlags::Wind);
    CHECK(wind->gateIndex == 0u);
    CHECK(wind->uniformSize == 16u);
    REQUIRE(wind->properties.Size() == 3u);
    CHECK(wind->properties[2].name.AsView() == Hlsl("Height"));
    CHECK(wind->properties[2].offset == 8u);
    CHECK(pack.Effects().ForFlag(ShaderFlags::Dither) == pack.Effects().Find(u8"Fade"));

    (void)FileDelete(u8".test-scratch/fxcook/fxsway.vs.hlsl");
    (void)FileDelete(u8".test-scratch/fxcook/fx_wind.hlsli");
    (void)FileDelete(u8".test-scratch/fxcook/surface/fx_fade.hlsli");
    compiler->Destroy();
}

TEST_CASE("cook: two effects with one name or one flag, or a malformed one, fail the cook")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    (void)CreateDirectory(u8".test-scratch");
    (void)CreateDirectory(u8".test-scratch/fxdup");
    WriteScratch(u8".test-scratch/fxdup/a.hlsli", kWindEffect);
    WriteScratch(u8".test-scratch/fxdup/b.hlsli",
                 "effect Gust : WIND\n{\n    float Amount = 1 (gate);\n}\n");
    WriteScratch(u8".test-scratch/fxdup/c.hlsli", kBadEffect);

    const CookedShaderFormat formats[] = {CookedShaderFormat::SpirV};
    ShaderCookOptions opts;
    opts.shaderDir = u8".test-scratch/fxdup";
    opts.scratchDir = u8".test-scratch";
    opts.formats = Span<const CookedShaderFormat>(formats, 1);

    CookedShaderPack pack;
    const ShaderCookReport report = CookEngineShaders(*compiler, opts, pack);
    CHECK_FALSE(report.success);
    bool sharedFlag = false;
    bool badLine = false;
    for (const String& e : report.errors)
    {
        sharedFlag = sharedFlag || (Contains(e.AsView(), u8"b.hlsli:1") &&
                                    Contains(e.AsView(), u8"share a variant flag"));
        badLine = badLine || Contains(e.AsView(), u8"c.hlsli:3: ");
    }
    CHECK(sharedFlag);
    CHECK(badLine);
    CHECK(pack.Effects().Size() == 1u); // the first Wind only

    (void)FileDelete(u8".test-scratch/fxdup/a.hlsli");
    (void)FileDelete(u8".test-scratch/fxdup/b.hlsli");
    (void)FileDelete(u8".test-scratch/fxdup/c.hlsli");
    compiler->Destroy();
}

TEST_CASE("pack: effect tables survive the round trip")
{
    CookedShaderPack pack;
    String error;
    REQUIRE(pack.AddEffect(ParseShaderEffect(Hlsl(kWindEffect)).effect, error));
    CHECK_FALSE(pack.AddEffect(ParseShaderEffect(Hlsl(kWindEffect)).effect, error));

    MemoryStream buffer;
    REQUIRE(pack.Write(buffer).IsOk());
    REQUIRE(buffer.Seek(0, SeekOrigin::Begin) == 0);
    CookedShaderPack loaded;
    REQUIRE(loaded.Read(buffer).IsOk());

    REQUIRE(loaded.Effects().Size() == 1u);
    const ShaderEffect* wind = loaded.Effects().ForFlag(ShaderFlags::Wind);
    REQUIRE(wind != nullptr);
    CHECK(wind->name.AsView() == Hlsl("Wind"));
    CHECK(wind->gateIndex == 0u);
    CHECK(wind->uniformSize == 16u);
    REQUIRE(wind->properties.Size() == 3u);
    const ShaderProperty& height = wind->properties[2];
    CHECK(height.name.AsView() == Hlsl("Height"));
    CHECK(height.type == ShaderPropertyType::Float);
    CHECK(height.value.x == 1.0f);
    CHECK(height.hasRange);
    CHECK(height.rangeMin == doctest::Approx(0.01f));
    CHECK(height.rangeMax == 100.0f);
    CHECK(height.line == 6u);
    CHECK(height.offset == 8u);
    CHECK(height.size == 4u);
    CHECK(wind->properties[1].hasRange); // Speed's [0, 20]

    // A pack of the previous version (3, before effect tables) is refused: packs are recooked.
    MemoryStream old;
    {
        BinaryWriter w(old);
        w.Write<u32>(0x4b505344u); // "DSPK"
        w.Write<u32>(3u);
        w.Write<u32>(0u); // names
        w.Write<u32>(0u); // declared masks
        w.Write<u32>(0u); // entry points
        w.Write<u32>(0u); // entries
    }
    REQUIRE(old.Seek(0, SeekOrigin::Begin) == 0);
    CookedShaderPack refused;
    CHECK_FALSE(refused.Read(old).IsOk());
}
