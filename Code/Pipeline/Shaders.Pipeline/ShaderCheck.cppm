// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Shaders - the `:check` partition: a custom shader checked without cooking it.
//
// CheckShaderSource runs what the cook runs on a shader's text (the properties block expanded,
// both stages' variant lattices read, every flag the file tests declared, the variants compiled
// with DXC) and returns the errors as data, each with its file, line and column, instead of
// logging them. The cook logs them and fails; the editor's shader page puts them on the lines as
// you type; the MCP tools return them. One check, so the three never disagree about a shader.

module;
#include "Core/Prelude.h"

export module shaders.pipeline:check;

import foundation.core;
import foundation.shaders;
import foundation.shaders.resource; // kDepthProgramSuffix, kPickProgramSuffix

using namespace foundation::core;
using namespace foundation::shaders;

export namespace pipeline
{
    // One error in a shader: where (the file it is in, which may be an engine library file the
    // shader includes; 1-based line and column, 0 when unknown) and what.
    struct ShaderDiagnostic
    {
        String file;
        u32 line = 0;
        u32 column = 0;
        String message;
    };

    // Which variants a check compiles. The cook proves every one; a live check as you type
    // compiles each stage's base variant, the one every draw starts from, and leaves the flags to
    // the cook so typing stays responsive.
    enum class ShaderCheckDepth : u8
    {
        BaseVariants,
        EveryVariant,
    };

    struct ShaderCheckOptions
    {
        StringView vertexEntryPoint = u8"VSMain";
        StringView fragmentEntryPoint = u8"PSMain";
        StringView shaderLibrary; // the engine's Shaders/ folder, the include path; empty for none
        ShaderCheckDepth depth = ShaderCheckDepth::EveryVariant;
        // A compiler to reuse (a page checking on every pause in typing); when null, one is created
        // and destroyed for this check.
        Compiler* compiler = nullptr;
    };

    // A surface shader's shading model (its `// surface:` line); None for a low-level shader.
    enum class SurfaceModel : u8
    {
        None,
        Lit,
        Unlit,
    };

    // One program a shader compiles to: what the shader system registers under the shader's name
    // plus `suffix` (empty for the main one).
    struct ShaderCheckProgram
    {
        String suffix;
        String text;
        String vertexEntryPoint;
        String fragmentEntryPoint;
        ShaderFlags vertexMask = ShaderFlags::None;
        ShaderFlags fragmentMask = ShaderFlags::None;
    };

    // What a check found. `expanded`, the masks, `block` and `programs` are valid whenever the
    // properties block parsed (`expanded` and the masks are the main program's); `errors` empty
    // means the shader compiles at the depth asked.
    struct ShaderCheck
    {
        ShaderPropertiesBlock block;
        String expanded;
        ShaderFlags vertexMask = ShaderFlags::None;
        ShaderFlags fragmentMask = ShaderFlags::None;
        SurfaceModel surface = SurfaceModel::None;
        bool hasVertex = false; // a surface shader's Vertex(): its depth programs replace the built-ins
        Array<ShaderCheckProgram> programs;
        Array<ShaderDiagnostic> errors;

        [[nodiscard]] bool IsOk() const noexcept { return errors.IsEmpty(); }
    };

    // `file:line: message`, `file:line:column: message` or, for the whole file, `file: message`:
    // the form the Console and the cook print and a terminal makes clickable.
    [[nodiscard]] inline String FormatShaderDiagnostic(const ShaderDiagnostic& d)
    {
        if (d.line == 0)
        {
            return Format(u8"{}: {}", d.file, d.message); // about the whole file
        }
        return d.column != 0 ? Format(u8"{}:{}:{}: {}", d.file, d.line, d.column, d.message)
                             : Format(u8"{}:{}: {}", d.file, d.line, d.message);
    }

    namespace detail
    {
        [[nodiscard]] inline Optional<usize> FindIn(StringView hay, StringView needle,
                                                    usize from = 0) noexcept
        {
            for (usize i = from; i + needle.Size() <= hay.Size(); ++i)
            {
                if (hay.SubStr(i, needle.Size()) == needle)
                {
                    return i;
                }
            }
            return {};
        }

