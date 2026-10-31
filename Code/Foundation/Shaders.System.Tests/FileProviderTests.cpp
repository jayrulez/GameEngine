// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for the FileShaderSourceProvider (a Shaders folder in a borrowed filesystem) and the
// ShaderSystem's provider: the manifest scan and stem/stage mapping, lazy fetch, pull-on-miss
// through GetVariant with .hlsli includes resolved through the mount (the provider is the
// compiler's include resolver), explicit-registration precedence, and PumpReloads hot reload
// (including a .hlsli edit reloading everything). File shaders compile real SPIR-V through
// DXC on the Null RHI backend, as in ShaderSystemTests.
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
namespace vfs = foundation::vfs;

namespace
{
    constexpr const char* kRedPS = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n";
    constexpr const char* kGreenPS =
        "// reloaded body (different size so the stat sweep can't miss it)\n"
        "float4 main() : SV_Target { return float4(0, 1, 0, 1); }\n";
    constexpr const char* kTrivialVS =
        "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n";

    void WriteFile(const std::filesystem::path& path, const char* text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out << text;
    }

    Compiler* MakeCompiler()
    {
        Compiler* c = nullptr;
        if (!createCompiler(CompilerDesc{}, c).IsOk())
        {
            return nullptr;
        }
        return c;
    }

    // Spins PumpReloads past the provider's sweep throttle (two windows: the file edit can
    // land right after a sweep). Returns the total number of reloaded shaders.
    usize SpinReloads(ShaderSystem& ss)
    {
        usize reloaded = 0;
        for (u32 i = 0; i < FileShaderSourceProvider::PollEveryNCalls * 2 + 1; ++i)
        {
            reloaded += ss.PumpReloads();
        }
        return reloaded;
    }
}

TEST_CASE("file provider: manifest scan, stem/stage mapping, lazy fetch")
{
    const std::filesystem::path root = "shader_provider_scan";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteFile(root / "tonemap.ps.hlsl", kRedPS);
    WriteFile(root / "tonemap.vs.hlsl", kTrivialVS);
    WriteFile(root / "cluster.cs.hlsl", kRedPS);
    WriteFile(root / "shared.hlsli", "// helper only\n");
    WriteFile(root / "readme.txt", "not a shader\n");

    vfs::NativeFileSystem fs(u8"shader_provider_scan", DefaultAllocator());
    FileShaderSourceProvider provider{DefaultAllocator()};
    REQUIRE(provider.Initialize(fs, u8"").IsOk());
    CHECK(provider.Folder().IsEmpty());
    CHECK(provider.SupportsReload()); // a native mount can watch
    CHECK(provider.ShaderFileCount() == 3); // .hlsli and .txt are not shader entries

    Array<String> names;
    provider.CollectShaderNames(names);
    CHECK(names.Size() == 2); // tonemap deduped across stages
    bool sawTonemap = false, sawCluster = false;
    for (const String& n : names)
    {
        sawTonemap = sawTonemap || n.AsView() == u8"tonemap";
        sawCluster = sawCluster || n.AsView() == u8"cluster";
    }
    CHECK(sawTonemap);
    CHECK(sawCluster);

    String source;
    String entryPoint;
    CHECK(provider.FetchSource(u8"tonemap", ShaderStage::Fragment, source, entryPoint));
    CHECK(source.AsView() == StringView(reinterpret_cast<const char8_t*>(kRedPS)));
    CHECK(entryPoint.AsView() == kCorpusEntryPoint); // the corpus: a file per stage, `main`
    CHECK(provider.FetchSource(u8"cluster", ShaderStage::Compute, source, entryPoint));
    CHECK_FALSE(provider.FetchSource(u8"tonemap", ShaderStage::Compute, source, entryPoint));
    CHECK_FALSE(provider.FetchSource(u8"nope", ShaderStage::Fragment, source, entryPoint));

    // A folder the mount does not have is NotFound (callers fall back to registered strings).
    FileShaderSourceProvider missing{DefaultAllocator()};
    CHECK(missing.Initialize(fs, u8"does_not_exist") == ErrorCode::NotFound);
    CHECK(missing.ShaderFileCount() == 0);
}

