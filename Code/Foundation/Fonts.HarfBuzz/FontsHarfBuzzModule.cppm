// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.HarfBuzz - the `foundation.fonts.harfbuzz` module.
//
// Real text shaping behind the engine's ITextShaper: ligatures,
// Arabic joining, Indic reordering, marks placed by the font's own tables. Install() makes it the
// shaper every font service hands out (TextShaperFactory); a font whose file is not at hand (a
// hand-built table) is shaped by the basic shaper instead. HarfBuzz itself never crosses this
// interface: the shaper is defined in the implementation unit.

module;
#include "Core/Prelude.h"

export module foundation.fonts.harfbuzz;

import foundation.core;
import foundation.fonts;

using namespace foundation::core;

export namespace foundation::fonts
{
    class HarfBuzzShaping
    {
    public:
        /// Register the HarfBuzz shaper with TextShaperFactory (idempotent): fonts made after this
        /// shape through it.
        static void Install();
        /// Unregister it: fonts made after this get the basic shaper again.
        static void Uninstall();
        [[nodiscard]] static bool IsInstalled();
        /// A HarfBuzz shaper, allocated from `allocator` (the font service's), owned by the caller.
        [[nodiscard]] static ITextShaper* CreateShaper(IAllocator& allocator);
    };
}
