#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cstring>

import draconic.foundation;
import draconic.shaders;

using namespace draconic::foundation;
using namespace draconic::shaders;

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

#ifdef DRACONIC_ENGINE_SHADER_DIR
TEST_CASE("cook: the engine corpus cooks to a SPIR-V pack (lint clean, variants expanded)")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping cook test");
        return;
    }

    // SPIR-V only: in-process (no naga/tint spawns), so this stays fast while still exercising
    // enumeration + the drift-lint + every shader compiling. The WGSL path is covered above.
    const CookedShaderFormat formats[] = {CookedShaderFormat::SpirV};
    ShaderCookOptions opts;
    opts.shaderDir = StringView(reinterpret_cast<const char8_t*>(DRACONIC_ENGINE_SHADER_DIR));
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
    const char* bad = "// draconic:variants\n"
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
#endif // DRACONIC_ENGINE_SHADER_DIR

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
}

// --- Variant model (directive parse / canonicalize / power-set / drift-lint) ---

TEST_CASE("variants: directive parse maps flag names to a mask")
{
    const VariantDirective d =
        ParseVariantDirective(Hlsl("// draconic:variants SKINNED INSTANCED\nfloat4 main(){}\n"));
    CHECK(d.present);
    CHECK(HasFlag(d.mask, ShaderFlags::Skinned));
    CHECK(HasFlag(d.mask, ShaderFlags::Instanced));
    CHECK_FALSE(HasFlag(d.mask, ShaderFlags::GBuffer));

    const VariantDirective none = ParseVariantDirective(Hlsl("float4 main(){ return 0; }\n"));
    CHECK_FALSE(none.present);
    CHECK(none.mask == ShaderFlags::None);
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

    const WgslCookResult r = translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment);
    CHECK(r.success);
    CHECK(r.failedStage == WgslCookStage::Ok);
    // Standard() shift scheme: SRV t0 -> +100, Sampler s0 -> +300, CBV b0 -> 0.
    CHECK(Contains(r.wgsl.AsView(), u8"@binding(100)"));
    CHECK(Contains(r.wgsl.AsView(), u8"@binding(300)"));
    CHECK(Contains(r.wgsl.AsView(), u8"@binding(0)"));

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

    const WgslCookResult r = translator.Translate(Hlsl(kNonUniformPs), ShaderStage::Fragment);
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
    const WgslCookResult failed = translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment);
    CHECK_FALSE(failed.success);
    CHECK(failed.failedStage == WgslCookStage::Validate);
    CHECK_FALSE(failed.error.IsEmpty());

    // The explicit opt-out cooks unvalidated WGSL - the caller owns the risk.
    translator.SetValidateWithTint(false);
    const WgslCookResult optedOut = translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment);
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
    translator.SetNagaPath(u8"draconic_no_such_naga_zzz");

    const WgslCookResult r = translator.Translate(Hlsl(kCleanPs), ShaderStage::Fragment);
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
    const char* stage = "// draconic:variants\n"
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
    // "user-defined" + a whole-word flag name in a COMMENT used to trip the substring
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
