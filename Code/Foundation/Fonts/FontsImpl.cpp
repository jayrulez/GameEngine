// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Process-wide state for foundation.fonts, defined here (one instance per process) because a
// backend registers from one library and font services read from others; a function-local static
// in a module interface would be duplicated per shared library.

module;
#include "Core/Prelude.h"

module foundation.fonts;

import foundation.core;

using namespace foundation::core;

namespace foundation::fonts
{
    Array<TextShaperFactory::Creator>& TextShaperFactory::Store()
    {
        static Array<Creator> store;
        return store;
    }

    IBidiResolver*& BidiResolverFactory::Slot()
    {
        static IBidiResolver* resolver = nullptr;
        return resolver;
    }
}