TEST_CASE("file provider: a Shaders subfolder - mount-relative paths and folder-first includes")
{
    // The production shape: the data root is the mount, the shaders live in Shaders/ under it.
    const std::filesystem::path root = "shader_provider_data";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Shaders" / "nested");
    WriteFile(root / "Shaders" / "tonemap.ps.hlsl", kRedPS);
    WriteFile(root / "Shaders" / "common.hlsli", "// common\n");
    WriteFile(root / "Shaders" / "nested" / "inner.hlsli", "// inner\n");
    WriteFile(root / "stray.hlsli", "// outside the shader folder\n");

    vfs::NativeFileSystem fs(u8"shader_provider_data", DefaultAllocator());
    FileShaderSourceProvider provider{DefaultAllocator()};
    REQUIRE(provider.Initialize(fs, u8"Shaders").IsOk());
    CHECK(provider.Folder() == u8"Shaders");
    CHECK(provider.ShaderFileCount() == 1);

    String source;
    String entryPoint;
    CHECK(provider.FetchSource(u8"tonemap", ShaderStage::Fragment, source, entryPoint));
    CHECK(source.AsView() == StringView(reinterpret_cast<const char8_t*>(kRedPS)));

    // The include resolver half: a bare first-level include resolves inside the folder; a
    // nested include arrives with the folder prefix already (DXC forms it from the includer's
    // directory); a file outside the folder is not reachable bare; an absent one fails.
    String inc;
    CHECK(provider.LoadInclude(u8"common.hlsli", inc));
    CHECK(inc.AsView() == u8"// common\n");
    CHECK(provider.LoadInclude(u8"Shaders/nested/inner.hlsli", inc));
    CHECK(inc.AsView() == u8"// inner\n");
    CHECK(provider.LoadInclude(u8"nested/inner.hlsli", inc)); // folder-first also finds it
    CHECK_FALSE(provider.LoadInclude(u8"absent.hlsli", inc));
    CHECK_FALSE(provider.LoadInclude(u8"", inc));
    // A mount-relative path outside the folder still resolves (the includer could be there).
    CHECK(provider.LoadInclude(u8"stray.hlsli", inc));
}

TEST_CASE("shader system: pulls source from the provider; includes resolve; "
          "explicit registration wins")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    const std::filesystem::path root = "shader_provider_sys";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteFile(root / "prov_common.hlsli", "float4 Tint() { return float4(0, 0, 1, 1); }\n");
    WriteFile(root / "prov.ps.hlsl",
              "#include \"prov_common.hlsli\"\nfloat4 main() : SV_Target { return Tint(); }\n");

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        vfs::NativeFileSystem fs(u8"shader_provider_sys", DefaultAllocator());
        FileShaderSourceProvider provider{DefaultAllocator()};
        REQUIRE(provider.Initialize(fs, u8"").IsOk());

        ShaderSystem ss(*compiler, device);
        ss.SetSourceProvider(&provider);
        ss.SetIncludeResolver(&provider); // includes come through the mount, not the disk

        // Never registered: the source comes from the provider, the #include from the mount.
        rhi::ShaderModule* m = ss.GetVariant(u8"prov", ShaderStage::Fragment, ShaderFlags::None);
        CHECK(m != nullptr);

        // Explicit registration takes precedence over the provider on the next compile.
        ss.RegisterSource(u8"prov", ShaderStage::Fragment,
                          u8"float4 main() : SV_Target { return oops; }", kCorpusEntryPoint);
        ss.InvalidateShader(u8"prov");
        CHECK(ss.GetVariant(u8"prov", ShaderStage::Fragment, ShaderFlags::None) == nullptr);
    }
    compiler->Destroy();
}

