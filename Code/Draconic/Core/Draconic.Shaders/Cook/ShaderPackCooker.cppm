// Engine-shader cook - enumerate the built-in HLSL corpus and precompile it into a CookedShaderPack.
//
// The EXPORT-time step that retires the runtime compiler for shipped dists (docs/design/shaders.md,
// "Cook split"). For each stage file under the shader dir it: parses the variant directive, drift-
// lints (fails on a #ifdef'd-but-undeclared flag), enumerates the power set of the declared mask, and
// for every (variant x requested backend format) emits a blob into the pack - SPIR-V / DXIL via DXC,
// WGSL via the WgslTranslator. Runs on the dev/CI host only; the dist just reads the pack.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shaders:pack_cook;

import draconic.foundation;
import :types;
import :flags;
import :variants;
import :pack;
import :compiler;
import :wgsl_cook;

using namespace draconic::foundation;

export namespace draconic::shaders
{
    struct ShaderCookOptions
    {
        StringView shaderDir;                    // source + #include root (e.g. Data/Shaders)
        StringView scratchDir;                   // WgslTranslator intermediates (must exist)
        Span<const CookedShaderFormat> formats;  // which backend blobs to emit
        bool validateWgsl = true;                // run tint over the WGSL
        // SPIR-V target for the SpirV blobs. vulkan1.1 by contract: the SpirV bucket serves
        // BOTH Vulkan and native WebGPU (wgpu-native/naga), and naga rejects SPIR-V 1.4+
        // instructions (OpCopyLogical) that vulkan1.3-targeted DXC emits - a 1.3 pack
        // panics wgpu-native at pipeline creation. Matches the dev-mode WebGPU compile.
        StringView spirvTargetEnv = u8"vulkan1.1";
    };

    struct ShaderCookReport
    {
        bool success = false;
        usize filesCooked = 0;    // stage files processed
        usize variantsCooked = 0; // (variant x format) blobs emitted
        Array<String> errors;     // lint + compile diagnostics (each one line)
    };

    // Forward declarations (definitions below CookEngineShaders).
    [[nodiscard]] bool ParseStageFile(StringView fileName, ShaderStage& outStage,
                                      StringView& outStem);
    [[nodiscard]] String FormatError(StringView file, StringView msg);
    bool CookOne(Compiler& compiler, WgslTranslator& translator, StringView source,
                 ShaderStage stage, ShaderFlags flags, CookedShaderFormat format,
                 const ShaderCookOptions& opts, Span<const StringView> includePaths, StringView stem,
                 StringView fileName, CookedShaderPack& pack, ShaderCookReport& report);

    // Inline-expand `#include "file"` directives against the shader root, so the drift-lint
    // sees the .hlsli bodies where the real flag #ifdefs live (a stage file is often just the
    // directive + one include - linting only its own text would pass a cook that silently
    // strips a used flag in dist). Quote-includes only (the corpus has no angle includes),
    // depth-limited and de-duplicated; unreadable includes are skipped here - DXC reports
    // them properly during the actual compile.
    inline void AppendExpandedSource(StringView shaderDir, StringView source, u32 depth,
                                     Array<String>& visited, String& out)
    {
        constexpr u32 kMaxIncludeDepth = 8;
        const usize n = source.Size();
        const char8_t* d = source.Data();
        usize lineStart = 0;
        while (lineStart <= n)
        {
            usize lineEnd = lineStart;
            while (lineEnd < n && d[lineEnd] != u8'\n')
            {
                ++lineEnd;
            }
            const StringView line = source.SubStr(lineStart, lineEnd - lineStart);

            usize k = 0;
            while (k < line.Size() && (line.Data()[k] == u8' ' || line.Data()[k] == u8'\t'))
            {
                ++k;
            }
            const StringView trimmed = line.SubStr(k, line.Size() - k);
            bool expanded = false;
            if (depth < kMaxIncludeDepth && trimmed.StartsWith(u8"#include"))
            {
                const usize firstQuote = [&]() -> usize
                {
                    for (usize i = 8; i < trimmed.Size(); ++i)
                    {
                        if (trimmed.Data()[i] == u8'"')
                        {
                            return i;
                        }
                    }
                    return trimmed.Size();
                }();
                usize closeQuote = trimmed.Size();
                for (usize i = firstQuote + 1; i < trimmed.Size(); ++i)
                {
                    if (trimmed.Data()[i] == u8'"')
                    {
                        closeQuote = i;
                        break;
                    }
                }
                if (firstQuote < trimmed.Size() && closeQuote < trimmed.Size())
                {
                    const StringView includeName =
                        trimmed.SubStr(firstQuote + 1, closeQuote - firstQuote - 1);
                    bool seen = false;
                    for (const String& v : visited)
                    {
                        if (v.AsView() == includeName)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        visited.PushBack(String(includeName));
                        String includePath(shaderDir);
                        includePath += u8"/";
                        includePath += includeName;
                        const Result<Array<byte>> bytes = ReadFile(includePath.AsView());
                        if (bytes.HasValue())
                        {
                            const Array<byte>& b = bytes.Value();
                            AppendExpandedSource(
                                shaderDir,
                                StringView(reinterpret_cast<const char8_t*>(b.Data()), b.Size()),
                                depth + 1, visited, out);
                            expanded = true;
                        }
                    }
                    else
                    {
                        expanded = true; // already inlined once - don't re-scan or keep the line
                    }
                }
            }
            if (!expanded)
            {
                out += line;
                out += u8"\n";
            }

            if (lineEnd >= n)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }
    }

