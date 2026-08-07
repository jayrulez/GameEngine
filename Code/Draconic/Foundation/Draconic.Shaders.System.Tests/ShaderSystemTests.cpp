// Headless tests for the shader variant system: compile-on-demand, flags->defines,
// caching, and invalidation. Compiles real SPIR-V via DXC; creates modules on the
// Null RHI backend (so distinct compiles yield distinct module objects).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

#include <filesystem>
#include <fstream>

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
using namespace draconic::shaders;
namespace rhi = draconic::rhi;

namespace
{
    // Fails to compile unless NORMAL_MAP is defined - proves flags->defines apply.
    constexpr const char8_t* kNeedsNormalMap = u8"float4 main() : SV_Target {\n"
                                               u8"#ifndef NORMAL_MAP\n"
                                               u8"#error NORMAL_MAP required\n"
                                               u8"#endif\n"
                                               u8"    return float4(1, 0, 0, 1);\n"
                                               u8"}\n";

    // Compiles regardless of flags.
    constexpr const char8_t* kTrivialVertex =
        u8"float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n";

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

TEST_CASE("shader system: flags become defines; failures aren't cached")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        ShaderSystem ss(*compiler, device);
        ss.RegisterSource(u8"guarded", ShaderStage::Fragment, kNeedsNormalMap);

        // Without the flag, NORMAL_MAP is undefined -> compile error -> null (not cached).
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::None) == nullptr);

        // With NormalMap -> #define NORMAL_MAP -> compiles.
        rhi::ShaderModule* m =
            ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::NormalMap);
        CHECK(m != nullptr);

        // Same variant is cached (same module object).
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::NormalMap) == m);

        // A different flag doesn't define NORMAL_MAP -> still fails (specific mapping).
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::Emissive) == nullptr);

        // Unknown shader / wrong stage -> null.
        CHECK(ss.GetVariant(u8"missing", ShaderStage::Fragment, ShaderFlags::NormalMap) == nullptr);
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Vertex, ShaderFlags::NormalMap) == nullptr);
    }

    compiler->Destroy();
}

TEST_CASE("shader system: device shader format maps to the cooked-pack format")
{
    CHECK(SelectCookedFormat(rhi::ShaderFormat::SpirV) == CookedShaderFormat::SpirV);
    CHECK(SelectCookedFormat(rhi::ShaderFormat::DXIL) == CookedShaderFormat::Dxil);
    CHECK(SelectCookedFormat(rhi::ShaderFormat::WGSL) == CookedShaderFormat::Wgsl);
}

