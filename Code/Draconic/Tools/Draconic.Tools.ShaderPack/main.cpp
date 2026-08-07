// Draconic.Tools.ShaderPack - a headless CLI that cooks the engine shader corpus into a CookedShaderPack
// (shaders.dpak). This is the standalone shader half of what the exporter does: enumerate a shader
// directory (Data/Shaders), compile every stage x variant to the requested backend blobs, and write
// the pack. It needs DXC (SPIR-V/DXIL) and, for WGSL, the vendored naga + tint (baked paths in
// draconic.shaders), so it is a desktop authoring tool - the WGSL pack it produces is what the WEB
// build ships and loads at runtime (no compiler in the browser).
//
//   Draconic.Tools.ShaderPack <shaderDir> <output.dpak> [wgsl] [spirv] [dxil]
//
// Defaults to wgsl when no formats are given.

#include "Draconic.Foundation/Prelude.h"

#include <cstdio>

import draconic.foundation;
import draconic.shaders;

using namespace draconic::foundation;
namespace shaders = draconic::shaders;

namespace
{
    void PrintUsage()
    {
        std::fprintf(stderr,
                     "usage: Draconic.Tools.ShaderPack <shaderDir> <output.dpak> [wgsl] [spirv] [dxil]\n");
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        PrintUsage();
        return 2;
    }

    const StringView shaderDir(reinterpret_cast<const char8_t*>(argv[1]));
    const StringView outputPath(reinterpret_cast<const char8_t*>(argv[2]));

    Array<shaders::CookedShaderFormat> formats;
    for (int i = 3; i < argc; ++i)
    {
        const StringView flag(reinterpret_cast<const char8_t*>(argv[i]));
        if (flag == u8"wgsl")
        {
            formats.PushBack(shaders::CookedShaderFormat::Wgsl);
        }
        else if (flag == u8"spirv")
        {
            formats.PushBack(shaders::CookedShaderFormat::SpirV);
        }
        else if (flag == u8"dxil")
        {
            formats.PushBack(shaders::CookedShaderFormat::Dxil);
        }
        else
        {
            std::fprintf(stderr, "Draconic.Tools.ShaderPack: unknown format '%s'\n", argv[i]);
            PrintUsage();
            return 2;
        }
    }
    if (formats.IsEmpty())
    {
        formats.PushBack(shaders::CookedShaderFormat::Wgsl);
    }

    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        std::fprintf(stderr, "Draconic.Tools.ShaderPack: DXC runtime unavailable\n");
        return 1;
    }

    const StringView scratchDir = u8".shaderpackcook-scratch";
    CreateDirectories(scratchDir); // WgslTranslator intermediates (must exist)

    shaders::ShaderCookOptions opts;
    opts.shaderDir = shaderDir;
    opts.scratchDir = scratchDir;
    opts.formats = Span<const shaders::CookedShaderFormat>(formats.Data(), formats.Size());

    shaders::CookedShaderPack pack;
    const shaders::ShaderCookReport report = shaders::CookEngineShaders(*compiler, opts, pack);
    compiler->Destroy();

    for (usize i = 0; i < report.errors.Size(); ++i)
    {
        std::fprintf(stderr, "cook error: %s\n",
                     reinterpret_cast<const char*>(report.errors[i].CStr()));
    }
    std::fprintf(stderr, "Draconic.Tools.ShaderPack: files=%u variants=%u success=%d\n",
                 static_cast<unsigned>(report.filesCooked),
                 static_cast<unsigned>(report.variantsCooked), report.success ? 1 : 0);
    if (!report.success)
    {
        return 1;
    }

    FileStream out(outputPath, FileMode::Write);
    if (!out.IsValid() || !pack.Write(out).IsOk())
    {
        std::fprintf(stderr, "Draconic.Tools.ShaderPack: could not write '%s'\n", argv[2]);
        return 1;
    }
    std::fprintf(stderr, "Draconic.Tools.ShaderPack: wrote '%s' (%u entries)\n", argv[2],
                 static_cast<unsigned>(pack.Count()));
    return 0;
}
