// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:init partition
//
// Registers and unregisters the TTF parser and atlas baker with the IO factories.
// Initialize news the parser and baker and registers them; Shutdown unregisters
// them (so the factory won't also free them) and deletes them. Idempotent.

module;
#include "Core/Prelude.h"

export module foundation.fonts.truetype:init;

import foundation.core;
import foundation.fonts;
import :parser;
import :atlas_baker;

using namespace foundation::core;

namespace foundation::fonts
{
    TrueTypeFontParser*& ParserSlot()
    {
        static TrueTypeFontParser* p = nullptr;
        return p;
    }
    TrueTypeFontAtlasBaker*& BakerSlot()
    {
        static TrueTypeFontAtlasBaker* b = nullptr;
        return b;
    }
}

export namespace foundation::fonts
{
    class TrueTypeFonts
    {
    public:
        // Register the TTF parser and atlas baker with their factories. Idempotent.
        static void Initialize()
        {
            if (ParserSlot() == nullptr)
            {
                ParserSlot() = DefaultAllocator().New<TrueTypeFontParser>();
                FontParserFactory::RegisterParser(ParserSlot());
            }
            if (BakerSlot() == nullptr)
            {
                BakerSlot() = DefaultAllocator().New<TrueTypeFontAtlasBaker>();
                FontAtlasBakerFactory::RegisterBaker(BakerSlot());
            }
        }

        // Unregister and delete the TTF parser and baker.
        static void Shutdown()
        {
            if (ParserSlot() != nullptr)
            {
                FontParserFactory::UnregisterParser(ParserSlot());
                DefaultAllocator().Delete(ParserSlot());
                ParserSlot() = nullptr;
            }
            if (BakerSlot() != nullptr)
            {
                FontAtlasBakerFactory::UnregisterBaker(BakerSlot());
                DefaultAllocator().Delete(BakerSlot());
                BakerSlot() = nullptr;
            }
        }

        [[nodiscard]] static bool IsInitialized()
        {
            return ParserSlot() != nullptr && BakerSlot() != nullptr;
        }
    };
}