TEST_CASE("shader system: cooked pack path - blob lookup + canonicalization, no compiler")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        // Cook two SPIR-V variants of "vs" (None, Skinned) into a pack; declared mask = Skinned.
        const StringView src(kTrivialVertex);
        auto spirv = [&](ShaderFlags flags) -> Array<byte>
        {
            Array<ShaderDefine> defs;
            AppendDefines(flags, defs);
            CompileOptions co{};
            co.defines = Span<const ShaderDefine>(defs.Data(), defs.Size());
            co.bindingShifts = BindingShifts::Standard();
            co.bindingShiftSets = 4;
            CompileResult cr{};
            (void)compiler->compile(reinterpret_cast<const u8*>(src.Data()), src.Size(),
                                    ShaderStage::Vertex, u8"main", ShaderTarget::SPIRV, co, cr);
            Array<byte> out;
            if (cr.success)
            {
                out.Resize(cr.bytecodeSize);
                MemCopy(out.Data(), cr.bytecode, cr.bytecodeSize);
            }
            compiler->freeResult(cr);
            return out;
        };

        CookedShaderPack pack;
        const Array<byte> none = spirv(ShaderFlags::None);
        const Array<byte> skin = spirv(ShaderFlags::Skinned);
        REQUIRE_FALSE(none.IsEmpty());
        pack.Add(u8"vs", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
                 Span<const byte>(none.Data(), none.Size()));
        pack.Add(u8"vs", ShaderStage::Vertex, ShaderFlags::Skinned, CookedShaderFormat::SpirV,
                 Span<const byte>(skin.Data(), skin.Size()));
        pack.AddDeclaredMask(u8"vs", ShaderStage::Vertex, ShaderFlags::Skinned);

        ShaderSystem ss(*compiler, device);
        ss.SetCookedPack(&pack);

        // Lookup builds a module directly from the cooked blob (no compile).
        rhi::ShaderModule* m = ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None);
        CHECK(m != nullptr);

        // Canonicalization: only declared bits survive. Skinned|Emissive -> Skinned variant.
        rhi::ShaderModule* s = ss.GetVariant(u8"vs", ShaderStage::Vertex,
                                             ShaderFlags::Skinned | ShaderFlags::Emissive);
        CHECK(s != nullptr);
        CHECK(s != m);

        // Emissive alone is NOT declared -> canonicalizes to None -> dedups onto the None module.
        CHECK(ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::Emissive) == m);

        // A name absent from the pack is a cook-coverage miss -> null (logged).
        CHECK(ss.GetVariant(u8"nope", ShaderStage::Vertex, ShaderFlags::None) == nullptr);

        // Dist boot: a COMPILER-FREE ShaderSystem (no DXC) serves the same pack. This is what a
        // shipped player does when the DXC sidecar was dropped.
        ShaderSystem packOnly(device); // no compiler
        packOnly.SetCookedPack(&pack);
        CHECK(packOnly.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
        CHECK(packOnly.GetVariant(u8"vs", ShaderStage::Vertex,
                                  ShaderFlags::Skinned | ShaderFlags::Emissive) != nullptr);
        // With no compiler and no pack entry, on-demand compilation is unavailable -> null.
        CHECK(packOnly.GetVariant(u8"nope", ShaderStage::Vertex, ShaderFlags::None) == nullptr);
    }

    compiler->Destroy();
}

TEST_CASE("shader system: distinct variants cache separately; invalidate recompiles")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        return;
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        ShaderSystem ss(*compiler, device);
        ss.RegisterSource(u8"vs", ShaderStage::Vertex, kTrivialVertex);

        rhi::ShaderModule* a = ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None);
        rhi::ShaderModule* b = ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::Skinned);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a != b); // different variants
        CHECK(ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None) == a); // cached

        // Invalidate drops the cached variants; a later request recompiles.
        CHECK(ss.InvalidateShader(u8"vs") == 2u);
        rhi::ShaderModule* a2 = ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None);
        CHECK(a2 != nullptr);
    }

    compiler->Destroy();
}

TEST_CASE("shader system: an explicitly registered source beats the cooked pack")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        // A pack that does NOT contain "user_shader" - the shape of a dist with the engine
        // pack loaded and a user shader ASSET (ShaderResource) registered at runtime.
        CookedShaderPack pack;
        const byte bogus[] = {byte{1}};
        pack.Add(u8"engine_only", ShaderStage::Vertex, ShaderFlags::None,
                 CookedShaderFormat::SpirV, Span<const byte>(bogus, 1));

        ShaderSystem ss(*compiler, device);
        ss.SetCookedPack(&pack);
        ss.RegisterSource(u8"user_shader", ShaderStage::Vertex, kTrivialVertex);

        // The registered source compiles even though the system is in pack mode.
        rhi::ShaderModule* m =
            ss.GetVariant(u8"user_shader", ShaderStage::Vertex, ShaderFlags::None);
        CHECK(m != nullptr);
    }
    compiler->Destroy();
}

