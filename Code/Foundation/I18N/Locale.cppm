// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - :locale partition
//
// Locale ids shaped as BCP 47 (`en`, `pt-BR`, `zh-Hans-CN`), with no database of locales: a
// locale's parent, a normal form for comparing, and whether an id has the shape. Which way a
// language is written and how it counts are the string tables' to say, never this code's.

module;
#include "Core/Prelude.h"

export module foundation.i18n:locale;

import foundation.core;

using namespace foundation::core;

export namespace foundation::i18n
{
    /// The locale one subtag shorter (`pt-BR` to `pt`, `zh-Hans-CN` to `zh-Hans`); empty when there
    /// is none. Takes `-` or `_` between subtags.
    [[nodiscard]] inline StringView ParentLocale(StringView locale) noexcept
    {
        for (usize i = locale.Size(); i-- > 0;)
            if (locale[i] == u8'-' || locale[i] == u8'_')
                return locale.SubStr(0, i);
        return {};
    }

    /// The form two ids compare in: the language lowercased, subtags joined by `-`, the rest as
    /// written (`en_us` and `en-us` are both `en-us`; `EN-US` is `en-US`).
    [[nodiscard]] inline String NormalizeLocale(StringView locale)
    {
        String out;
        bool inLanguage = true;
        for (usize i = 0; i < locale.Size(); ++i)
        {
            utf8char c = locale[i];
            if (c == u8'_' || c == u8'-')
            {
                inLanguage = false;
                out.Append(StringView(u8"-"));
                continue;
            }
            if (inLanguage && c >= u8'A' && c <= u8'Z')
                c = static_cast<utf8char>(c - u8'A' + u8'a');
            out.Append(StringView(&c, 1));
        }
        return out;
    }

    /// Whether two ids name the same locale, by their normal forms and ignoring the case of the
    /// region and script subtags as well.
    [[nodiscard]] inline bool SameLocale(StringView a, StringView b) noexcept
    {
        if (a.Size() != b.Size())
            return false;
        for (usize i = 0; i < a.Size(); ++i)
        {
            utf8char x = a[i], y = b[i];
            if (x == u8'_')
                x = u8'-';
            if (y == u8'_')
                y = u8'-';
            if (x >= u8'A' && x <= u8'Z')
                x = static_cast<utf8char>(x - u8'A' + u8'a');
            if (y >= u8'A' && y <= u8'Z')
                y = static_cast<utf8char>(y - u8'A' + u8'a');
            if (x != y)
                return false;
        }
        return true;
    }

    /// Whether `locale` has a locale's shape: a language of two or three letters, then a script of
    /// four letters, a region of two letters or three digits, or both in that order (`en`, `fil`,
    /// `pt-BR`, `zh-Hans`, `zh-Hans-CN`, `es-419`). The shape only: never whether it exists.
    [[nodiscard]] inline bool IsLocaleShaped(StringView locale) noexcept
    {
        const auto letter = [](utf8char c) { return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z'); };
        const auto digit = [](utf8char c) { return c >= u8'0' && c <= u8'9'; };
        StringView parts[4];
        usize count = 0;
        usize start = 0;
        for (usize i = 0; i <= locale.Size(); ++i)
        {
            if (i == locale.Size() || locale[i] == u8'-' || locale[i] == u8'_')
            {
                if (count == 4)
                    return false;
                parts[count++] = locale.SubStr(start, i - start);
                start = i + 1;
            }
        }
        const auto all = [](StringView s, auto test)
        {
            for (usize i = 0; i < s.Size(); ++i)
                if (!test(s[i]))
                    return false;
            return true;
        };
        if (parts[0].Size() < 2 || parts[0].Size() > 3 || !all(parts[0], letter))
            return false;
        usize at = 1;
        if (at < count && parts[at].Size() == 4 && all(parts[at], letter))
            ++at; // a script
        if (at < count && ((parts[at].Size() == 2 && all(parts[at], letter)) || (parts[at].Size() == 3 && all(parts[at], digit))))
            ++at; // a region
        return at == count;
    }
}
