// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Headless tests for the shader variant system: compile-on-demand, flags to defines,
// caching, and invalidation. Compiles real SPIR-V via DXC; creates modules on the
// Null RHI backend (so distinct compiles yield distinct module objects).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <filesystem>
#include <fstream>

import foundation.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.shaders;
import foundation.shaders.system;
import foundation.vfs;

using namespace foundation::core;
using namespace foundation::shaders;
namespace rhi = foundation::rhi;

namespace
{
    // Fails to compile unless NORMAL_MAP is defined, which proves flags become defines.
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

TEST_CASE("shader system: flags become defines; a failure is remembered until the source changes")
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
        ss.RegisterSource(u8"guarded", ShaderStage::Fragment, kNeedsNormalMap, kCorpusEntryPoint);

        // Without the flag, NORMAL_MAP is undefined: a compile error, so null (not cached).
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::None) == nullptr);

        // NormalMap defines NORMAL_MAP, so it compiles.
        rhi::ShaderModule* m =
            ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::NormalMap);
        CHECK(m != nullptr);

        // Same variant is cached (same module object).
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::NormalMap) == m);

        // A different flag doesn't define NORMAL_MAP, so it still fails (specific mapping).
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::Emissive) == nullptr);

        // An unknown shader or wrong stage is null.
        CHECK(ss.GetVariant(u8"missing", ShaderStage::Fragment, ShaderFlags::NormalMap) == nullptr);
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Vertex, ShaderFlags::NormalMap) == nullptr);

        // A failed variant is not compiled again until its source changes; a fixed source
        // registered under the same name compiles.
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::None) == nullptr);
        ss.RegisterSource(u8"guarded", ShaderStage::Fragment,
                          u8"float4 main() : SV_Target { return float4(0, 1, 0, 1); }",
                          kCorpusEntryPoint);
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::None) != nullptr);
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
        pack.AddEntryPoint(u8"vs", ShaderStage::Vertex, kCorpusEntryPoint);

        ShaderSystem ss(*compiler, device);
        ss.SetCookedPack(&pack);

        // Lookup builds a module directly from the cooked blob (no compile).
        rhi::ShaderModule* m = ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None);
        CHECK(m != nullptr);

        // Canonicalization: only declared bits survive, so Skinned|Emissive is the Skinned variant.
        rhi::ShaderModule* s = ss.GetVariant(u8"vs", ShaderStage::Vertex,
                                             ShaderFlags::Skinned | ShaderFlags::Emissive);
        CHECK(s != nullptr);
        CHECK(s != m);

        // Emissive alone is not declared, so it canonicalizes to None and dedups onto the None module.
        CHECK(ss.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::Emissive) == m);

        // A name absent from the pack is a cook-coverage miss: null (logged).
        CHECK(ss.GetVariant(u8"nope", ShaderStage::Vertex, ShaderFlags::None) == nullptr);

        // Dist boot: a compiler-free ShaderSystem (no DXC) serves the same pack. This is what a
        // dist player does when the DXC sidecar is absent.
        ShaderSystem packOnly(device); // no compiler
        packOnly.SetCookedPack(&pack);
        CHECK(packOnly.GetVariant(u8"vs", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
        CHECK(packOnly.GetVariant(u8"vs", ShaderStage::Vertex,
                                  ShaderFlags::Skinned | ShaderFlags::Emissive) != nullptr);
        // With no compiler and no pack entry, on-demand compilation is unavailable: null.
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
        ss.RegisterSource(u8"vs", ShaderStage::Vertex, kTrivialVertex, kCorpusEntryPoint);

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
        // A pack that does not contain "user_shader": the shape of a dist with the engine
        // pack loaded and a user shader asset (ShaderResource) registered at runtime.
        CookedShaderPack pack;
        const byte bogus[] = {byte{1}};
        pack.Add(u8"engine_only", ShaderStage::Vertex, ShaderFlags::None,
                 CookedShaderFormat::SpirV, Span<const byte>(bogus, 1));

        ShaderSystem ss(*compiler, device);
        ss.SetCookedPack(&pack);
        ss.RegisterSource(u8"user_shader", ShaderStage::Vertex, kTrivialVertex, kCorpusEntryPoint);

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

    // A tiny corpus: one shader declaring Skinned, one with no directive (single-variant).
    const std::filesystem::path root = "shader_canon_corpus";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream a(root / "declared.vs.hlsl", std::ios::binary);
        a << "// variants: SKINNED\n"
          << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
        std::ofstream b(root / "plain.vs.hlsl", std::ios::binary);
        b << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        foundation::vfs::NativeFileSystem fs(u8"shader_canon_corpus", DefaultAllocator());
        FileShaderSourceProvider provider{DefaultAllocator()};
        REQUIRE(provider.Initialize(fs, u8"").IsOk());

        ShaderSystem ss(*compiler, device);
        ss.SetSourceProvider(&provider);

        // Declared mask Skinned: a request carrying an extra undeclared flag collapses onto
        // the declared-only variant, the same module object, not a recompile.
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

        // No directive means single-variant: every request lands on the one variant.
        rhi::ShaderModule* plain = ss.GetVariant(u8"plain", ShaderStage::Vertex, ShaderFlags::None);
        REQUIRE(plain != nullptr);
        CHECK(ss.GetVariant(u8"plain", ShaderStage::Vertex,
                            ShaderFlags::Skinned | ShaderFlags::Instanced) == plain);

        // Explicitly registered sources are outside the corpus model: raw flags still apply
        // (the NormalMap-guarded source only compiles when the define survives).
        ss.RegisterSource(u8"guarded", ShaderStage::Fragment, kNeedsNormalMap, kCorpusEntryPoint);
        CHECK(ss.GetVariant(u8"guarded", ShaderStage::Fragment, ShaderFlags::NormalMap) !=
              nullptr);
    }
    compiler->Destroy();
}