TEST_CASE("shader system: PumpReloads picks up file edits and .hlsli edits")
{
    Compiler* compiler = MakeCompiler();
    if (compiler == nullptr)
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    const std::filesystem::path root = "shader_provider_reload";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteFile(root / "hot.ps.hlsl", kRedPS);
    WriteFile(root / "cold.ps.hlsl", kRedPS);
    WriteFile(root / "reload_common.hlsli", "// v1\n");

    rhi::null::NullDevice device{DefaultAllocator()};
    {
        vfs::NativeFileSystem fs(u8"shader_provider_reload", DefaultAllocator());
        FileShaderSourceProvider provider{DefaultAllocator()};
        REQUIRE(provider.Initialize(fs, u8"").IsOk());

        ShaderSystem ss(*compiler, device);
        ss.SetSourceProvider(&provider);
        ss.SetIncludeResolver(&provider);

        CHECK(ss.GetVariant(u8"hot", ShaderStage::Fragment, ShaderFlags::None) != nullptr);
        CHECK(ss.Version(u8"hot") == 0);

        // A registered (project) shader that includes the library file, which does not yet
        // define what it needs: it fails, and the failure is remembered.
        ss.RegisterSource(u8"user", ShaderStage::Fragment,
                          u8"#include \"reload_common.hlsli\"\n"
                          u8"float4 main() : SV_Target { return float4(COMMON_RED, 0, 0, 1); }\n",
                          kCorpusEntryPoint);
        CHECK(ss.GetVariant(u8"user", ShaderStage::Fragment, ShaderFlags::None) == nullptr);

        // With no edits, a full throttle window of pumps reloads nothing.
        CHECK(SpinReloads(ss) == 0);

        // Edit the shader body: exactly that shader reloads, its version bumps, and the
        // next GetVariant compiles the new source.
        WriteFile(root / "hot.ps.hlsl", kGreenPS);
        CHECK(SpinReloads(ss) == 1);
        CHECK(ss.Version(u8"hot") == 1);
        CHECK(ss.Version(u8"cold") == 0);
        CHECK(ss.GetVariant(u8"hot", ShaderStage::Fragment, ShaderFlags::None) != nullptr);
        String fetched;
        String entryPoint;
        CHECK(provider.FetchSource(u8"hot", ShaderStage::Fragment, fetched, entryPoint));
        CHECK(fetched.AsView() == StringView(reinterpret_cast<const char8_t*>(kGreenPS)));

        // Edit a .hlsli: includers are unknown, so every served shader reloads.
        WriteFile(root / "reload_common.hlsli",
                  "// v2 - bigger than before\n#define COMMON_RED 1\n");
        CHECK(SpinReloads(ss) == 2);
        CHECK(ss.Version(u8"hot") == 2);
        CHECK(ss.Version(u8"cold") == 1);
        // ... and a remembered failure gets another compile: the registered shader that needed
        // the fixed library file now compiles.
        CHECK(ss.GetVariant(u8"user", ShaderStage::Fragment, ShaderFlags::None) != nullptr);
    }
    compiler->Destroy();
}

// The same reload flow with an absolute mount root and a Shaders subfolder, as in the
// application's data mount (an absolute discovered data root).
TEST_CASE("file provider: reload detection with an absolute root and a subfolder")
{
    const std::filesystem::path root = std::filesystem::absolute("shader_provider_abs");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Shaders");
    WriteFile(root / "Shaders" / "hot.ps.hlsl", kRedPS);

    const std::string rootStr = root.string();
    vfs::NativeFileSystem fs(
        StringView(reinterpret_cast<const char8_t*>(rootStr.c_str()), rootStr.size()),
        DefaultAllocator());
    FileShaderSourceProvider provider{DefaultAllocator()};
    REQUIRE(provider.Initialize(fs, u8"Shaders").IsOk());

    Array<String> changed;
    for (u32 i = 0; i < FileShaderSourceProvider::PollEveryNCalls * 2 + 1; ++i)
    {
        (void)provider.PollChanges(changed);
    }
    CHECK(changed.Size() == 0);

    WriteFile(root / "Shaders" / "hot.ps.hlsl", kGreenPS);
    for (u32 i = 0; i < FileShaderSourceProvider::PollEveryNCalls * 2 + 1; ++i)
    {
        (void)provider.PollChanges(changed);
    }
    CHECK(changed.Size() == 1);
}
