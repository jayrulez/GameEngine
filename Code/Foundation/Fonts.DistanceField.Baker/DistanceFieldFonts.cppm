// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.DistanceField.Baker - foundation.fonts.distancefield.baker:init partition
//
// Registration helper for the distance-field atlas baker.

module;
#include "Core/Prelude.h"

export module foundation.fonts.distancefield.baker:init;

import foundation.core;
import foundation.fonts;
import :baker;

using namespace foundation::core;

export namespace foundation::fonts
{

    class DistanceFieldFonts
    {
    public:
        // Idempotent and thread-safe: the font cook calls this per build on job workers, and
        // a plain null check would let two of them each register a baker (the second leaking
        // and the factory list holding both). The initializer runs once and blocks concurrent
        // callers; Shutdown re-arms it for the next Initialize.
        static void Initialize()
        {
            static bool armed = false; // set once the baker exists; cleared by Shutdown
            static Mutex lock;
            ScopedLock guard(lock);
            if (armed && s_baker != nullptr)
                return;
            s_baker = DefaultAllocator().New<DistanceFieldFontAtlasBaker>();
            FontAtlasBakerFactory::RegisterBaker(s_baker);
            armed = true;
        }

        static void Shutdown()
        {
            if (!s_baker)
                return;
            FontAtlasBakerFactory::UnregisterBaker(s_baker);
            DefaultAllocator().Delete(s_baker);
            s_baker = nullptr;
        }

        [[nodiscard]] static bool IsInitialized() { return s_baker != nullptr; }

    private:
        static inline DistanceFieldFontAtlasBaker* s_baker = nullptr;
    };

} // namespace foundation::fonts
