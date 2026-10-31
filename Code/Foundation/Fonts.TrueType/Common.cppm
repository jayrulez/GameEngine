// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:common partition
//
// Small helpers shared by the TTF parser and atlas baker: the supported
// extension list and a case-insensitive extension compare. They live once here
// to avoid cross-TU inline-symbol clashes in C++ modules.

module;
#include "Core/Prelude.h"

export module foundation.fonts.truetype:common;

import foundation.core;

using namespace foundation::core;

export namespace foundation::fonts
{
    // ASCII case-insensitive compare for extension matching.
    [[nodiscard]] inline bool ExtEquals(StringView a, StringView b)
    {
        if (a.Size() != b.Size())
            return false;
        for (usize i = 0; i < a.Size(); ++i)
        {
            utf8char ca = a[i], cb = b[i];
            if (ca >= utf8char('A') && ca <= utf8char('Z'))
                ca = static_cast<utf8char>(static_cast<u8>(ca) - 'A' + 'a');
            if (cb >= utf8char('A') && cb <= utf8char('Z'))
                cb = static_cast<utf8char>(static_cast<u8>(cb) - 'A' + 'a');
            if (ca != cb)
                return false;
        }
        return true;
    }

    // .ttf, .ttc and .otf: the formats the TTF backend handles.
    [[nodiscard]] inline Span<const StringView> TrueTypeExtensions()
    {
        static const StringView exts[] = {StringView(u8".ttf"), StringView(u8".ttc"),
                                          StringView(u8".otf")};
        return Span<const StringView>(exts, 3);
    }
}
