// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor App - :property_names partition
//
// A row's label from its reflected property's name, as the scene inspector and the asset pages
// write it.
module;
#include "Core/Prelude.h"

export module editor.app:property_names;

import foundation.core;

using namespace foundation::core;

export namespace editor::app
{
    /// "castsShadows" -> "Casts Shadows", "fovYRadians" -> "Fov Y Radians", "IBL" -> "IBL".
    [[nodiscard]] inline String PrettifyPropertyName(StringView name)
    {
        const utf8char* d = name.Data();
        String out;
        bool prevLower = false;
        bool prevUpper = false;
        for (usize i = 0; i < name.Size(); ++i)
        {
            utf8char c = d[i];
            const bool upper = (c >= u8'A' && c <= u8'Z');
            const bool lower = (c >= u8'a' && c <= u8'z');
            if (i == 0 && lower)
            {
                c = static_cast<utf8char>(c - (u8'a' - u8'A'));
            }
            else if (upper)
            {
                const bool nextLower =
                    (i + 1 < name.Size()) && (d[i + 1] >= u8'a' && d[i + 1] <= u8'z');
                if (prevLower || (prevUpper && nextLower))
                {
                    out += u8' ';
                }
            }
            out += c;
            prevLower = lower;
            prevUpper = upper;
        }
        return out;
    }
}