        // The position of the last ':' in [0, end), or none.
        [[nodiscard]] inline Optional<usize> LastColon(StringView text, usize end) noexcept
        {
            for (usize i = end; i > 0; --i)
            {
                if (text[i - 1] == u8':')
                {
                    return i - 1;
                }
            }
            return {};
        }
    }

    // Reads the errors out of DXC's message text, one per `path:line:col: error: message` line
    // (or `fatal error:`); notes, warnings, the source excerpts and the caret lines are skipped.
    // The path is split off from its right, so a drive letter's colon stays in it. True when at
    // least one error was found.
    inline bool ParseCompilerDiagnostics(StringView messages, Array<ShaderDiagnostic>& out)
    {
        constexpr StringView kMarkers[] = {u8": fatal error: ", u8": error: "};
        bool found = false;
        usize lineStart = 0;
        while (lineStart < messages.Size())
        {
            usize lineEnd = lineStart;
            while (lineEnd < messages.Size() && messages[lineEnd] != u8'\n')
            {
                ++lineEnd;
            }
            StringView line = messages.SubStr(lineStart, lineEnd - lineStart);
            if (!line.IsEmpty() && line[line.Size() - 1] == u8'\r')
            {
                line = line.SubStr(0, line.Size() - 1);
            }
            lineStart = lineEnd + 1;

            for (const StringView marker : kMarkers)
            {
                const Optional<usize> at = detail::FindIn(line, marker);
                if (!at.HasValue())
                {
                    continue;
                }
                // `path:line:col` before the marker.
                const StringView where = line.SubStr(0, *at);
                const Optional<usize> colColon = detail::LastColon(where, where.Size());
                const Optional<usize> lineColon =
                    colColon.HasValue() ? detail::LastColon(where, *colColon) : Optional<usize>{};
                if (!lineColon.HasValue())
                {
                    break;
                }
                const Optional<i64> lineNo =
                    ParseInt(where.SubStr(*lineColon + 1, *colColon - *lineColon - 1));
                const Optional<i64> colNo =
                    ParseInt(where.SubStr(*colColon + 1, where.Size() - *colColon - 1));
                if (!lineNo.HasValue() || !colNo.HasValue())
                {
                    break;
                }
                ShaderDiagnostic d;
                d.file = String(where.SubStr(0, *lineColon));
                d.line = static_cast<u32>(*lineNo);
                d.column = static_cast<u32>(*colNo);
                const usize text = *at + marker.Size();
                d.message = String(line.SubStr(text, line.Size() - text));
                out.PushBack(Move(d));
                found = true;
                break;
            }
        }
        return found;
    }

    // A surface shader's lattices: the template's, the same for every surface shader. WIND in each
    // vertex lattice: the engine's vertex stages sway by the material's Wind group, so every surface
    // shader can sway, its shadow and pick with it.
    inline constexpr ShaderFlags kSurfaceForwardVertexMask =
        ShaderFlags::Skinned | ShaderFlags::Instanced | ShaderFlags::Wind;
    inline constexpr ShaderFlags kSurfaceForwardFragmentMask =
        ShaderFlags::AlphaTest | ShaderFlags::GBuffer | ShaderFlags::Dither;
    inline constexpr ShaderFlags kSurfaceDepthVertexMask =
        ShaderFlags::Skinned | ShaderFlags::Instanced | ShaderFlags::AlphaTest | ShaderFlags::Wind;
    inline constexpr ShaderFlags kSurfaceDepthFragmentMask = ShaderFlags::AlphaTest;

    namespace detail
    {
        // The `// surface: lit` (or `unlit`) line, where the file says it is a surface shader. A
        // `// surface:` line with another word is an error, reported at its line.
        struct SurfaceDirective
        {
            SurfaceModel model = SurfaceModel::None;
            u32 line = 0;
            String bad; // the word that is neither lit nor unlit
        };

