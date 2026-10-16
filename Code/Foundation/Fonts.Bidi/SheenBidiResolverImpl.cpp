// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.Bidi - the SheenBidi resolver (implementation unit; SheenBidi stays here).
//
// Resolves a paragraph of UTF-32 codepoints, so its levels come back one per character, the unit
// GlyphPosition clusters count. Each call makes and releases its own SheenBidi objects, so the one
// resolver serves every shaper and thread.

module;
#include "Core/Prelude.h"
#include <SheenBidi/SheenBidi.h>

module foundation.fonts.bidi;

import foundation.core;
import foundation.fonts;

using namespace foundation::core;

namespace foundation::fonts
{
    namespace
    {
        class SheenBidiResolver final : public IBidiResolver
        {
        public:
            void Resolve(Span<const u32> codepoints, TextDirection base, BidiLevels& out) override
            {
                out.levels.Clear();
                out.paragraphLevel = base == TextDirection::RightToLeft ? 1 : 0;
                if (codepoints.IsEmpty())
                    return;
                SBCodepointSequence sequence{SBStringEncodingUTF32, codepoints.Data(),
                                             static_cast<SBUInteger>(codepoints.Size())};
                SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
                if (algorithm == nullptr)
                    return;
                const SBLevel baseLevel = base == TextDirection::LeftToRight    ? SBLevel(0)
                                          : base == TextDirection::RightToLeft ? SBLevel(1)
                                                                               : SBLevel(SBLevelDefaultLTR);
                SBParagraphRef paragraph =
                    SBAlgorithmCreateParagraph(algorithm, 0, static_cast<SBUInteger>(codepoints.Size()), baseLevel);
                if (paragraph != nullptr)
                {
                    out.paragraphLevel = static_cast<u8>(SBParagraphGetBaseLevel(paragraph));
                    const SBUInteger length = SBParagraphGetLength(paragraph);
                    const SBLevel* levels = SBParagraphGetLevelsPtr(paragraph);
                    for (usize i = 0; i < codepoints.Size(); ++i)
                        out.levels.PushBack(i < length ? static_cast<u8>(levels[i]) : out.paragraphLevel);
                    SBParagraphRelease(paragraph);
                }
                SBAlgorithmRelease(algorithm);
            }
        };

        SheenBidiResolver& Instance()
        {
            static SheenBidiResolver resolver;
            return resolver;
        }
    }

    void BidiAlgorithm::Install() { BidiResolverFactory::Install(&Instance()); }
    void BidiAlgorithm::Uninstall() { BidiResolverFactory::Remove(&Instance()); }
    bool BidiAlgorithm::IsInstalled() { return BidiResolverFactory::Resolver() == &Instance(); }
    IBidiResolver& BidiAlgorithm::Resolver() { return Instance(); }
}
