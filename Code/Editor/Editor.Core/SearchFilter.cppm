// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor Core - :search_filter partition
//
// What the editor's filter fields find, the asset browser's and picker's and the hierarchy's and
// entity picker's alike: a thing whose name contains the text (any case), or whose guid begins
// with it - so a guid copied from a log line, an MCP answer or a scene file finds what it names,
// written whole or as its first few digits, with or without braces and dashes, in any case. A
// whole guid (FilterAsGuid) is a request for that one thing: the fields select it.
module;
#include "Core/Prelude.h"

export module editor.core:search_filter;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    namespace search_filter_detail
    {
        [[nodiscard]] constexpr utf8char Lower(utf8char c) noexcept
        {
            return (c >= utf8char('A') && c <= utf8char('Z')) ? static_cast<utf8char>(c + 32) : c;
        }
        [[nodiscard]] constexpr bool IsHex(utf8char c) noexcept
        {
            const utf8char l = Lower(c);
            return (l >= utf8char('0') && l <= utf8char('9')) || (l >= utf8char('a') && l <= utf8char('f'));
        }

        /// The filter's hex digits, lowercased, when it reads as (part of) a guid: hex digits,
        /// dashes, braces and surrounding spaces only, at least 4 digits. Empty otherwise.
        [[nodiscard]] inline String GuidDigits(StringView filter)
        {
            String digits;
            for (const utf8char c : filter)
            {
                if (IsHex(c))
                {
                    digits.Append(Lower(c));
                }
                else if (c != utf8char('-') && c != utf8char('{') && c != utf8char('}') && c != utf8char(' '))
                {
                    return {};
                }
            }
            return digits.Size() >= 4 ? digits : String{};
        }

        [[nodiscard]] inline bool NameContains(StringView name, StringView filter)
        {
            if (name.Size() < filter.Size())
            {
                return false;
            }
            for (usize i = 0; i + filter.Size() <= name.Size(); ++i)
            {
                bool match = true;
                for (usize j = 0; j < filter.Size() && match; ++j)
                {
                    match = Lower(name[i + j]) == Lower(filter[j]);
                }
                if (match)
                {
                    return true;
                }
            }
            return false;
        }
    }

    /// Whether the filter field's text finds the thing called `name` with id `id`: empty finds
    /// everything; otherwise its name contains the text (any case), or its guid begins with the
    /// text's hex digits.
    [[nodiscard]] inline bool NameOrGuidMatches(StringView name, const Guid& id, StringView filter)
    {
        if (filter.IsEmpty() || search_filter_detail::NameContains(name, filter))
        {
            return true;
        }
        const String digits = search_filter_detail::GuidDigits(filter);
        if (digits.IsEmpty())
        {
            return false;
        }
        utf8char text[37];
        id.ToChars(text);
        usize matched = 0;
        for (usize i = 0; i < 36 && matched < digits.Size(); ++i)
        {
            if (text[i] == utf8char('-'))
            {
                continue;
            }
            if (text[i] != digits.AsView()[matched])
            {
                return false;
            }
            ++matched;
        }
        return matched == digits.Size();
    }

    /// The guid the filter spells whole (all 32 digits; braces, case and dashes aside), if it does.
    [[nodiscard]] inline bool FilterAsGuid(StringView filter, Guid& out)
    {
        const String digits = search_filter_detail::GuidDigits(filter);
        if (digits.Size() != 32)
        {
            return false;
        }
        String canonical;
        for (usize i = 0; i < 32; ++i)
        {
            if (i == 8 || i == 12 || i == 16 || i == 20)
            {
                canonical.Append(utf8char('-'));
            }
            canonical.Append(digits.AsView()[i]);
        }
        return Guid::TryParse(canonical.AsView(), out);
    }
}
