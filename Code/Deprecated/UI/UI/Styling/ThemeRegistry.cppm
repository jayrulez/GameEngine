// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_registry partition
//
// Central registry for theme extensions, applied to every theme StyleSheet created by the
// theme factories. Ported from Sedulous.UI/src/Styling/ThemeRegistry.bf.
//
// Divergence (language): Beef's static list OWNED the extensions (deleted on shutdown). Here the
// registry holds NON-owning pointers - the app owns each extension's lifetime (avoids C++ static
// destruction-order pitfalls). The list lives in a function-local static.

module;
#include "Core/Prelude.h"

export module foundation.ui:theme_registry;

import foundation.core; // Array
import :style_sheet;
import :theme_palette;
import :theme_extension;

using namespace foundation::core;

namespace foundation::ui::detail
{
    // NON-inline (UiRegistryStateImpl.cpp): shared-libraries.md rendezvous rule.
    [[nodiscard]] Array<IThemeExtension*>& ThemeExtensionList();
}

export namespace foundation::ui
{
    struct ThemeRegistry
    {
        /// Register an extension (applied to all themes created afterward). No-op if already present.
        static void RegisterExtension(IThemeExtension* ext)
        {
            if (ext == nullptr)
            {
                return;
            }
            Array<IThemeExtension*>& list = detail::ThemeExtensionList();
            for (IThemeExtension* e : list)
            {
                if (e == ext)
                {
                    return;
                }
            }
            list.PushBack(ext);
        }

        /// Unregister an extension.
        static void UnregisterExtension(IThemeExtension* ext)
        {
            Array<IThemeExtension*>& list = detail::ThemeExtensionList();
            for (usize i = 0; i < list.Size(); ++i)
            {
                if (list[i] == ext)
                {
                    list.RemoveAt(i);
                    return;
                }
            }
        }

        /// Apply all registered extensions to a theme sheet.
        static void ApplyExtensions(StyleSheet& sheet, ThemePalette palette)
        {
            for (IThemeExtension* ext : detail::ThemeExtensionList())
            {
                ext->Apply(sheet, palette);
            }
        }
    };
}
