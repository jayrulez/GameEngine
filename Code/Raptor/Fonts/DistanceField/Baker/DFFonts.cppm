// Raptor::FontsDFBaker — raptor.fonts.df.baker:init partition
//
// Registration helper for the distance-field atlas baker.

module;
#include "Core/Prelude.h"

export module raptor.fonts.df.baker:init;

import raptor.core;
import raptor.fonts;
import raptor.fonts.io;
import :baker;

using namespace raptor::core;

export namespace raptor::fonts
{

class DFFonts
{
public:
    static void Initialize()
    {
        if (s_baker) return;
        s_baker = DefaultAllocator().New<DFFontAtlasBaker>();
        FontAtlasBakerFactory::RegisterBaker(s_baker);
    }

    static void Shutdown()
    {
        if (!s_baker) return;
        FontAtlasBakerFactory::UnregisterBaker(s_baker);
        DefaultAllocator().Delete(s_baker);
        s_baker = nullptr;
    }

    [[nodiscard]] static bool IsInitialized() { return s_baker != nullptr; }

private:
    static inline DFFontAtlasBaker* s_baker = nullptr;
};

} // namespace raptor::fonts
