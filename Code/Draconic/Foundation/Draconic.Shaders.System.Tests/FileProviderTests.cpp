// Tests for the FileShaderSourceProvider (engine shader root, shaders.md P1) and the
// ShaderSystem provider seam: manifest scan + stem/stage mapping, lazy fetch, pull-on-miss
// through GetVariant with .hlsli include resolution, explicit-registration precedence, and
// PumpReloads hot reload (including the .hlsli -> reload-everything fallback). File shaders
// compile real SPIR-V via DXC on the Null RHI backend, same as ShaderSystemTests.
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

    // Spins PumpReloads past the provider's sweep throttle (2 windows: the file edit can
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

    FileShaderSourceProvider provider;
    REQUIRE(provider.Initialize(u8"shader_provider_scan").IsOk());
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
    CHECK(provider.FetchSource(u8"tonemap", ShaderStage::Fragment, source));
    CHECK(source.AsView() == StringView(reinterpret_cast<const char8_t*>(kRedPS)));
    CHECK(provider.FetchSource(u8"cluster", ShaderStage::Compute, source));
    CHECK_FALSE(provider.FetchSource(u8"tonemap", ShaderStage::Compute, source));
    CHECK_FALSE(provider.FetchSource(u8"nope", ShaderStage::Fragment, source));

    FileShaderSourceProvider missing;
    CHECK(missing.Initialize(u8"shader_provider_does_not_exist") == ErrorCode::NotFound);
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
        FileShaderSourceProvider provider;
        REQUIRE(provider.Initialize(u8"shader_provider_sys").IsOk());

        ShaderSystem ss(*compiler, device);
        ss.SetSourceProvider(&provider);
        const StringView includePaths[] = {provider.RootDirectory()};
        ss.SetIncludePaths(Span<const StringView>{includePaths, 1});

        // Never registered - the source comes from the provider, the #include from the root.
        rhi::ShaderModule* m = ss.GetVariant(u8"prov", ShaderStage::Fragment, ShaderFlags::None);
        CHECK(m != nullptr);

        // Explicit registration takes precedence over the provider on the next compile.
        ss.RegisterSource(u8"prov", ShaderStage::Fragment,
                          u8"float4 main() : SV_Target { return oops; }");
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
        FileShaderSourceProvider provider;
        REQUIRE(provider.Initialize(u8"shader_provider_reload").IsOk());

        ShaderSystem ss(*compiler, device);
        ss.SetSourceProvider(&provider);

        CHECK(ss.GetVariant(u8"hot", ShaderStage::Fragment, ShaderFlags::None) != nullptr);
        CHECK(ss.Version(u8"hot") == 0);

        // No edits -> a full throttle window of pumps reloads nothing.
        CHECK(SpinReloads(ss) == 0);

        // Edit the shader body: exactly that shader reloads, its version bumps, and the
        // next GetVariant compiles the new source.
        WriteFile(root / "hot.ps.hlsl", kGreenPS);
        CHECK(SpinReloads(ss) == 1);
        CHECK(ss.Version(u8"hot") == 1);
        CHECK(ss.Version(u8"cold") == 0);
        CHECK(ss.GetVariant(u8"hot", ShaderStage::Fragment, ShaderFlags::None) != nullptr);
        String fetched;
        CHECK(provider.FetchSource(u8"hot", ShaderStage::Fragment, fetched));
        CHECK(fetched.AsView() == StringView(reinterpret_cast<const char8_t*>(kGreenPS)));

        // Edit a .hlsli: includers are unknown, so EVERY served shader reloads.
        WriteFile(root / "reload_common.hlsli", "// v2 - bigger than before\n");
        CHECK(SpinReloads(ss) == 2);
        CHECK(ss.Version(u8"hot") == 2);
        CHECK(ss.Version(u8"cold") == 1);
    }
    compiler->Destroy();
}

// Same reload flow with an ABSOLUTE root - mirrors how the RenderSubsystem passes the
// compile-time engine shader dir (relative roots are the dist fallback).
TEST_CASE("file provider: reload detection with an absolute root")
{
    const std::filesystem::path root = std::filesystem::absolute("shader_provider_abs");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteFile(root / "hot.ps.hlsl", kRedPS);

    FileShaderSourceProvider provider;
    const std::string rootStr = root.string();
    REQUIRE(provider.Initialize(StringView(reinterpret_cast<const char8_t*>(rootStr.c_str()),
                                           rootStr.size()))
                .IsOk());

    Array<String> changed;
    for (u32 i = 0; i < FileShaderSourceProvider::PollEveryNCalls * 2 + 1; ++i)
    {
        (void)provider.PollChanges(changed);
    }
    CHECK(changed.Size() == 0);

    WriteFile(root / "hot.ps.hlsl", kGreenPS);
    for (u32 i = 0; i < FileShaderSourceProvider::PollEveryNCalls * 2 + 1; ++i)
    {
        (void)provider.PollChanges(changed);
    }
    CHECK(changed.Size() == 1);
}