TEST_CASE("shader system host: dev-first policy - a pack in the data root does not silently win")
{
    namespace vfs = foundation::vfs;
    rhi::null::NullDevice device{DefaultAllocator()};

    // A scratch data root in the production shape: Engine/Shaders/<sources> and its shaders.dpak.
    const std::filesystem::path root = "host_data_root";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Engine" / "Shaders");
    {
        CookedShaderPack pack;
        const byte blob[] = {byte{1}};
        pack.Add(u8"x", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
                 Span<const byte>(blob, 1));
        FileStream out(u8"host_data_root/Engine/Shaders/shaders.dpak", FileMode::Write);
        REQUIRE(out.IsValid());
        REQUIRE(pack.Write(out).IsOk());
    }
    {
        std::ofstream f(root / "Engine" / "Shaders" / "hosted.vs.hlsl", std::ios::binary);
        f << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    }
    {
        std::ofstream f(root / "Engine" / "Shaders" / "hosted_common.hlsli", std::ios::binary);
        f << "float HostedZero() { return 0.0; }\n";
    }
    vfs::NativeFileSystem dataFs(u8"host_data_root", DefaultAllocator());
    CHECK(kShaderPackPath == u8"Engine/Shaders/shaders.dpak"); // the layout every writer targets

    {
        // Automatic: dev is possible (DXC and Engine/Shaders/), so the pack must not take over.
        ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, dataFs))
        {
            MESSAGE("DXC unavailable; skipping");
            return;
        }
        CHECK_FALSE(host.UsingPack());
        CHECK(host.GetVariant(u8"hosted", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
    }
    {
        // A distribution marks itself (Engine/Shaders/.pack-first): Automatic then starts from the pack
        // although the sources and DXC are there; the sources stay for whoever cooks from them.
        {
            std::ofstream marker(root / "Engine" / "Shaders" / ".pack-first", std::ios::binary);
        }
        ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, dataFs));
        CHECK(host.UsingPack());
        CHECK(host.PackVariantCount() == 1u);
        // A registered source (a project's custom shader) still compiles, and its includes of the
        // engine's .hlsli files resolve from the sources beside the pack.
        host.System()->RegisterSource(u8"custom", ShaderStage::Vertex,
                                      u8"#include \"hosted_common.hlsli\"\n"
                                      u8"float4 main(uint id : SV_VertexID) : SV_Position "
                                      u8"{ return float4(HostedZero(), 0, 0, 1); }\n",
                                      u8"main");
        CHECK(host.GetVariant(u8"custom", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
        std::filesystem::remove(root / "Engine" / "Shaders" / ".pack-first");
    }
    {
        // Explicit opt-in: ForcePack loads Engine/Shaders/shaders.dpak from the data root, and
        // only from there (no executable-dir or cwd probing), so it is our one-variant pack.
        ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, dataFs, ShaderPackPolicy::ForcePack));
        CHECK(host.UsingPack());
        CHECK(host.PackVariantCount() == 1u);
    }
    {
        // ForceDev ignores the pack even when the source folder is missing (registered-only
        // mode): a data root with no Engine/Shaders/ at all.
        std::filesystem::create_directories("host_data_root_empty");
        vfs::NativeFileSystem emptyFs(u8"host_data_root_empty", DefaultAllocator());
        ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, emptyFs, ShaderPackPolicy::ForceDev));
        CHECK_FALSE(host.UsingPack());
        CHECK(host.GetVariant(u8"hosted", ShaderStage::Vertex, ShaderFlags::None) == nullptr);
    }
    {
        // Pack mode with no pack present and no compiler path forced: Automatic on an empty
        // root still succeeds (a compiler exists) but serves only registered sources.
        vfs::NativeFileSystem emptyFs(u8"host_data_root_empty", DefaultAllocator());
        ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, emptyFs));
        CHECK_FALSE(host.UsingPack());
    }
}

