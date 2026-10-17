// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :css_parser partition
//
// CSSParser: turns `.css` text into a StyleSheet. Ported from eepp's css/StyleSheetParser
// (common subset). Handles `/* comments */`, comma-separated selector lists, and
// `name: value;` declaration blocks; best-effort (malformed bits are skipped). Deferred:
// @media / @keyframes / @import, custom properties (--var), nesting, !important.

module;
#include "Core/Prelude.h"

export module experimental.gui:css_parser;

import foundation.core; // String, StringView, Move
import :style_rule;
import :style_sheet;
import :style_selector; // StyleSelector::SplitList
import :media_query;

using namespace foundation::core;
namespace core = foundation::core;

namespace experimental::gui
{
    // Remove /* ... */ comments (run-copied so indices in the result are contiguous).
    [[nodiscard]] inline core::String StripComments(core::StringView s)
    {
        core::String out;
        const usize n = s.Size();
        usize runStart = 0, i = 0;
        while (i < n)
        {
            if (i + 1 < n && s[i] == u8'/' && s[i + 1] == u8'*')
            {
                out.Append(s.SubStr(runStart, i - runStart));
                i += 2;
                while (i + 1 < n && !(s[i] == u8'*' && s[i + 1] == u8'/'))
                    ++i;
                i = (i + 1 < n) ? i + 2 : n; // skip closing */ (or to end)
                runStart = i;
            }
            else
            {
                ++i;
            }
        }
        out.Append(s.SubStr(runStart, n - runStart));
        return out;
    }
}

export namespace experimental::gui
{
    class CSSParser
    {
    public:
        // Parse CSS source into a StyleSheet.
        [[nodiscard]] static StyleSheet Parse(core::StringView css)
        {
            const core::String cleaned = StripComments(css);
            StyleSheet sheet;
            ParseBlock(sheet, cleaned.AsView(), MediaQuery{});
            return sheet;
        }

    private:
        // Parse a run of rules (and nested @media blocks), attaching `media` to each rule.
        static void ParseBlock(StyleSheet& sheet, core::StringView src, const MediaQuery& media)
        {
            const usize n = src.Size();
            usize i = 0;
            while (i < n)
            {
                while (i < n && IsWhiteSpace(src[i]))
                    ++i;
                if (i >= n)
                    break;

                if (src[i] == u8'@')
                {
                    // At-rule: read the prelude up to '{' (block) or ';' (statement).
                    const usize atStart = i;
                    while (i < n && src[i] != u8'{' && src[i] != u8';')
                        ++i;
                    const core::StringView prelude = Trim(src.SubStr(atStart, i - atStart));
                    if (i < n && src[i] == u8';')
                    {
                        ++i;
                        continue;
                    } // at-statement (e.g. @import) - skipped
                    if (i >= n)
                        break;

                    // Brace-matched block for this at-rule.
                    ++i; // consume '{'
                    const usize blockStart = i;
                    i32 depth = 1;
                    while (i < n && depth > 0)
                    {
                        if (src[i] == u8'{')
                            ++depth;
                        else if (src[i] == u8'}')
                        {
                            --depth;
                            if (depth == 0)
                                break;
                        }
                        ++i;
                    }
                    const core::StringView inner = src.SubStr(blockStart, i - blockStart);
                    if (i < n)
                        ++i; // consume '}'

                    if (IsMedia(prelude))
                    {
                        const core::StringView condition =
                            Trim(prelude.SubStr(6, prelude.Size() - 6));
                        ParseBlock(sheet, inner,
                                   MediaQuery{condition}); // v1: inner media replaces outer
                    }
                    else if (IsKeyframes(prelude))
                    {
                        ParseKeyframes(sheet, Trim(prelude.SubStr(10, prelude.Size() - 10)),
                                       inner); // after "@keyframes"
                    }
                    // unknown at-rule blocks are consumed and skipped
                    continue;
                }

                // Normal rule: selector list up to '{'.
                const usize selStart = i;
                while (i < n && src[i] != u8'{' && src[i] != u8'}')
                    ++i;
                if (i >= n || src[i] != u8'{')
                    break;
                const core::StringView selectorList = src.SubStr(selStart, i - selStart);
                ++i; // consume '{'

                const usize blockStart = i;
                while (i < n && src[i] != u8'}')
                    ++i;
                const core::StringView block = src.SubStr(blockStart, i - blockStart);
                if (i < n)
                    ++i; // consume '}'

                EmitRules(sheet, selectorList, block, media);
            }
        }

        [[nodiscard]] static bool IsMedia(core::StringView prelude) noexcept
        {
            return prelude.Size() >= 6 && prelude.SubStr(0, 6) == core::StringView(u8"@media");
        }
        [[nodiscard]] static bool IsKeyframes(core::StringView prelude) noexcept
        {
            return prelude.Size() >= 10 &&
                   prelude.SubStr(0, 10) == core::StringView(u8"@keyframes");
        }

