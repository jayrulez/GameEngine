// Draconic::FontsTTF - draconic.fonts.ttf:init partition
//
// Registers/unregisters the TTF parser + atlas baker with the IO factories.
// Ported from Sedulous.Fonts.TTF/TrueTypeFonts.bf. Initialize news the parser
// + baker and registers them; Shutdown unregisters (so the factory won't also
// free them) and deletes them. Idempotent.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.ttf:init;

import draconic.foundation;
import draconic.fonts.io;
import :parser;
import :atlas_baker;

using namespace draconic::foundation;

namespace draconic::fonts
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

export namespace draconic::fonts
{
    class TrueTypeFonts
    {
    public:
        // Register the TTF parser + atlas baker with their factories. Idempotent.
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

        // Unregister + delete the TTF parser + baker.
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