TEST_CASE("shader system host: a dist data root with ONLY the pack under Shaders/ enters pack mode")
{
    namespace vfs = foundation::vfs;
    rhi::null::NullDevice device{DefaultAllocator()};

    // The layout every dist stages (export and the editor dist scripts): Data/Engine/Shaders/ holds
    // the cooked pack and nothing else, no .hlsl sources. With DXC present (the editor dist ships
    // it) the folder's existence must not read as "sources present", or the shipped editor would
    // run in dev mode over an empty corpus.
    const std::filesystem::path root = "host_dist_root";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Engine" / "Shaders");
    {
        CookedShaderPack pack;
        const byte blob[] = {byte{1}};
        pack.Add(u8"x", ShaderStage::Vertex, ShaderFlags::None, CookedShaderFormat::SpirV,
                 Span<const byte>(blob, 1));
        FileStream out(u8"host_dist_root/Engine/Shaders/shaders.dpak", FileMode::Write);
        REQUIRE(out.IsValid());
        REQUIRE(pack.Write(out).IsOk());
    }
    vfs::NativeFileSystem dataFs(u8"host_dist_root", DefaultAllocator());
    {
        ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, dataFs)); // Automatic
        CHECK(host.UsingPack());
        CHECK(host.PackVariantCount() == 1u);
    }
    // Whether a compiler is available on this machine: a root with one source and no pack initializes
    // only through DXC.
    bool haveCompiler = false;
    {
        std::filesystem::create_directories("host_dist_probe/Shaders");
        std::ofstream f("host_dist_probe/Shaders/probe.vs.hlsl", std::ios::binary);
        f << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
        f.close();
        vfs::NativeFileSystem probeFs(u8"host_dist_probe", DefaultAllocator());
        ShaderSystemHost host{DefaultAllocator()};
        haveCompiler = host.Initialize(device, probeFs);
    }
    {
        // A stage file beside the pack is a corpus again: dev-first (with a compiler), else the
        // pack still serves.
        std::ofstream f(root / "Engine" / "Shaders" / "hosted.vs.hlsl", std::ios::binary);
        f << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
        f.close();
        ShaderSystemHost host{DefaultAllocator()};
        REQUIRE(host.Initialize(device, dataFs));
        CHECK(host.UsingPack() == !haveCompiler);
    }
}

