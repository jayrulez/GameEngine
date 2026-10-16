// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.Bidi - the `foundation.fonts.bidi` module.
//
// Mixed-direction paragraphs (F3 of Specs/vg-and-gui-lab.md): Install() puts the Unicode
// bidirectional algorithm behind BidiResolverFactory, so every shaper lays a line out in visual
// order (Arabic or Hebrew with Latin inside, or the reverse). SheenBidi does the resolving and
// never crosses this interface: the resolver is defined in the implementation unit.

module;
#include "Core/Prelude.h"

export module foundation.fonts.bidi;

import foundation.core;
import foundation.fonts;

using namespace foundation::core;

export namespace foundation::fonts
{
    class BidiAlgorithm
    {
    public:
        /// Install the resolver (idempotent): lines shaped after this reorder by their levels.
        static void Install();
        /// Remove it: a line is one run in its request's direction again.
        static void Uninstall();
        [[nodiscard]] static bool IsInstalled();
        /// The resolver itself (one per process, stateless), for a caller that resolves levels.
        [[nodiscard]] static IBidiResolver& Resolver();
    };
}