        [[nodiscard]] inline SurfaceDirective ParseSurfaceDirective(StringView text)
        {
            constexpr StringView kTag = u8"// surface:";
            SurfaceDirective out;
            u32 lineNo = 1;
            usize start = 0;
            while (start <= text.Size())
            {
                usize end = start;
                while (end < text.Size() && text[end] != u8'\n')
                {
                    ++end;
                }
                StringView line = Trimmed(text.SubStr(start, end - start));
                if (line.StartsWith(kTag))
                {
                    const StringView word = Trimmed(line.SubStr(kTag.Size(), line.Size() - kTag.Size()));
                    out.line = lineNo;
                    if (word == u8"lit")
                    {
                        out.model = SurfaceModel::Lit;
                    }
                    else if (word == u8"unlit")
                    {
                        out.model = SurfaceModel::Unlit;
                    }
                    else
                    {
                        out.bad = String(word);
                    }
                    return out;
                }
                if (end >= text.Size())
                {
                    break;
                }
                start = end + 1;
                ++lineNo;
            }
            return out;
        }

        // Whether the text defines `void Vertex(` outside a comment (a commented-out example in a
        // starter is not one).
        [[nodiscard]] inline bool DefinesVertexFunction(StringView text)
        {
            usize i = 0;
            const usize n = text.Size();
            const auto isIdent = [](char8_t c)
            {
                return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') ||
                       (c >= u8'0' && c <= u8'9') || c == u8'_';
            };
            while (i < n)
            {
                if (text[i] == u8'/' && i + 1 < n && text[i + 1] == u8'/')
                {
                    while (i < n && text[i] != u8'\n')
                    {
                        ++i;
                    }
                    continue;
                }
                if (text[i] == u8'/' && i + 1 < n && text[i + 1] == u8'*')
                {
                    i += 2;
                    while (i + 1 < n && !(text[i] == u8'*' && text[i + 1] == u8'/'))
                    {
                        ++i;
                    }
                    i += 2;
                    continue;
                }
                if (text.SubStr(i, Min<usize>(4, n - i)) == u8"void" && (i == 0 || !isIdent(text[i - 1])) &&
                    i + 4 < n && !isIdent(text[i + 4]))
                {
                    usize k = i + 4;
                    while (k < n && (text[k] == u8' ' || text[k] == u8'\t' || text[k] == u8'\n' || text[k] == u8'\r'))
                    {
                        ++k;
                    }
                    if (text.SubStr(k, Min<usize>(6, n - k)) == u8"Vertex")
                    {
                        k += 6;
                        while (k < n && (text[k] == u8' ' || text[k] == u8'\t'))
                        {
                            ++k;
                        }
                        if (k < n && text[k] == u8'(')
                        {
                            return true;
                        }
                    }
                }
                ++i;
            }
            return false;
        }

        // One of a surface shader's programs: the engine's API, the author's (expanded) text with
        // its own line numbers, the glue and the pass's template.
        [[nodiscard]] inline String SurfaceProgramText(StringView expanded, StringView file,
                                                      SurfaceModel model, bool hasVertex,
                                                      bool hasAlphaCutoff, StringView passTemplate)
        {
            String out;
            out += u8"// preserve-interface\n"; // a variant that does not read an input keeps it
            out += u8"#define SURFACE_PROGRAM 1\n";
            out += model == SurfaceModel::Lit ? StringView(u8"#define SURFACE_LIT 1\n")
                                              : StringView(u8"#define SURFACE_UNLIT 1\n");
            if (hasVertex)
            {
                out += u8"#define SURFACE_HAS_VERTEX 1\n";
            }
            if (hasAlphaCutoff)
            {
                out += u8"#define SURFACE_HAS_ALPHA_CUTOFF 1\n";
            }
            out += u8"#include \"surface/surface.hlsli\"\n";
            out += Format(u8"#line 1 \"{}\"\n", file);
            out += expanded;
            out += u8"\n#include \"surface/glue.hlsli\"\n#include \"surface/";
            out += passTemplate;
            out += u8"\"\n";
            return out;
        }
    }