// One file can serve both stages as named functions: each registered stage carries its entry,
// every module made from it records that entry (so a pipeline need not name it), and the cooked
// path takes the entry from the pack.
TEST_CASE("shader system: a stage's entry point is registered with it and recorded on its modules")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    constexpr const char8_t* kOneFile =
        u8"struct VSOut { float4 pos : SV_Position; };\n"
        u8"VSOut VSMain(uint id : SV_VertexID) { VSOut o; o.pos = float4(0, 0, 0, 1); return o; }\n"
        u8"float4 PSMain(VSOut input) : SV_Target0 { return float4(1, 0, 0, 1); }\n";

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        ShaderSystem ss(*compiler, device);
        ss.RegisterSource(u8"one", ShaderStage::Vertex, kOneFile, u8"VSMain");
        ss.RegisterSource(u8"one", ShaderStage::Fragment, kOneFile, u8"PSMain");
        rhi::ShaderModule* vs = ss.GetVariant(u8"one", ShaderStage::Vertex, ShaderFlags::None);
        rhi::ShaderModule* ps = ss.GetVariant(u8"one", ShaderStage::Fragment, ShaderFlags::None);
        REQUIRE(vs != nullptr);
        REQUIRE(ps != nullptr);
        CHECK(vs->entryPoint.AsView() == StringView(u8"VSMain"));
        CHECK(ps->entryPoint.AsView() == StringView(u8"PSMain"));
        // A stage that names none runs its module's own.
        CHECK(rhi::EntryPointOf(rhi::StageOf(vs, rhi::ShaderStage::Vertex)) ==
              StringView(u8"VSMain"));
        CHECK(rhi::EntryPointOf(rhi::ProgrammableStage{vs, u8"Other", rhi::ShaderStage::Vertex}) ==
              StringView(u8"Other"));

        // A wrong entry name is a compile failure, not a module named something it lacks.
        ss.RegisterSource(u8"wrong", ShaderStage::Vertex, kOneFile, u8"NoSuchFunction");
        CHECK(ss.GetVariant(u8"wrong", ShaderStage::Vertex, ShaderFlags::None) == nullptr);
    }
    {
        // Compiler-free: the cooked pack supplies the entry the cook compiled the stage for.
        CookedShaderPack pack;
        const byte blob[] = {byte{1}, byte{2}, byte{3}, byte{4}};
        pack.Add(u8"cooked", ShaderStage::Fragment, ShaderFlags::None, CookedShaderFormat::SpirV,
                 Span<const byte>(blob, 4));
        pack.AddEntryPoint(u8"cooked", ShaderStage::Fragment, u8"PSMain");
        ShaderSystem ss(device);
        ss.SetCookedPack(&pack);
        rhi::ShaderModule* m = ss.GetVariant(u8"cooked", ShaderStage::Fragment, ShaderFlags::None);
        REQUIRE(m != nullptr);
        CHECK(m->entryPoint.AsView() == StringView(u8"PSMain"));
    }
    compiler->Destroy();
}

// A cooked shader asset registers each stage with the lattice its cook compiled: a request
// canonicalizes onto it (flags the stage never declared share the declared variant), as the
// corpus's requests do, so the variants a dist will look up are the ones the cook proved.
TEST_CASE("shader system: a registered source with a declared mask canonicalizes requests")
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
        ss.RegisterSource(u8"asset", ShaderStage::Vertex, kTrivialVertex, kCorpusEntryPoint,
                          ShaderFlags::Skinned);
        rhi::ShaderModule* plain = ss.GetVariant(u8"asset", ShaderStage::Vertex, ShaderFlags::None);
        rhi::ShaderModule* skinned =
            ss.GetVariant(u8"asset", ShaderStage::Vertex, ShaderFlags::Skinned);
        REQUIRE(plain != nullptr);
        REQUIRE(skinned != nullptr);
        CHECK(plain != skinned);
        // Wind was never declared: the request is the declared variant it reduces to.
        CHECK(ss.GetVariant(u8"asset", ShaderStage::Vertex, ShaderFlags::Wind) == plain);
        CHECK(ss.GetVariant(u8"asset", ShaderStage::Vertex,
                            ShaderFlags::Skinned | ShaderFlags::Wind) == skinned);
    }
    compiler->Destroy();
}