        // Parse the inner of @keyframes: a run of `<offset-list> { decls }` stops (offset is a
        // percentage, or from/to). Comma-separated offsets share a block.
        static void ParseKeyframes(StyleSheet& sheet, core::StringView name, core::StringView inner)
        {
            Keyframes keyframes;
            keyframes.Name = core::String(name);

            const usize n = inner.Size();
            usize i = 0;
            while (i < n)
            {
                while (i < n && IsWhiteSpace(inner[i]))
                    ++i;
                if (i >= n)
                    break;

                const usize selStart = i;
                while (i < n && inner[i] != u8'{')
                    ++i;
                if (i >= n)
                    break;
                const core::StringView offsetList = inner.SubStr(selStart, i - selStart);
                ++i; // consume '{'
                const usize blockStart = i;
                while (i < n && inner[i] != u8'}')
                    ++i;
                const core::StringView block = inner.SubStr(blockStart, i - blockStart);
                if (i < n)
                    ++i; // consume '}'

                // A stop per (comma-separated) offset, sharing the declarations.
                usize s = 0;
                const usize on = offsetList.Size();
                for (usize k = 0; k <= on; ++k)
                {
                    if (k == on || offsetList[k] == u8',')
                    {
                        const core::StringView spec = Trim(offsetList.SubStr(s, k - s));
                        f32 offset = 0.0f;
                        if (ParseKeyframeOffset(spec, offset))
                        {
                            KeyframeStop stop;
                            stop.Offset = offset;
                            stop.Properties = ParseDeclList(block);
                            keyframes.Stops.PushBack(core::Move(stop));
                        }
                        s = k + 1;
                    }
                }
            }
            sheet.AddKeyframes(core::Move(keyframes));
        }

        // "0%".."100%" -> 0..1; "from" -> 0; "to" -> 1. Returns false if unrecognized.
        [[nodiscard]] static bool ParseKeyframeOffset(core::StringView spec, f32& out) noexcept
        {
            if (spec == core::StringView(u8"from"))
            {
                out = 0.0f;
                return true;
            }
            if (spec == core::StringView(u8"to"))
            {
                out = 1.0f;
                return true;
            }
            usize len = spec.Size();
            if (len == 0)
                return false;
            if (spec[len - 1] == u8'%')
                --len; // strip trailing %
            f32 value = 0.0f, scale = 1.0f;
            bool seenDot = false, any = false;
            for (usize k = 0; k < len; ++k)
            {
                const char8_t c = spec[k];
                if (c == u8'.')
                {
                    seenDot = true;
                    continue;
                }
                if (c < u8'0' || c > u8'9')
                    return false;
                any = true;
                if (seenDot)
                {
                    scale *= 0.1f;
                    value += static_cast<f32>(c - u8'0') * scale;
                }
                else
                    value = value * 10.0f + static_cast<f32>(c - u8'0');
            }
            if (!any)
                return false;
            out = value * 0.01f; // percent -> fraction
            return true;
        }

        // One rule per comma-separated selector, each carrying the block's declarations + media.
        static void EmitRules(StyleSheet& sheet, core::StringView selectorList,
                              core::StringView block, const MediaQuery& media)
        {
            // Commas inside :is(), :not() or [a="b,c"] do not split.
            for (const core::StringView selector : StyleSelector::SplitList(selectorList))
            {
                StyleRule rule{selector};
                ApplyDeclarations(rule, block);
                if (!media.IsEmpty())
                    rule.SetMedia(media);
                sheet.AddRule(core::Move(rule));
            }
        }

        // Split `name: value;` declarations into the rule (later same-name wins via SetProperty).
        static void ApplyDeclarations(StyleRule& rule, core::StringView block)
        {
            for (const StyleProperty& p : ParseDeclList(block))
                rule.SetProperty(p.Name.AsView(), p.Value.AsView(), p.Important);
        }

        // Parse `name: value;` declarations into a flat property list (in source order).
        [[nodiscard]] static Array<StyleProperty> ParseDeclList(core::StringView block)
        {
            Array<StyleProperty> out;
            usize start = 0;
            const usize n = block.Size();
            for (usize i = 0; i <= n; ++i)
            {
                if (i == n || block[i] == u8';')
                {
                    const core::StringView decl = block.SubStr(start, i - start);
                    const usize colon = FindChar(decl, u8':');
                    if (colon != kNotFound)
                    {
                        const core::StringView name = Trim(decl.SubStr(0, colon));
                        core::StringView value =
                            Trim(decl.SubStr(colon + 1, decl.Size() - colon - 1));

                        // Trailing !important.
                        bool important = false;
                        const core::StringView flag(u8"!important");
                        if (value.Size() >= flag.Size() &&
                            value.SubStr(value.Size() - flag.Size(), flag.Size()) == flag)
                        {
                            important = true;
                            value = Trim(value.SubStr(0, value.Size() - flag.Size()));
                        }

                        if (name.Size() != 0)
                            out.PushBack(StyleProperty(name, value, important));
                    }
                    start = i + 1;
                }
            }
            return out;
        }

        static constexpr usize kNotFound = static_cast<usize>(-1);
        [[nodiscard]] static usize FindChar(core::StringView s, char8_t c) noexcept
        {
            for (usize i = 0; i < s.Size(); ++i)
                if (s[i] == c)
                    return i;
            return kNotFound;
        }
    };
}