    // Fill `pack` from every stage file under opts.shaderDir. On any error the pack is left partially
    // filled and success=false - the caller decides whether to ship (a dist should not).
    inline ShaderCookReport CookEngineShaders(Compiler& compiler, const ShaderCookOptions& opts,
                                              CookedShaderPack& pack)
    {
        ShaderCookReport report;

        // Enumerate the shader dir (stage files only, sorted for determinism).
        struct Collector
        {
            Array<String> files;
        } collector;
        const bool listed = ListDirectory(
            opts.shaderDir,
            [](void* ctx, StringView name, bool isDir)
            {
                if (!isDir)
                {
                    static_cast<Collector*>(ctx)->files.PushBack(String(name));
                }
            },
            &collector);
        if (!listed)
        {
            report.errors.PushBack(String(u8"could not list the shader directory"));
            return report;
        }
        // ListDirectory yields filesystem order (readdir/FindFirstFile) - actually sort, so
        // the pack's entry order (and bytes) is reproducible across hosts and runs.
        collector.files.Sort(
            [](const String& a, const String& b)
            {
                const usize n = a.Size() < b.Size() ? a.Size() : b.Size();
                for (usize i = 0; i < n; ++i)
                {
                    if (a[i] != b[i])
                    {
                        return a[i] < b[i];
                    }
                }
                return a.Size() < b.Size();
            });

        const StringView includePaths[] = {opts.shaderDir};

        WgslTranslator translator(compiler, opts.scratchDir);
        translator.SetValidateWithTint(opts.validateWgsl);

        for (const String& fileName : collector.files)
        {
            ShaderStage stage = ShaderStage::Vertex;
            StringView stem;
            if (!ParseStageFile(fileName.AsView(), stage, stem))
            {
                continue; // .hlsli include or non-shader file
            }

            String pathBuf(opts.shaderDir);
            pathBuf += u8"/";
            pathBuf += fileName.AsView();
            const Result<Array<byte>> srcBytes = ReadFile(pathBuf.AsView());
            if (!srcBytes.HasValue())
            {
                report.errors.PushBack(FormatError(fileName.AsView(), u8"could not read source"));
                continue;
            }
            const Array<byte>& sb = srcBytes.Value();
            const StringView source(reinterpret_cast<const char8_t*>(sb.Data()), sb.Size());

            // Variant model: declared mask + drift-lint (used-but-undeclared is a hard error).
            // The directive comes from the STAGE FILE, but the lint runs over the
            // include-EXPANDED text - the flag #ifdefs live in the shared .hlsli bodies.
            const VariantDirective directive = ParseVariantDirective(source);
            Array<StringView> undeclared;
            String expandedSource;
            {
                Array<String> visitedIncludes;
                AppendExpandedSource(opts.shaderDir, source, 0, visitedIncludes, expandedSource);
            }
            FindUndeclaredFlagUses(expandedSource.AsView(), directive.mask, undeclared);
            if (!undeclared.IsEmpty())
            {
                String msg(u8"uses undeclared variant flag(s):");
                for (usize i = 0; i < undeclared.Size(); ++i)
                {
                    msg += u8" ";
                    msg += undeclared[i];
                }
                msg += u8" (add them to the // draconic:variants directive)";
                report.errors.PushBack(FormatError(fileName.AsView(), msg.AsView()));
                continue;
            }

            // Record the declared mask so the dist runtime can canonicalize requests onto the
            // cooked lattice (dev == dist behaviour).
            pack.AddDeclaredMask(stem, stage, directive.mask);

            Array<ShaderFlags> variants;
            EnumerateVariants(directive.mask, variants);

            ++report.filesCooked;
            for (usize v = 0; v < variants.Size(); ++v)
            {
                const ShaderFlags flags = variants[v];
                for (usize f = 0; f < opts.formats.Size(); ++f)
                {
                    const CookedShaderFormat format = opts.formats[f];
                    if (CookOne(compiler, translator, source, stage, flags, format, opts,
                                Span<const StringView>(includePaths, 1), stem, fileName.AsView(),
                                pack, report))
                    {
                        ++report.variantsCooked;
                    }
                }
            }
        }

        report.success = report.errors.IsEmpty();
        return report;
    }