// A dist has no compiler: a project's shader is registered from its cooked product (its source,
// possibly stripped) but served from the pack the export cooked it into, with the pack's entry
// point and lattice. A variant the pack lacks stays the loud cook-coverage miss.
TEST_CASE("shader system: without a compiler, a registered shader is served from the pack")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    CookedShaderPack pack;
    const byte blob[] = {byte{1}, byte{2}, byte{3}, byte{4}};
    pack.Add(u8"toon", ShaderStage::Fragment, ShaderFlags::None, CookedShaderFormat::SpirV,
             Span<const byte>(blob, 4));
    pack.Add(u8"toon", ShaderStage::Fragment, ShaderFlags::GBuffer, CookedShaderFormat::SpirV,
             Span<const byte>(blob, 4));
    pack.AddDeclaredMask(u8"toon", ShaderStage::Fragment, ShaderFlags::GBuffer);
    pack.AddEntryPoint(u8"toon", ShaderStage::Fragment, u8"PSMain");

    ShaderSystem ss(device);
    ss.SetCookedPack(&pack);
    ss.RegisterSource(u8"toon", ShaderStage::Fragment, u8"", u8"PSMain", ShaderFlags::GBuffer);
    rhi::ShaderModule* plain = ss.GetVariant(u8"toon", ShaderStage::Fragment, ShaderFlags::None);
    REQUIRE(plain != nullptr);
    CHECK(plain->entryPoint.AsView() == StringView(u8"PSMain"));
    // Flags the stage never declared reduce to the cooked variant.
    CHECK(ss.GetVariant(u8"toon", ShaderStage::Fragment, ShaderFlags::Skinned) == plain);
    CHECK(ss.GetVariant(u8"toon", ShaderStage::Fragment, ShaderFlags::GBuffer) != nullptr);
    // A stage the pack never cooked is a miss.
    CHECK(ss.GetVariant(u8"toon", ShaderStage::Vertex, ShaderFlags::None) == nullptr);
}

// A dist's player can carry a compiler (a dev build exported as a host template does), but the
// export stripped the project shader's source: an empty registered source is served from the pack,
// never compiled (DXC would report a missing entry point).
TEST_CASE("shader system: with a compiler, a stripped registered shader is served from the pack")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    rhi::null::NullDevice device{DefaultAllocator()};
    {
        CookedShaderPack pack;
        const byte blob[] = {byte{1}, byte{2}, byte{3}, byte{4}};
        pack.Add(u8"toon", ShaderStage::Fragment, ShaderFlags::None, CookedShaderFormat::SpirV,
                 Span<const byte>(blob, 4));
        pack.AddEntryPoint(u8"toon", ShaderStage::Fragment, u8"PSMain");

        ShaderSystem ss(*compiler, device);
        ss.SetCookedPack(&pack);
        ss.RegisterSource(u8"toon", ShaderStage::Fragment, u8"", u8"PSMain", ShaderFlags::None);
        rhi::ShaderModule* m = ss.GetVariant(u8"toon", ShaderStage::Fragment, ShaderFlags::None);
        REQUIRE(m != nullptr);
        CHECK(m->entryPoint.AsView() == StringView(u8"PSMain"));
    }
    compiler->Destroy();
}

// The effects the engine's includes declare: a game reads their
// tables from the cooked pack, the editor parses them from the sources it serves.
TEST_CASE("shader system: effects are served by name and by flag from the pack")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    CookedShaderPack pack;
    String error;
    const ShaderEffectBlock wind = ParseShaderEffect(u8"effect Wind : WIND\n"
                                                     u8"{\n"
                                                     u8"    float Strength = 0 [0, 2] (gate);\n"
                                                     u8"    float Speed = 1 [0, 20];\n"
                                                     u8"    float Height = 1 [0.01, 100];\n"
                                                     u8"}\n");
    REQUIRE(wind.IsOk());
    REQUIRE(pack.AddEffect(wind.effect, error));

    ShaderSystem ss(device); // a game's: no compiler
    CHECK(ss.FindEffect(u8"Wind") == nullptr); // no pack, no provider: nothing declares one
    ss.SetCookedPack(&pack);
    const ShaderEffect* byName = ss.FindEffect(u8"Wind");
    REQUIRE(byName != nullptr);
    CHECK(byName->flag == ShaderFlags::Wind);
    CHECK(byName->properties.Size() == 3u);
    CHECK(byName->gateIndex == 0u);
    CHECK(byName->uniformSize == 16u);
    CHECK(ss.EffectForFlag(ShaderFlags::Wind) == byName);
    CHECK(ss.EffectForFlag(ShaderFlags::Dither) == nullptr);
    CHECK(ss.FindEffect(u8"Gust") == nullptr);
    CHECK(ss.Effects().Size() == 1u);
}

