// Draconic UI - :theme_registry partition
//
// Central registry for theme extensions, applied to every theme StyleSheet created by the
// theme factories. Ported from Sedulous.UI/src/Styling/ThemeRegistry.bf.
//
// Divergence (language): Beef's static list OWNED the extensions (deleted on shutdown). Here the
// registry holds NON-owning pointers - the app owns each extension's lifetime (avoids C++ static
// destruction-order pitfalls). The list lives in a function-local static.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:theme_registry;

import draconic.foundation; // Array
import :style_sheet;
import :theme_palette;
import :theme_extension;

using namespace draconic::foundation;

namespace draconic::ui::detail
{
    inline Array<IThemeExtension*>& ThemeExtensionList()
    {
        static Array<IThemeExtension*> extensions;
        return extensions;
    }
}

export namespace draconic::ui
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
