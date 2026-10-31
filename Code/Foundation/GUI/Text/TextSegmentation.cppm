// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_segmentation partition
//
// Where a caret may stand and what a word is, over UTF-8 byte offsets. A grapheme is what a
// reader sees as one character: a base with the marks that combine with it, CR LF, an emoji with
// its modifiers, variation selectors and zero-width-joined parts, a flag's pair of regional
// indicators. The code implements the UAX #29 rules that matter to an editor, over tables of the
// common ranges rather than the whole Unicode database. Neither fonts nor core segments text, and
// these rules keep a caret from splitting "é" written as e and U+0301.
//
// Words are runs of letters, digits and '_' (any character past ASCII that is not a space or
// punctuation counts as a letter). Moving or deleting by word to the right passes the rest of a
// word and the gap after it; to the left, it passes the gap and then the word. It never crosses a
// line break.

module;
#include "Core/Prelude.h"

export module foundation.gui:text_segmentation;

import foundation.core; // StringView, DecodeUtf8, Utf8PrevBoundary, Utf8NextBoundary

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    namespace detail
    {
        struct CodeRange
        {
            u32 First, Last;
        };
        // Marks that combine with what comes before them (Unicode's Extend and SpacingMark,
        // the common ranges).
        inline constexpr CodeRange kExtend[] = {
            {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
            {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
            {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0903},
            {0x093A, 0x093C}, {0x093E, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0981, 0x0983},
            {0x09BC, 0x09BC}, {0x09BE, 0x09CD}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
            {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200C, 0x200C}, {0x20D0, 0x20FF}, {0x302A, 0x302F},
            {0x3099, 0x309A}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0x1F3FB, 0x1F3FF}, {0xE0020, 0xE007F},
            {0xE0100, 0xE01EF},
        };
        [[nodiscard]] constexpr bool InRanges(u32 c, const CodeRange* ranges, usize count) noexcept
        {
            for (usize i = 0; i < count; ++i)
                if (c >= ranges[i].First && c <= ranges[i].Last)
                    return true;
            return false;
        }
        [[nodiscard]] constexpr bool IsExtend(u32 c) noexcept
        {
            return c >= 0x0300 && InRanges(c, kExtend, sizeof(kExtend) / sizeof(kExtend[0]));
        }
        inline constexpr u32 kZeroWidthJoiner = 0x200D;
        [[nodiscard]] constexpr bool IsRegionalIndicator(u32 c) noexcept { return c >= 0x1F1E6 && c <= 0x1F1FF; }

        // Returns the codepoint at `offset` (which is on a codepoint boundary) and sets `next` to
        // where the next one starts.
        [[nodiscard]] inline u32 CodepointAt(core::StringView text, usize offset, usize& next)
        {
            next = offset;
            return core::DecodeUtf8(text, next);
        }
        // Returns the codepoint before `offset` and sets `start` to where it starts.
        [[nodiscard]] inline u32 CodepointBefore(core::StringView text, usize offset, usize& start)
        {
            start = core::Utf8PrevBoundary(text, offset);
            usize next = start;
            return core::DecodeUtf8(text, next);
        }
    }

    /// Returns whether a caret may stand between the codepoints before and after `offset`.
    [[nodiscard]] inline bool IsGraphemeBoundary(core::StringView text, usize offset)
    {
        if (offset == 0 || offset >= text.Size())
            return true;
        usize next = 0, start = 0;
        const u32 after = detail::CodepointAt(text, offset, next);
        const u32 before = detail::CodepointBefore(text, offset, start);
        if (before == u8'\r' && after == u8'\n')
            return false;
        if (before == u8'\r' || before == u8'\n' || after == u8'\r' || after == u8'\n')
            return true;
        if (detail::IsExtend(after) || after == detail::kZeroWidthJoiner)
            return false;
        if (before == detail::kZeroWidthJoiner && after >= 0x80)
            return false; // a part after a zero-width joiner stays with the emoji
        if (detail::IsRegionalIndicator(before) && detail::IsRegionalIndicator(after))
        {
            // Flags pair up: a break falls after an even count of indicators.
            usize count = 0;
            usize at = offset;
            while (at > 0)
            {
                usize s = 0;
                if (!detail::IsRegionalIndicator(detail::CodepointBefore(text, at, s)))
                    break;
                ++count;
                at = s;
            }
            return count % 2 == 0;
        }
        return true;
    }

    /// Returns the next caret position after `offset`, at most the text's end.
    [[nodiscard]] inline usize NextGrapheme(core::StringView text, usize offset)
    {
        if (offset >= text.Size())
            return text.Size();
        usize at = core::Utf8NextBoundary(text, offset);
        while (at < text.Size() && !IsGraphemeBoundary(text, at))
            at = core::Utf8NextBoundary(text, at);
        return at;
    }
    /// Returns the caret position before `offset`, at least 0.
    [[nodiscard]] inline usize PrevGrapheme(core::StringView text, usize offset)
    {
        if (offset == 0)
            return 0;
        usize at = core::Utf8PrevBoundary(text, core::Min(offset, text.Size()));
        while (at > 0 && !IsGraphemeBoundary(text, at))
            at = core::Utf8PrevBoundary(text, at);
        return at;
    }
    /// Returns how many graphemes `text` holds, which is what a reader counts as characters.
    [[nodiscard]] inline usize GraphemeCount(core::StringView text)
    {
        usize count = 0;
        for (usize at = 0; at < text.Size(); at = NextGrapheme(text, at))
            ++count;
        return count;
    }

    /// How word movement classifies a character.
    enum class WordClass : u8
    {
        Word,
        Space,
        Punctuation,
        LineBreak,
    };

    [[nodiscard]] constexpr WordClass ClassifyForWords(u32 c) noexcept
    {
        if (c == u8'\n' || c == u8'\r')
            return WordClass::LineBreak;
        if (c == u8' ' || c == u8'\t' || c == 0xA0 || (c >= 0x2000 && c <= 0x200B) || c == 0x3000)
            return WordClass::Space;
        if ((c >= u8'0' && c <= u8'9') || (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || c == u8'_')
            return WordClass::Word;
        if (c < 0x80)
            return WordClass::Punctuation;
        if ((c >= 0x2010 && c <= 0x206F) || (c >= 0x3001 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) ||
            (c >= 0x00A1 && c <= 0x00BF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7)
            return WordClass::Punctuation;
        return WordClass::Word; // a letter of some script, or a mark (which stays with its word)
    }

    namespace detail
    {
        [[nodiscard]] inline WordClass ClassAt(core::StringView text, usize offset)
        {
            usize next = 0;
            return ClassifyForWords(CodepointAt(text, offset, next));
        }
        [[nodiscard]] inline WordClass ClassBefore(core::StringView text, usize offset)
        {
            usize start = 0;
            return ClassifyForWords(CodepointBefore(text, offset, start));
        }
    }

    /// Returns where word movement to the right from `offset` stops: past the rest of a word and
    /// the gap after it (the next word's start), or at a line break if one comes first.
    [[nodiscard]] inline usize NextWordBoundary(core::StringView text, usize offset)
    {
        usize at = core::Min(offset, text.Size());
        if (at < text.Size() && detail::ClassAt(text, at) == WordClass::LineBreak)
            return NextGrapheme(text, at); // at a break, step over the break itself
        while (at < text.Size() && detail::ClassAt(text, at) == WordClass::Word)
            at = NextGrapheme(text, at);
        while (at < text.Size() && detail::ClassAt(text, at) != WordClass::Word &&
               detail::ClassAt(text, at) != WordClass::LineBreak)
            at = NextGrapheme(text, at);
        return at;
    }
    /// Returns where word movement to the left from `offset` stops: back over the gap and then the
    /// word (the word's start), or just after a line break if one comes first.
    [[nodiscard]] inline usize PrevWordBoundary(core::StringView text, usize offset)
    {
        usize at = core::Min(offset, text.Size());
        if (at > 0 && detail::ClassBefore(text, at) == WordClass::LineBreak)
            return PrevGrapheme(text, at);
        while (at > 0 && detail::ClassBefore(text, at) != WordClass::Word &&
               detail::ClassBefore(text, at) != WordClass::LineBreak)
            at = PrevGrapheme(text, at);
        while (at > 0 && detail::ClassBefore(text, at) == WordClass::Word)
            at = PrevGrapheme(text, at);
        return at;
    }

    /// A range of bytes.
    struct TextRange
    {
        usize Start = 0;
        usize End = 0;
        [[nodiscard]] bool IsEmpty() const noexcept { return Start == End; }
        [[nodiscard]] bool operator==(const TextRange&) const noexcept = default;
    };

    /// Returns the range a double click selects at `offset`: the word, the spaces or the
    /// punctuation there. The character after the offset decides which; at the end of a line, the
    /// character before it decides.
    [[nodiscard]] inline TextRange WordRangeAt(core::StringView text, usize offset)
    {
        offset = core::Min(offset, text.Size());
        if (text.Size() == 0)
            return {};
        WordClass kind;
        if (offset < text.Size() && detail::ClassAt(text, offset) != WordClass::LineBreak)
            kind = detail::ClassAt(text, offset);
        else if (offset > 0 && detail::ClassBefore(text, offset) != WordClass::LineBreak)
            kind = detail::ClassBefore(text, offset);
        else
            return TextRange{offset, offset};
        usize start = offset;
        while (start > 0 && detail::ClassBefore(text, start) == kind)
            start = PrevGrapheme(text, start);
        usize end = offset;
        while (end < text.Size() && detail::ClassAt(text, end) == kind)
            end = NextGrapheme(text, end);
        return TextRange{start, end};
    }

    /// Returns the logical line (between line breaks) that holds `offset`, without its line break.
    [[nodiscard]] inline TextRange LineRangeAt(core::StringView text, usize offset)
    {
        offset = core::Min(offset, text.Size());
        usize start = offset;
        while (start > 0 && text[start - 1] != u8'\n')
            --start;
        usize end = offset;
        while (end < text.Size() && text[end] != u8'\n')
            ++end;
        if (end > start && text[end - 1] == u8'\r')
            --end;
        return TextRange{start, end};
    }
}