TEST_CASE("shader system: in dev, effects are parsed from the provider's includes")
{
    const std::filesystem::path root = "shader_effect_corpus";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "surface");
    {
        std::ofstream stage(root / "sway.vs.hlsl", std::ios::binary);
        stage << "// variants: WIND\n#include \"surface/wind_fx.hlsli\"\n"
              << "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
        // An include in a subfolder, as the corpus's surface templates are.
        std::ofstream include(root / "surface" / "wind_fx.hlsli", std::ios::binary);
        include << "effect Wind : WIND\n{\n    float Strength = 0 (gate);\n}\n";
        std::ofstream plain(root / "plain.hlsli", std::ios::binary);
        plain << "float Plain() { return 1; }\n";
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        foundation::vfs::NativeFileSystem fs(u8"shader_effect_corpus", DefaultAllocator());
        FileShaderSourceProvider provider{DefaultAllocator()};
        REQUIRE(provider.Initialize(fs, u8"").IsOk());

        Array<String> includes;
        provider.CollectIncludeNames(includes);
        CHECK(includes.Size() == 2u);

        ShaderSystem ss(device);
        ss.SetSourceProvider(&provider);
        ss.SetIncludeResolver(&provider);
        const ShaderEffect* wind = ss.FindEffect(u8"Wind");
        REQUIRE(wind != nullptr);
        CHECK(wind->flag == ShaderFlags::Wind);
        CHECK(ss.EffectForFlag(ShaderFlags::Wind) == wind);
        CHECK(ss.Effects().Size() == 1u);
    }
    std::filesystem::remove_all(root);
}

// What a shader's stage declares, asked by the material factory for the effects a material on it
// carries: a registered lattice, the pack's, or the source's line.
TEST_CASE("shader system: a stage's declared variants, registered, packed or read from its source")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    CookedShaderPack pack;
    pack.AddDeclaredMask(u8"packed", ShaderStage::Vertex, ShaderFlags::Instanced | ShaderFlags::Wind);

    ShaderSystem game(device); // no compiler: the pack answers
    game.SetCookedPack(&pack);
    CHECK(game.DeclaredVariants(u8"packed", ShaderStage::Vertex) == (ShaderFlags::Instanced | ShaderFlags::Wind));
    CHECK(game.DeclaredVariants(u8"packed", ShaderStage::Fragment) == ShaderFlags::None);
    CHECK(game.DeclaredVariants(u8"unknown", ShaderStage::Vertex) == ShaderFlags::None);

    ShaderSystem editor(device);
    editor.RegisterSource(u8"lattice", ShaderStage::Vertex, u8"float4 main() : SV_Position { return 0; }\n",
                          u8"main", ShaderFlags::Skinned | ShaderFlags::Wind);
    CHECK(editor.DeclaredVariants(u8"lattice", ShaderStage::Vertex) == (ShaderFlags::Skinned | ShaderFlags::Wind));
    // Registered without a lattice: its own line says what its code reads.
    editor.RegisterSource(u8"raw", ShaderStage::Vertex,
                          u8"// variants(vertex): WIND\n// variants(fragment): DITHER\n"
                          u8"float4 main() : SV_Position { return 0; }\n",
                          u8"main");
    CHECK(editor.DeclaredVariants(u8"raw", ShaderStage::Vertex) == ShaderFlags::Wind);
}
