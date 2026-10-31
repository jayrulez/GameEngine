// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shader variant model: declared masks and canonicalization.
//
// A dist has no compiler, so every variant the runtime can request must exist in the cooked
// pack by construction. The mechanism:
//
//   - Authoring: one directive line per stage file, `// variants: SKINNED INSTANCED`, names
//     the flags this stage actually branches on. Without one the stage is single-variant (mask
//     None), as most shaders are.
//   - Canonicalization: GetVariant intersects every request with the declared mask, identically
//     in dev and dist, so a request for a flag the stage ignores collapses onto a variant that
//     exists.
//   - Cook: build the power set of the declared mask, so any canonicalized request lands in the
//     prebuilt lattice; a dist miss is impossible, not merely loud.
//   - Drift-lint: a stage that #ifdef's a flag it did not declare is a bug (canonicalization would
//     silently strip it, giving wrong visuals), so the cook fails on a flag used but undeclared.
//
// All of it derives from the single kShaderFlagNames table in :flags.

module;
#include "Core/Prelude.h"

export module foundation.shaders:variants;

import foundation.core;
import :types;
import :flags;

using namespace foundation::core;

export namespace foundation::shaders
{
    struct VariantDirective
    {
        ShaderFlags mask = ShaderFlags::None; // OR of the declared flags
        bool present = false;                 // was a `variants:` line found at all
    };

    // Map a flag #define name (case-sensitive) to its bit; None if unknown.
    [[nodiscard]] inline ShaderFlags FlagFromName(StringView name) noexcept
    {
        for (const ShaderFlagName& e : kShaderFlagNames)
        {
            if (e.define == name)
            {
                return e.flag;
            }
        }
        return ShaderFlags::None;
    }

    // Parse the `// variants: A B C` directive from a stage's HLSL source. The tag may sit
    // anywhere on a line (after `//`); tokens are whitespace-separated flag #define names. Unknown
    // tokens are ignored (a shader may name a not-yet-defined flag without breaking the cook). An
    // absent directive gives {None, present=false}: single-variant. `tag` is the directive's text
    // (ParseStageVariantDirective passes a stage's tagged form).
    [[nodiscard]] inline VariantDirective ParseVariantDirective(StringView source,
                                                                StringView tag = u8"variants:")
    {
        VariantDirective out;

        const usize n = source.Size();
        const char8_t* d = source.Data();
        usize i = 0;
        while (i < n)
        {
            // Find the tag.
            if (i + tag.Size() <= n && source.SubStr(i, tag.Size()) == tag)
            {
                out.present = true;
                usize j = i + tag.Size();
                // Consume tokens to end of line.
                while (j < n && d[j] != u8'\n')
                {
                    while (j < n && (d[j] == u8' ' || d[j] == u8'\t' || d[j] == u8'\r'))
                    {
                        ++j;
                    }
                    const usize start = j;
                    while (j < n && d[j] != u8' ' && d[j] != u8'\t' && d[j] != u8'\r' &&
                           d[j] != u8'\n')
                    {
                        ++j;
                    }
                    if (j > start)
                    {
                        out.mask |= FlagFromName(source.SubStr(start, j - start));
                    }
                }
                return out;
            }
            ++i;
        }
        return out;
    }

    // A one-file shader declares each stage's lattice: `// variants(vertex): SKINNED INSTANCED`
    // and `// variants(fragment): GBUFFER ALPHA_TEST` (also `compute`). A stage with no tagged line
    // takes the untagged `// variants:` (the corpus's form, every stage of its file), else none.
    [[nodiscard]] inline VariantDirective ParseStageVariantDirective(StringView source,
                                                                     ShaderStage stage)
    {
        StringView tag;
        switch (stage)
        {
        case ShaderStage::Vertex:
            tag = u8"variants(vertex):";
            break;
        case ShaderStage::Fragment:
            tag = u8"variants(fragment):";
            break;
        case ShaderStage::Compute:
            tag = u8"variants(compute):";
            break;
        default:
            break;
        }
        if (!tag.IsEmpty())
        {
            const VariantDirective tagged = ParseVariantDirective(source, tag);
            if (tagged.present)
            {
                return tagged;
            }
        }
        return ParseVariantDirective(source);
    }

    // `// preserve-interface`: the stage keeps every declared stage input and output in its SPIR-V
    // interface even when unused (DXC's -fspv-preserve-interface; -O3 otherwise strips unused
    // inputs). It is for a fragment stage that declares another shader family's full VSOut and
    // reads one member (terrain_depth.ps under HOLES), so the pairing is exact and Vulkan's
    // interface validation sees a match, not a shorter input list. Absent = the default (strip).
    [[nodiscard]] inline bool ParsePreserveInterfaceDirective(StringView source)
    {
        constexpr StringView kTag = u8"preserve-interface";
        const usize n = source.Size();
        for (usize i = 0; i + kTag.Size() <= n; ++i)
        {
            if (source.SubStr(i, kTag.Size()) == kTag)
            {
                return true;
            }
        }
        return false;
    }