    // --- helpers -----------------------------------------------------------

    // "<stem>.<vs|ps|cs>.hlsl" -> (stage, stem); false for .hlsli and anything else. Matches the
    // FileShaderSourceProvider naming so cooked and dev names agree.
    [[nodiscard]] inline bool ParseStageFile(StringView fileName, ShaderStage& outStage,
                                             StringView& outStem)
    {
        StringView suffix;
        if (fileName.EndsWith(u8".vs.hlsl"))
        {
            outStage = ShaderStage::Vertex;
            suffix = u8".vs.hlsl";
        }
        else if (fileName.EndsWith(u8".ps.hlsl"))
        {
            outStage = ShaderStage::Fragment;
            suffix = u8".ps.hlsl";
        }
        else if (fileName.EndsWith(u8".cs.hlsl"))
        {
            outStage = ShaderStage::Compute;
            suffix = u8".cs.hlsl";
        }
        else
        {
            return false;
        }
        outStem = fileName.SubStr(0, fileName.Size() - suffix.Size());
        return !outStem.IsEmpty();
    }

    [[nodiscard]] inline String FormatError(StringView file, StringView msg)
    {
        String s(file);
        s += u8": ";
        s += msg;
        return s;
    }

    // Compile/translate one (variant, format) blob and Add it to the pack. Returns false + records an
    // error on failure.
    inline bool CookOne(Compiler& compiler, WgslTranslator& translator, StringView source,
                        ShaderStage stage, ShaderFlags flags, CookedShaderFormat format,
                        const ShaderCookOptions& opts, Span<const StringView> includePaths,
                        StringView stem, StringView fileName, CookedShaderPack& pack,
                        ShaderCookReport& report)
    {
        if (format == CookedShaderFormat::Wgsl)
        {
            const WgslCookResult wr = translator.Translate(source, stage, flags, includePaths);
            if (!wr.success)
            {
                report.errors.PushBack(FormatError(fileName, wr.error.AsView()));
                return false;
            }
            pack.Add(stem, stage, flags, format,
                     Span<const byte>(reinterpret_cast<const byte*>(wr.wgsl.Data()),
                                      wr.wgsl.Size()));
            return true;
        }

        // SPIR-V (Vulkan) or DXIL (DX12) via DXC.
        Array<ShaderDefine> defines;
        AppendDefines(flags, defines);
        CompileOptions co{};
        co.shaderModel = u8"6_0";
        co.optimizationLevel = 3;
        co.defines = Span<const ShaderDefine>(defines.Data(), defines.Size());
        co.includePaths = includePaths;

        ShaderTarget target = ShaderTarget::SPIRV;
        if (format == CookedShaderFormat::SpirV)
        {
            co.spirvTargetEnvironment = opts.spirvTargetEnv;
            co.bindingShifts = BindingShifts::Standard();
            co.bindingShiftSets = 4;
        }
        else // Dxil
        {
            target = ShaderTarget::DXIL;
        }

        CompileResult cr{};
        const Status st =
            compiler.compile(reinterpret_cast<const u8*>(source.Data()), source.Size(), stage,
                             u8"main", target, co, cr);
        if (st != ErrorCode::Ok || !cr.success)
        {
            report.errors.PushBack(FormatError(
                fileName, cr.messages != nullptr
                              ? StringView(reinterpret_cast<const char8_t*>(cr.messages))
                              : StringView(u8"DXC compile failed")));
            compiler.freeResult(cr);
            return false;
        }
        pack.Add(stem, stage, flags, format,
                 Span<const byte>(reinterpret_cast<const byte*>(cr.bytecode), cr.bytecodeSize));
        compiler.freeResult(cr);
        return true;
    }
}