TEST_CASE("shader system: dev mode canonicalizes corpus requests like the cooked path")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    // A tiny corpus: one shader DECLARING Skinned, one with no directive (single-variant).
    const std::filesystem::path root = "shader_canon_corpus";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream a(root / "declared.vs.hlsl", std::ios::binary);
        a << "// draconic:variants SKINNED\n"
          << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
        std::ofstream b(root / "plain.vs.hlsl", std::ios::binary);
        b << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        FileShaderSourceProvider provider;
        REQUIRE(provider.Initialize(u8"shader_canon_corpus").IsOk());

        ShaderSystem ss(*compiler, device);
        ss.SetSourceProvider(&provider);

        // Declared mask Skinned: a request carrying an extra UNDECLARED flag collapses onto
        // the declared-only variant - the same module object, not a recompile.
        rhi::ShaderModule* declared =
            ss.GetVariant(u8"declared", ShaderStage::Vertex, ShaderFlags::Skinned);
        REQUIRE(declared != nullptr);
        CHECK(ss.GetVariant(u8"declared", ShaderStage::Vertex,
                            ShaderFlags::Skinned | ShaderFlags::Emissive) == declared);
        // And the undeclared-only request collapses onto the None variant.
        rhi::ShaderModule* none =
            ss.GetVariant(u8"declared", ShaderStage::Vertex, ShaderFlags::None);
        REQUIRE(none != nullptr);
        CHECK(ss.GetVariant(u8"declared", ShaderStage::Vertex, ShaderFlags::Emissive) == none);
        CHECK(none != declared);

        // No directive = single-variant: EVERY request lands on the one variant.
        rhi::ShaderModule* plain = ss.GetVariant(u8"plain", ShaderStage::Vertex, ShaderFlags::None);
        REQUIRE(plain != nullptr);
        CHECK(ss.GetVariant(u8"plain", ShaderStage::Vertex,
                            ShaderFlags::Skinned | ShaderFlags::Instanced) == plain);

        // Explicitly REGISTERED sources are outside the corpus model: raw flags still apply
        // (the NormalMap-guarded source only compiles when the define survives).
        ss.RegisterSource(u8"guarded", ShaderStage::Fragment, kNeedsNormalMap);
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::NormalMap) !=
              nullptr);
    }
    compiler->Destroy();
}

TEST_CASE("shader system host: dev-first policy - a nearby pack does not silently win")
{
    rhi::null::NullDevice device{DefaultAllocator()};

    // A usable pack in the CWD (one of the two locations LoadPack scans).
    {
        CookedShaderPack pack;
        const byte blob[] = {byte{1}};
        pack.Add(u8"x", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
                 Span<const byte>(blob, 1));
        FileStream out(u8"shaders.dpak", FileMode::Write);
        REQUIRE(out.IsValid());
        REQUIRE(pack.Write(out).IsOk());
    }
    // A dev source root.
    std::filesystem::create_directories("host_dev_root");
    {
        std::ofstream f("host_dev_root/hosted.vs.hlsl", std::ios::binary);
        f << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    }

    {
        // Automatic: dev is possible (DXC + root), so the pack must NOT take over.
        ShaderSystemHost host;
        if (!host.Initialize(device, u8"host_dev_root"))
        {
            MESSAGE("DXC unavailable; skipping");
            (void)FileDelete(u8"shaders.dpak");
            return;
        }
        CHECK_FALSE(host.UsingPack());
        CHECK(host.GetVariant(u8"hosted", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
    }
    {
        // Explicit opt-in: ForcePack loads a nearby pack. LoadPack scans the EXECUTABLE
        // directory before the cwd, and a dev machine may legitimately have a cooked pack
        // beside the test binary - so assert pack mode, not which pack won.
        ShaderSystemHost host;
        REQUIRE(host.Initialize(device, u8"host_dev_root", ShaderPackPolicy::ForcePack));
        CHECK(host.UsingPack());
        CHECK(host.PackVariantCount() >= 1u);
    }
    {
        // ForceDev ignores the pack even when the root is missing (registered-only mode).
        ShaderSystemHost host;
        REQUIRE(host.Initialize(device, u8"no_such_root_zzz", ShaderPackPolicy::ForceDev));
        CHECK_FALSE(host.UsingPack());
    }

    (void)FileDelete(u8"shaders.dpak");
}