    // Checks a shader's text as the cook would, saved as `file` (the name errors carry). A
    // low-level shader is one program, its stages' entries and lattices its own; a surface shader
    // (a `// surface:` line) three, assembled around its Surface() and Vertex() from the engine's
    // templates: the forward program, `.depth` and `.pick`.
    [[nodiscard]] inline ShaderCheck CheckShaderSource(StringView text, StringView file,
                                                       const ShaderCheckOptions& options = {})
    {
        ShaderCheck check;
        const auto fail = [&](u32 line, StringView message)
        {
            ShaderDiagnostic d;
            d.file = String(file);
            d.line = line;
            d.message = String(message);
            check.errors.PushBack(Move(d));
        };

        // The properties block becomes the set-2 declarations; `#line` keeps the author's line
        // numbers for everything after it.
        check.block = ExpandShaderProperties(text, file, check.expanded);
        for (const ShaderPropertyError& e : check.block.errors)
        {
            fail(e.line, e.message.AsView());
        }
        if (!check.block.IsOk())
        {
            return check;
        }

        const detail::SurfaceDirective directive = detail::ParseSurfaceDirective(text);
        if (!directive.bad.IsEmpty())
        {
            fail(directive.line, Format(u8"'{}' is not a surface model (lit or unlit)",
                                        directive.bad).AsView());
            return check;
        }
        check.surface = directive.model;

        if (check.surface == SurfaceModel::None)
        {
            // Each stage's lattice; a flag the file tests but no stage declares would never be
            // compiled (the same drift lint the engine's own shaders get, over both stages).
            check.vertexMask = ParseStageVariantDirective(text, ShaderStage::Vertex).mask;
            check.fragmentMask = ParseStageVariantDirective(text, ShaderStage::Fragment).mask;
            ShaderCheckProgram main;
            main.text = check.expanded;
            main.vertexEntryPoint = String(options.vertexEntryPoint);
            main.fragmentEntryPoint = String(options.fragmentEntryPoint);
            main.vertexMask = check.vertexMask;
            main.fragmentMask = check.fragmentMask;
            check.programs.PushBack(Move(main));
        }
        else
        {
            // The variants are the template's; a variants line would say otherwise.
            if (ParseStageVariantDirective(text, ShaderStage::Vertex).present ||
                ParseStageVariantDirective(text, ShaderStage::Fragment).present)
            {
                fail(0, u8"a surface shader's variants are the engine's: remove its // variants "
                        u8"lines");
                return check;
            }
            check.hasVertex = detail::DefinesVertexFunction(text);
            bool hasAlphaCutoff = false;
            for (const ShaderProperty& p : check.block.properties)
            {
                hasAlphaCutoff = hasAlphaCutoff ||
                                 (p.name == u8"AlphaCutoff" && p.type == ShaderPropertyType::Float);
            }
            const struct
            {
                StringView suffix;
                StringView pass;
                ShaderFlags vertexMask;
                ShaderFlags fragmentMask;
            } passes[3] = {
                {StringView{}, u8"forward.hlsli", kSurfaceForwardVertexMask, kSurfaceForwardFragmentMask},
                {kDepthProgramSuffix, u8"depth.hlsli", kSurfaceDepthVertexMask, kSurfaceDepthFragmentMask},
                {kPickProgramSuffix, u8"pick.hlsli", kSurfaceDepthVertexMask, kSurfaceDepthFragmentMask},
            };
            for (const auto& pass : passes)
            {
                ShaderCheckProgram program;
                program.suffix = String(pass.suffix);
                program.text = detail::SurfaceProgramText(check.expanded.AsView(), file, check.surface,
                                                          check.hasVertex, hasAlphaCutoff, pass.pass);
                program.vertexEntryPoint = String(u8"VSMain");
                program.fragmentEntryPoint = String(u8"PSMain");
                program.vertexMask = pass.vertexMask;
                program.fragmentMask = pass.fragmentMask;
                check.programs.PushBack(Move(program));
            }
            check.expanded = check.programs[0].text;
            check.vertexMask = kSurfaceForwardVertexMask;
            check.fragmentMask = kSurfaceForwardFragmentMask;
        }

        Array<StringView> undeclared;
        FindUndeclaredFlagUses(text, check.vertexMask | check.fragmentMask, undeclared);
        for (const StringView flag : undeclared)
        {
            String message(u8"tests ");
            message += flag;
            message += check.surface == SurfaceModel::None
                           ? StringView(u8" but no stage declares it (add it to a // variants(vertex): "
                                        u8"or // variants(fragment): line)")
                           : StringView(u8", which a surface shader does not compile");
            fail(0, message.AsView());
        }
        if (!undeclared.IsEmpty())
        {
            return check;
        }

        Compiler* compiler = options.compiler;
        if (compiler == nullptr &&
            (!createCompiler(CompilerDesc{}, compiler).IsOk() || compiler == nullptr))
        {
            fail(0, u8"the DXC compiler is unavailable");
            return check;
        }

        const StringView includePaths[] = {options.shaderLibrary};
        for (const ShaderCheckProgram& program : check.programs)
        {
        const bool preserveInterface = ParsePreserveInterfaceDirective(program.text.AsView());
        const struct
        {
            ShaderStage stage;
            StringView entryPoint;
            ShaderFlags mask;
        } stages[2] = {{ShaderStage::Vertex, program.vertexEntryPoint.AsView(), program.vertexMask},
                       {ShaderStage::Fragment, program.fragmentEntryPoint.AsView(), program.fragmentMask}};
        for (const auto& stage : stages)
        {
            Array<ShaderFlags> variants;
            if (options.depth == ShaderCheckDepth::EveryVariant)
            {
                EnumerateVariants(stage.mask, variants);
            }
            else
            {
                variants.PushBack(ShaderFlags::None);
            }
            for (const ShaderFlags flags : variants)
            {
                Array<ShaderDefine> defines;
                AppendDefines(flags, defines);
                CompileOptions compile{};
                compile.defines = Span<const ShaderDefine>(defines.Data(), defines.Size());
                compile.includePaths = options.shaderLibrary.IsEmpty()
                                           ? Span<const StringView>{}
                                           : Span<const StringView>(includePaths, 1);
                compile.bindingShifts = BindingShifts::Standard();
                compile.bindingShiftSets = 4;
                compile.sourceName = file;
                compile.preserveInterface = preserveInterface; // as the runtime compiles it
                CompileResult result{};
                (void)compiler->compile(reinterpret_cast<const u8*>(program.text.Data()),
                                        program.text.Size(), stage.stage, stage.entryPoint,
                                        ShaderTarget::SPIRV, compile, result);
                if (!result.success)
                {
                    const StringView messages(reinterpret_cast<const char8_t*>(
                        result.messages != nullptr ? result.messages : ""));
                    const usize first = check.errors.Size();
                    if (!ParseCompilerDiagnostics(messages, check.errors))
                    {
                        fail(0, messages.IsEmpty() ? StringView(u8"the compile failed")
                                                   : messages);
                    }
                    // The flags the failing compile had, when it had any: an error under a flag
                    // reads differently from one in every variant. (Not the stage: DXC compiles
                    // the whole file for each, so an error anywhere fails the first stage tried.)
                    if (!defines.IsEmpty())
                    {
                        String context(u8" (under ");
                        for (usize d = 0; d < defines.Size(); ++d)
                        {
                            context += d == 0 ? StringView(u8"") : StringView(u8" ");
                            context += defines[d].name;
                        }
                        context += u8")";
                        for (usize i = first; i < check.errors.Size(); ++i)
                        {
                            check.errors[i].message += context;
                        }
                    }
                }
                compiler->freeResult(result);
                if (!check.errors.IsEmpty())
                {
                    break; // one stage's first failure says what is wrong; the rest repeat it
                }
            }
            if (!check.errors.IsEmpty())
            {
                break;
            }
        }
        if (!check.errors.IsEmpty())
        {
            break; // the next program would report the same error in the author's text
        }
        }
        if (options.compiler == nullptr)
        {
            compiler->Destroy();
        }

        // A property named like one of the engine's own declarations (the vertex stage's `Tint`,
        // say) is a redefinition DXC reports in the engine's file: report it at the property.
        for (ShaderDiagnostic& d : check.errors)
        {
            constexpr StringView kRedefinition = u8"redefinition of '";
            if (d.file.AsView() == file || !d.message.AsView().StartsWith(kRedefinition))
            {
                continue;
            }
            const StringView rest = d.message.AsView().SubStr(
                kRedefinition.Size(), d.message.Size() - kRedefinition.Size());
            const Optional<usize> quote = detail::FindIn(rest, u8"'");
            if (!quote.HasValue())
            {
                continue;
            }
            const StringView name = rest.SubStr(0, *quote);
            for (const ShaderProperty& p : check.block.properties)
            {
                if (p.name.AsView() == name)
                {
                    d.message = Format(u8"property '{}' takes a name the engine's shaders already "
                                       u8"use ({}:{}); rename it",
                                       name, d.file, d.line);
                    d.file = String(file);
                    d.line = p.line;
                    d.column = 0;
                    break;
                }
            }
        }
        return check;
    }
}
