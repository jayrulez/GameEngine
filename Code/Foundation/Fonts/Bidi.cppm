// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - :bidi partition
//
// The bidirectional algorithm's seam (F3 of Specs/vg-and-gui-lab.md): an IBidiResolver gives a
// paragraph's embedding levels (the hard part of UAX #9, behind this interface; Fonts.Bidi is
// one implementation), and ReorderLine lays a line's characters out in visual runs from those
// levels (rules L1 and L2, simple enough to keep here). Shapers ask BidiResolverFactory for the
// registered resolver; with none, a line is one run in the request's direction.

module;
#include "Core/Prelude.h"

export module foundation.fonts:bidi;

import foundation.core;
import :types;

using namespace foundation::core;

export namespace foundation::fonts
{
    /// A paragraph's resolved embedding levels, one per character: even reads left to right, odd
    /// right to left; the paragraph's own level is its base direction.
    struct BidiLevels
    {
        Array<u8> levels;
        u8 paragraphLevel = 0;
    };

    /// Characters [start, end) of a paragraph at one level, a run of a line in visual order.
    struct BidiRun
    {
        usize start = 0;
        usize end = 0;
        u8 level = 0;
        [[nodiscard]] bool RightToLeft() const noexcept { return (level & 1u) != 0; }
    };

    /// Resolves the embedding levels of a paragraph (UAX #9 up to rule I2).
    class IBidiResolver
    {
    public:
        virtual ~IBidiResolver() = default;
        /// The levels of one paragraph (no paragraph separator inside), its base direction given, or
        /// (Auto) taken from its first strong character.
        virtual void Resolve(Span<const u32> codepoints, TextDirection base, BidiLevels& out) = 0;
    };

    /// The process-wide resolver shapers use. A resolver keeps no state between calls, so one
    /// instance serves every shaper and thread; its owner installs it and removes it before it goes.
    /// Null with none installed: a line is one run in the request's direction.
    class BidiResolverFactory
    {
    public:
        static void Install(IBidiResolver* resolver) { Slot() = resolver; }
        static void Remove(IBidiResolver* resolver)
        {
            if (Slot() == resolver)
                Slot() = nullptr;
        }
        [[nodiscard]] static IBidiResolver* Resolver() { return Slot(); }

    private:
        static IBidiResolver*& Slot(); // FontsImpl.cpp: one per process
    };

    /// Whitespace for rule L1 (a line's trailing whitespace takes the paragraph's level).
    [[nodiscard]] constexpr bool IsBidiWhitespace(u32 cp) noexcept
    {
        return cp == U' ' || cp == U'\t' || cp == 0xA0 || cp == 0x2000 || cp == 0x3000 || (cp >= 0x2001 && cp <= 0x200A);
    }

    /// The visual runs of characters [start, end) of a paragraph: its line's trailing whitespace at
    /// the paragraph level (L1), then every maximal sequence at or above each level reversed, from
    /// the highest level down to the lowest odd one (L2). Runs come out left to right.
    inline void ReorderLine(const BidiLevels& paragraph, Span<const u32> codepoints, usize start, usize end,
                            Array<BidiRun>& outRuns)
    {
        outRuns.Clear();
        if (start >= end || end > paragraph.levels.Size())
            return;
        Array<u8> levels;
        for (usize i = start; i < end; ++i)
            levels.PushBack(paragraph.levels[i]);
        for (usize i = end; i > start && i - 1 < codepoints.Size() && IsBidiWhitespace(codepoints[i - 1]); --i)
            levels[i - 1 - start] = paragraph.paragraphLevel;
        // Logical runs of one level.
        u8 highest = 0;
        u8 lowestOdd = 0xFF;
        for (usize i = 0; i < levels.Size();)
        {
            usize j = i + 1;
            while (j < levels.Size() && levels[j] == levels[i])
                ++j;
            BidiRun run;
            run.start = start + i;
            run.end = start + j;
            run.level = levels[i];
            outRuns.PushBack(run);
            highest = Max(highest, run.level);
            if ((run.level & 1u) != 0)
                lowestOdd = Min(lowestOdd, run.level);
            i = j;
        }
        if (lowestOdd == 0xFF)
            return; // all left to right
        for (u8 level = highest; level >= lowestOdd; --level)
        {
            for (usize i = 0; i < outRuns.Size();)
            {
                if (outRuns[i].level < level)
                {
                    ++i;
                    continue;
                }
                usize j = i;
                while (j < outRuns.Size() && outRuns[j].level >= level)
                    ++j;
                for (usize a = i, b = j; a + 1 < b; ++a, --b)
                    Swap(outRuns[a], outRuns[b - 1]);
                i = j;
            }
            if (level == 0)
                break;
        }
    }
}
