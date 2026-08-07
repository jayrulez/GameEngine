// Shader variant model - declared masks + canonicalization (docs/design/shaders.md).
//
// A shipped dist has NO compiler, so every variant the runtime can request must exist in the cooked
// pack BY CONSTRUCTION. The mechanism:
//
//   - Authoring: one directive line per stage file, `// draconic:variants SKINNED INSTANCED`, names
//     the flags this stage actually branches on. Absent => single-variant (mask None) - most shaders.
//   - Canonicalization: GetVariant intersects every request with the declared mask, dev AND dist
//     identically, so a request for a flag the stage ignores collapses onto a variant that exists.
//   - Cook: build the POWER SET of the declared mask -> any canonicalized request lands in the
//     prebuilt lattice; a dist miss is impossible, not merely loud.
//   - Drift-lint: a stage that #ifdef's a flag it did NOT declare is a bug (canonicalization would
//     silently strip it -> wrong visuals), so the cook fails on used-but-undeclared.
//
// All of it derives from the single kShaderFlagNames table in :flags.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shaders:variants;

import draconic.foundation;
import :types;
import :flags;

using namespace draconic::foundation;

export namespace draconic::shaders
{
    struct VariantDirective
    {
        ShaderFlags mask = ShaderFlags::None; // OR of the declared flags
        bool present = false;                 // was a `draconic:variants` line found at all
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

    // Parse the `// draconic:variants A B C` directive from a stage's HLSL source. The tag may sit
    // anywhere on a line (after `//`); tokens are whitespace-separated flag #define names. Unknown
    // tokens are ignored (a shader may name a not-yet-defined flag without breaking the cook). Absent
    // directive => {None, present=false} = single-variant.
    [[nodiscard]] inline VariantDirective ParseVariantDirective(StringView source)
    {
        constexpr StringView kTag = u8"draconic:variants";
        VariantDirective out;

        const usize n = source.Size();
        const char8_t* d = source.Data();
        usize i = 0;
        while (i < n)
        {
            // Find the tag.
            if (i + kTag.Size() <= n && source.SubStr(i, kTag.Size()) == kTag)
            {
                out.present = true;
                usize j = i + kTag.Size();
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

    // The canonical variant for a request: only the bits the stage declared survive. Applied in dev
    // AND dist identically, so runtime dedupes (a PS that ignores SKINNED stops recompiling per skin)
    // and dev == dist behaviour.
    [[nodiscard]] constexpr ShaderFlags CanonicalizeFlags(ShaderFlags requested,
                                                          ShaderFlags declared) noexcept
    {
        return requested & declared;
    }

    // Power set of the declared mask -> every variant the cook must build. Always includes None.
    // A mask with k set bits yields 2^k entries (k <= 8, so <= 256).
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

    // Flag names that appear in a preprocessor conditional in the source but are NOT in `declared`.
    // These are the drift bugs canonicalization would silently strip. Scans lines that begin (after
    // whitespace) with a #if / #ifdef / #ifndef / #elif directive, matching whole-word flag names
    // (which also covers `defined(FLAG)` operands on those lines). Empty => clean.
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

            // Is this a preprocessor conditional line? Only lines that BEGIN (after whitespace)
            // with a '#' directive count - a bare substring test for "defined" would fail the
            // cook on comments like "user-defined" that merely mention a flag name. Whitespace
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
                        continue; // declared - fine
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
