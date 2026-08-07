// Draconic::Fonts - :text_util partition
//
// Codepoint iteration over a UTF-8 StringView. Mirrors how Sedulous walked text
// with Beef's `StringView.DecodedChars` (Beef strings are UTF-8); Draconic's
// String is UTF-8 too, so font measuring/shaping decodes UTF-8 sequences here.
// Shared by the baked font and the TTF text shaper.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts:text_util;

import draconic.foundation;
import :interfaces; // IFont (MeasureString) for TruncateToWidth

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Decodes the Unicode codepoint starting at `text[index]`, advancing
    // `index` past the consumed byte(s). A malformed/truncated sequence decodes
    // to U+FFFD (consuming one byte). Returns the codepoint. Caller guarantees
    // `index < text.Size()`.
    [[nodiscard]] inline u32 DecodeCodepoint(StringView text, usize& index) noexcept
    {
        const u8 lead = static_cast<u8>(text[index]);
        ++index;

        u32 codepoint;
        int extra;
        if (lead < 0x80u)
        {
            return lead;
        }
        else if ((lead & 0xE0u) == 0xC0u)
        {
            codepoint = lead & 0x1Fu;
            extra = 1;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            codepoint = lead & 0x0Fu;
            extra = 2;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            codepoint = lead & 0x07u;
            extra = 3;
        }
        else
        {
            return 0xFFFDu;
        } // invalid lead byte

        for (int k = 0; k < extra; ++k)
        {
            if (index >= text.Size())
            {
                return 0xFFFDu;
            }
            const u8 cont = static_cast<u8>(text[index]);
            if ((cont & 0xC0u) != 0x80u)
            {
                return 0xFFFDu;
            } // not a continuation byte
            codepoint = (codepoint << 6) | (cont & 0x3Fu);
            ++index;
        }
        return codepoint;
    }

    // Returns `text` if it fits within `maxWidth` (measured by `font`), otherwise the longest
    // codepoint-aligned prefix that fits once `ellipsis` is appended, plus the ellipsis. Measuring the
    // growing prefix (rather than summing per-glyph advances) keeps kerning honest. Used for label /
    // button / tile text that must not overflow its box.
    [[nodiscard]] inline String TruncateToWidth(const IFont& font, StringView text, f32 maxWidth,
                                                StringView ellipsis = u8"...")
    {
        const f32 textW = font.MeasureString(text);
        // 1px tolerance: a control sized to exactly fit its text can measure a sub-pixel short after
        // layout rounding. Without this, a snug button collapses to "..." (e.g. "OK" -> "...").
        if (text.Size() == 0 || textW <= maxWidth + 1.0f)
        {
            return String(text);
        }
        const f32 ellipsisW = font.MeasureString(ellipsis);
        // If the text is already no wider than the ellipsis, replacing it with "..." can't make it
        // narrower (and usually makes it WIDER - e.g. a "+" / "x" button) - leave it unchanged.
        if (textW <= ellipsisW)
        {
            return String(text);
        }
        const f32 availW = maxWidth - ellipsisW;

        usize fitBytes = 0;
        usize i = 0;
        while (availW > 0.0f && i < text.Size()) // ellipsis alone doesn't fit -> just draw "..."
        {
            usize probe = i;
            (void)DecodeCodepoint(text,
                                  probe); // advance past one codepoint (only the index matters)
            if (font.MeasureString(StringView{text.Data(), probe}) > availW)
            {
                break;
            }
            fitBytes = probe;
            i = probe;
        }

        String out;
        out.Append(text.Data(), fitBytes);
        out.Append(ellipsis);
        return out;
    }
}