    // The canonical variant for a request: only the bits the stage declared survive. Applied
    // identically in dev and dist, so the runtime dedupes (a PS that ignores SKINNED is not
    // recompiled per skin) and dev behaves as dist does.
    [[nodiscard]] constexpr ShaderFlags CanonicalizeFlags(ShaderFlags requested,
                                                          ShaderFlags declared) noexcept
    {
        return requested & declared;
    }

    // The power set of the declared mask: every variant the cook must build. Always includes None.
    // A mask with k set bits yields 2^k entries; at most 8 bits are taken, so at most 256.
    inline void EnumerateVariants(ShaderFlags declared, Array<ShaderFlags>& out)
    {
        // Collect the set bits.
        u32 bits[8];
        u32 count = 0;
        for (const ShaderFlagName& e : kShaderFlagNames)
        {
            if (HasFlag(declared, e.flag) && count < 8)
            {
                bits[count++] = static_cast<u32>(e.flag);
            }
        }
        const u32 total = 1u << count;
        for (u32 subset = 0; subset < total; ++subset)
        {
            u32 flags = 0;
            for (u32 b = 0; b < count; ++b)
            {
                if ((subset & (1u << b)) != 0u)
                {
                    flags |= bits[b];
                }
            }
            out.PushBack(static_cast<ShaderFlags>(flags));
        }
    }

    // Flag names that appear in a preprocessor conditional in the source but are not in `declared`.
    // These are the drift bugs canonicalization would silently strip. Scans lines that begin (after
    // whitespace) with a #if / #ifdef / #ifndef / #elif directive, matching whole-word flag names
    // (which also covers `defined(FLAG)` operands on those lines). Empty means clean.
    inline void FindUndeclaredFlagUses(StringView source, ShaderFlags declared,
                                       Array<StringView>& out)
    {
        const usize n = source.Size();
        const char8_t* d = source.Data();

        auto isWord = [](char8_t c) noexcept
        {
            return (c >= u8'A' && c <= u8'Z') || (c >= u8'a' && c <= u8'z') ||
                   (c >= u8'0' && c <= u8'9') || c == u8'_';
        };

        usize lineStart = 0;
        while (lineStart <= n)
        {
            usize lineEnd = lineStart;
            while (lineEnd < n && d[lineEnd] != u8'\n')
            {
                ++lineEnd;
            }
            const StringView line = source.SubStr(lineStart, lineEnd - lineStart);

            // Only lines that begin (after whitespace) with a '#' conditional directive count: a
            // bare substring test for "defined" would fail the cook on comments like
            // "user-defined" that merely mention a flag name. Whitespace
            // between '#' and the keyword ("#  if") is legal preprocessor syntax and accepted.
            usize k = 0;
            while (k < line.Size() && (line.Data()[k] == u8' ' || line.Data()[k] == u8'\t'))
            {
                ++k;
            }
            bool isCond = false;
            if (k < line.Size() && line.Data()[k] == u8'#')
            {
                usize m = k + 1;
                while (m < line.Size() && (line.Data()[m] == u8' ' || line.Data()[m] == u8'\t'))
                {
                    ++m;
                }
                const StringView keyword = line.SubStr(m, line.Size() - m);
                isCond = keyword.StartsWith(u8"if") || keyword.StartsWith(u8"elif");
            }
            if (isCond)
            {
                for (const ShaderFlagName& e : kShaderFlagNames)
                {
                    if (HasFlag(declared, e.flag))
                    {
                        continue; // declared
                    }
                    // Whole-word match of the flag name on this line.
                    const usize ln = line.Size();
                    const char8_t* ld = line.Data();
                    for (usize p = 0; p + e.define.Size() <= ln; ++p)
                    {
                        if (line.SubStr(p, e.define.Size()) != e.define)
                        {
                            continue;
                        }
                        const bool leftOk = (p == 0) || !isWord(ld[p - 1]);
                        const usize after = p + e.define.Size();
                        const bool rightOk = (after == ln) || !isWord(ld[after]);
                        if (leftOk && rightOk)
                        {
                            out.PushBack(e.define);
                            break; // report each flag once per line
                        }
                    }
                }
            }

            if (lineEnd >= n)
            {
                break;
            }
            lineStart = lineEnd + 1;
        }
    }
}
