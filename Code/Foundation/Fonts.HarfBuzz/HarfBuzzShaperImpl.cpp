// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.HarfBuzz - the HarfBuzz shaper (implementation unit; HarfBuzz stays here).
//
// Shapes UTF-32 codepoints, so HarfBuzz's clusters come back in characters, the unit GlyphPosition
// clusters count, with the monotone-characters cluster level (a ligature is one cluster over its
// characters, a base and its marks share one). Positions arrive in font units and are scaled to
// pixels by the font's Metrics().scale; y offsets flip to y-down. Wrapping shapes each paragraph
// whole and breaks it after spaces, so a ligature or a joined form never splits across a break it
// would not have split at.

module;
#include "Core/Prelude.h"
#include <hb.h>
#include <hb-ot.h>

module foundation.fonts.harfbuzz;

import foundation.core;
import foundation.fonts;
import foundation.fonts.truetype; // the basic shaper, for fonts without their file

using namespace foundation::core;

namespace foundation::fonts
{
    namespace
    {
        class HarfBuzzTextShaper final : public ClusterTextShaper
        {
        public:
            HarfBuzzTextShaper() = default;
            ~HarfBuzzTextShaper() override { Release(); }
            HarfBuzzTextShaper(const HarfBuzzTextShaper&) = delete;
            HarfBuzzTextShaper& operator=(const HarfBuzzTextShaper&) = delete;

            [[nodiscard]] Result<f32> ShapeText(IFont& font, StringView text,
                                                Array<GlyphPosition>& outPositions) override
            {
                return ShapeText(font, text, 0.0f, 0.0f, outPositions);
            }

            [[nodiscard]] Result<f32> ShapeText(IFont& font, StringView text, f32 startX, f32 startY,
                                                Array<GlyphPosition>& outPositions) override
            {
                IFont* const fonts[1] = {&font};
                ShapeRequest request;
                request.text = text;
                request.fonts = Span<IFont* const>(fonts, 1);
                request.startX = startX;
                request.startY = startY;
                return Shape(request, outPositions);
            }

            // Each paragraph (up to a line break, which draws nothing): its levels resolved, its
            // characters measured in logical order, lines broken after the last space that fits (or
            // before the character that overflows when a word alone is too wide), and each line laid
            // out in visual order, so a ligature is never split and right-to-left text reorders by line.
            [[nodiscard]] Status ShapeTextWrapped(IFont& font, StringView text, f32 maxWidth,
                                                  Array<GlyphPosition>& outPositions, f32& outTotalHeight) override
            {
                if (FontFor(font) == nullptr)
                {
                    return m_fallback.ShapeTextWrapped(font, text, maxWidth, outPositions, outTotalHeight);
                }
                outPositions.Clear();
                outTotalHeight = 0.0f;
                const f32 lineHeight = font.Metrics().lineHeight;
                Array<u32> codepoints;
                Array<usize> bytes;
                DecodeWithOffsets(text, codepoints, bytes);
                IFont* const fonts[1] = {&font};
                ShapeRequest request;
                request.text = text;
                request.fonts = Span<IFont* const>(fonts, 1);

                f32 y = 0.0f;
                usize start = 0;
                Array<GlyphPosition> measured;
                while (start <= codepoints.Size())
                {
                    usize end = start;
                    while (end < codepoints.Size() && codepoints[end] != U'\n')
                        ++end;
                    if (end > start)
                    {
                        BidiLevels levels;
                        const bool resolved = ResolveLevels(TextDirection::Auto, codepoints, start, end, levels);
                        // Each character's share of the advance, shaped in logical runs of one level.
                        Array<f32> widths;
                        widths.Resize(end - start);
                        for (f32& w : widths)
                            w = 0.0f;
                        usize runStart = start;
                        while (runStart < end)
                        {
                            usize runEnd = end;
                            RunDirection direction = RunDirection::Auto;
                            if (resolved)
                            {
                                runEnd = runStart + 1;
                                while (runEnd < end && levels.levels[runEnd - start] == levels.levels[runStart - start])
                                    ++runEnd;
                                direction = (levels.levels[runStart - start] & 1u) != 0 ? RunDirection::RightToLeft
                                                                                         : RunDirection::LeftToRight;
                            }
                            measured.Clear();
                            (void)ShapeRun(request, codepoints, bytes, runStart, runEnd, direction, 0.0f, 0.0f, measured);
                            for (const GlyphPosition& g : measured)
                                if (g.cluster >= static_cast<i32>(start) && static_cast<usize>(g.cluster) < end)
                                    widths[static_cast<usize>(g.cluster) - start] += g.advance;
                            runStart = runEnd;
                        }
                        usize lineStart = start;
                        while (lineStart < end)
                        {
                            f32 width = 0.0f;
                            usize lastSpace = end; // none yet
                            usize e = lineStart;
                            for (; e < end; ++e)
                            {
                                const bool space = codepoints[e] == U' ';
                                if (width + widths[e - start] > maxWidth && e > lineStart && !space)
                                    break;
                                if (space)
                                    lastSpace = e;
                                width += widths[e - start];
                            }
                            usize next = e;
                            if (e < end && lastSpace < end && lastSpace >= lineStart)
                                next = lastSpace + 1;
                            (void)ShapeLine(request, codepoints, bytes, lineStart, next, resolved ? &levels : nullptr, 0.0f, y,
                                            outPositions, start);
                            lineStart = next;
                            if (lineStart < end)
                                y += lineHeight;
                        }
                    }
                    if (end >= codepoints.Size())
                        break;
                    y += lineHeight; // the break starts the next paragraph on a line of its own
                    start = end + 1;
                }
                outTotalHeight = y + lineHeight;
                return Status();
            }

            // The GSUB closure of the codepoints' glyphs under the features shaping turns on by
            // default (ligatures, contextual and positional forms, the Indic and Arabic ones), less
            // the glyphs the codepoints map to themselves.
            void CollectReachableGlyphs(const IFont& font, Span<const i32> codepoints, Array<i32>& outGlyphs) override
            {
                outGlyphs.Clear();
                hb_font_t* hbFont = FontFor(font);
                if (hbFont == nullptr)
                    return;
                hb_face_t* face = hb_font_get_face(hbFont);
                hb_set_t* nominal = hb_set_create();
                for (const i32 cp : codepoints)
                {
                    hb_codepoint_t glyph = 0;
                    if (cp > 0 && hb_font_get_nominal_glyph(hbFont, static_cast<hb_codepoint_t>(cp), &glyph))
                        hb_set_add(nominal, glyph);
                }
                static const hb_tag_t kFeatures[] = {
                    HB_TAG('c', 'c', 'm', 'p'), HB_TAG('l', 'o', 'c', 'l'), HB_TAG('r', 'l', 'i', 'g'),
                    HB_TAG('l', 'i', 'g', 'a'), HB_TAG('c', 'l', 'i', 'g'), HB_TAG('c', 'a', 'l', 't'),
                    HB_TAG('r', 'c', 'l', 't'), HB_TAG('i', 's', 'o', 'l'), HB_TAG('i', 'n', 'i', 't'),
                    HB_TAG('m', 'e', 'd', 'i'), HB_TAG('m', 'e', 'd', '2'), HB_TAG('f', 'i', 'n', 'a'),
                    HB_TAG('f', 'i', 'n', '2'), HB_TAG('f', 'i', 'n', '3'), HB_TAG('n', 'u', 'k', 't'),
                    HB_TAG('a', 'k', 'h', 'n'), HB_TAG('r', 'p', 'h', 'f'), HB_TAG('r', 'k', 'r', 'f'),
                    HB_TAG('p', 'r', 'e', 'f'), HB_TAG('b', 'l', 'w', 'f'), HB_TAG('a', 'b', 'v', 'f'),
                    HB_TAG('h', 'a', 'l', 'f'), HB_TAG('p', 's', 't', 'f'), HB_TAG('v', 'a', 't', 'u'),
                    HB_TAG('c', 'j', 'c', 't'), HB_TAG('p', 'r', 'e', 's'), HB_TAG('a', 'b', 'v', 's'),
                    HB_TAG('b', 'l', 'w', 's'), HB_TAG('p', 's', 't', 's'), HB_TAG('h', 'a', 'l', 'n'),
                    HB_TAG('l', 'j', 'm', 'o'), HB_TAG('v', 'j', 'm', 'o'), HB_TAG('t', 'j', 'm', 'o'),
                    HB_TAG_NONE};
                hb_set_t* lookups = hb_set_create();
                hb_ot_layout_collect_lookups(face, HB_OT_TAG_GSUB, nullptr, nullptr, kFeatures, lookups);
                hb_set_t* glyphs = hb_set_create();
                hb_set_union(glyphs, nominal);
                hb_ot_layout_lookups_substitute_closure(face, lookups, glyphs);
                hb_set_subtract(glyphs, nominal);
                hb_codepoint_t glyph = HB_SET_VALUE_INVALID;
                while (hb_set_next(glyphs, &glyph))
                    outGlyphs.PushBack(static_cast<i32>(glyph));
                hb_set_destroy(glyphs);
                hb_set_destroy(lookups);
                hb_set_destroy(nominal);
            }

        protected:
            // Characters [start, end) as runs by font, then by script; each shaped in the run's
            // direction (or its script's), with the request's script and language. In a
            // right-to-left run the pieces are laid right to left. A font without its file is shaped
            // by the basic shaper.
            Result<f32> ShapeRun(const ShapeRequest& request, const Array<u32>& codepoints, const Array<usize>& bytes,
                                 usize start, usize end, RunDirection direction, f32 x, f32 y,
                                 Array<GlyphPosition>& out) override
            {
                const hb_language_t language =
                    request.language.IsEmpty()
                        ? HB_LANGUAGE_INVALID
                        : hb_language_from_string(reinterpret_cast<const char*>(request.language.Data()),
                                                  static_cast<int>(request.language.Size()));
                // The pieces, in logical order.
                struct Piece
                {
                    usize start = 0, end = 0;
                    u8 font = 0;
                    hb_script_t script = HB_SCRIPT_INVALID;
                };
                Array<Piece> pieces;
                Array<u32> sub;
                Array<usize> subBytes;
                for (usize i = start; i < end; ++i)
                {
                    sub.PushBack(codepoints[i]);
                    subBytes.PushBack(bytes[i]);
                }
                subBytes.PushBack(bytes[end]);
                Array<FontRun> runs;
                ItemizeByFont(request.fonts, sub, subBytes, runs);
                for (const FontRun& r : runs)
                {
                    usize at = start + r.start;
                    const usize runEnd = start + r.end;
                    while (at < runEnd)
                    {
                        Piece piece;
                        piece.start = at;
                        piece.font = r.font;
                        if (request.script != 0)
                        {
                            piece.end = runEnd;
                            piece.script = hb_script_from_iso15924_tag(request.script);
                        }
                        else
                        {
                            piece.end = ScriptRunEnd(codepoints, at, runEnd, piece.script);
                        }
                        pieces.PushBack(piece);
                        at = piece.end;
                    }
                }
                const bool reversed = direction == RunDirection::RightToLeft;
                const f32 startX = x;
                Array<GlyphPosition> fallback;
                for (usize k = 0; k < pieces.Size(); ++k)
                {
                    const Piece& piece = pieces[reversed ? pieces.Size() - 1 - k : k];
                    IFont* font = request.fonts[piece.font];
                    if (font == nullptr)
                        continue;
                    const usize first = out.Size();
                    hb_font_t* hbFont = FontFor(*font);
                    if (hbFont == nullptr)
                    {
                        const Result<f32> advance = m_fallback.ShapeText(
                            *font, request.text.SubStr(bytes[piece.start], bytes[piece.end] - bytes[piece.start]), x, y,
                            fallback);
                        if (advance.HasValue())
                        {
                            for (GlyphPosition g : fallback)
                            {
                                g.cluster += static_cast<i32>(piece.start);
                                out.PushBack(g);
                            }
                            x += advance.Value();
                        }
                    }
                    else
                    {
                        Array<u32> run;
                        for (usize i = piece.start; i < piece.end; ++i)
                            run.PushBack(codepoints[i]);
                        const hb_direction_t hbDirection = direction == RunDirection::LeftToRight    ? HB_DIRECTION_LTR
                                                           : direction == RunDirection::RightToLeft ? HB_DIRECTION_RTL
                                                                                                    : HB_DIRECTION_INVALID;
                        const Result<f32> advance = ShapeCodepoints(*font, hbFont, run, static_cast<i32>(piece.start), x, y,
                                                                    out, piece.script, hbDirection, language);
                        if (advance.HasValue())
                            x += advance.Value();
                    }
                    for (usize i = first; i < out.Size(); ++i)
                    {
                        out[i].font = piece.font;
                        if (direction == RunDirection::RightToLeft)
                            out[i].rightToLeft = true;
                    }
                }
                return x - startX;
            }

        private:
            // Shape `codepoints` (the characters from `clusterBase` on) and append their glyphs at
            // (startX, startY); the advance comes back.
            // Script, direction and language left invalid are guessed from the characters.
            Result<f32> ShapeCodepoints(IFont& font, hb_font_t* hbFont, const Array<u32>& codepoints,
                                        i32 clusterBase, f32 startX, f32 startY, Array<GlyphPosition>& out,
                                        hb_script_t script, hb_direction_t direction, hb_language_t language)
            {
                if (codepoints.IsEmpty())
                    return 0.0f;
                hb_buffer_t* buffer = hb_buffer_create();
                hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
                hb_buffer_add_utf32(buffer, codepoints.Data(), static_cast<int>(codepoints.Size()), 0,
                                    static_cast<int>(codepoints.Size()));
                if (script != HB_SCRIPT_INVALID)
                    hb_buffer_set_script(buffer, script);
                if (direction == HB_DIRECTION_INVALID && script != HB_SCRIPT_INVALID)
                    direction = hb_script_get_horizontal_direction(script);
                if (direction != HB_DIRECTION_INVALID)
                    hb_buffer_set_direction(buffer, direction);
                if (language != HB_LANGUAGE_INVALID)
                    hb_buffer_set_language(buffer, language);
                hb_buffer_guess_segment_properties(buffer); // fills only what is still unset
                hb_shape(hbFont, buffer, nullptr, 0);

                unsigned int count = 0;
                const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
                const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);

                // A cluster runs to the next cluster that starts after it (in characters).
                Array<u32> starts;
                for (unsigned int i = 0; i < count; ++i)
                    starts.PushBack(infos[i].cluster);
                for (usize i = 1; i < starts.Size(); ++i) // insertion sort; a run's glyphs are few
                    for (usize j = i; j > 0 && starts[j - 1] > starts[j]; --j)
                        Swap(starts[j - 1], starts[j]);
                const auto clusterEnd = [&](u32 cluster) -> u32
                {
                    for (const u32 s : starts)
                        if (s > cluster)
                            return s;
                    return static_cast<u32>(codepoints.Size());
                };

                const f32 scale = font.Metrics().scale;
                const bool rightToLeft = hb_buffer_get_direction(buffer) == HB_DIRECTION_RTL;
                f32 x = startX;
                for (unsigned int i = 0; i < count; ++i)
                {
                    GlyphPosition glyph;
                    glyph.cluster = clusterBase + static_cast<i32>(infos[i].cluster);
                    glyph.clusterLength = static_cast<i32>(clusterEnd(infos[i].cluster) - infos[i].cluster);
                    glyph.glyphId = static_cast<i32>(infos[i].codepoint);
                    glyph.x = x;
                    glyph.y = startY;
                    glyph.advance = static_cast<f32>(positions[i].x_advance) * scale;
                    glyph.xOffset = static_cast<f32>(positions[i].x_offset) * scale;
                    glyph.yOffset = -static_cast<f32>(positions[i].y_offset) * scale; // y-up -> y-down
                    glyph.glyphInfo.codepoint = static_cast<i32>(codepoints[infos[i].cluster]);
                    glyph.glyphInfo.glyphIndex = glyph.glyphId;
                    glyph.glyphInfo.advanceWidth = glyph.advance;
                    glyph.rightToLeft = rightToLeft;
                    out.PushBack(glyph);
                    x += glyph.advance;
                }
                hb_buffer_destroy(buffer);
                return x - startX;
            }

            // The end of the script run starting at `start` (within [start, end)), and its script. A
            // character of no particular script (common, inherited) belongs to the run it is in.
            static usize ScriptRunEnd(const Array<u32>& codepoints, usize start, usize end, hb_script_t& outScript)
            {
                hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
                outScript = HB_SCRIPT_INVALID;
                for (usize i = start; i < end; ++i)
                {
                    const hb_script_t script = hb_unicode_script(unicode, codepoints[i]);
                    if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN)
                        continue;
                    if (outScript == HB_SCRIPT_INVALID)
                        outScript = script;
                    else if (script != outScript)
                    {
                        return i; // the new script starts a run (spaces before it stay behind)
                    }
                }
                return end;
            }

            hb_font_t* FontFor(const IFont& font)
            {
                const Span<const u8> data = font.SourceData();
                if (data.IsEmpty())
                    return nullptr;
                for (const CachedHbFont& cached : m_fonts)
                    if (cached.data == data.Data() && cached.size == data.Size())
                        return cached.font;
                hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(data.Data()),
                                                 static_cast<unsigned int>(data.Size()), HB_MEMORY_MODE_READONLY,
                                                 nullptr, nullptr);
                hb_face_t* face = hb_face_create(blob, 0);
                hb_blob_destroy(blob); // the face holds it
                CachedHbFont cached;
                cached.font = hb_font_create(face); // scale = units per em: positions in font units
                hb_face_destroy(face);              // the font holds it
                cached.data = data.Data();
                cached.size = data.Size();
                m_fonts.PushBack(cached);
                return cached.font;
            }

            void Release()
            {
                for (const CachedHbFont& cached : m_fonts)
                    hb_font_destroy(cached.font);
                m_fonts.Clear();
            }

            // A HarfBuzz font per font file this shaper has shaped with (its own font, and any a
            // request falls back to).
            struct CachedHbFont
            {
                hb_font_t* font = nullptr;
                const u8* data = nullptr; // identity of the file it reads (never dereferenced)
                usize size = 0;
            };

            TrueTypeTextShaper m_fallback;
            Array<CachedHbFont> m_fonts;
        };

        bool& InstalledSlot()
        {
            static bool installed = false;
            return installed;
        }
    }

    void HarfBuzzShaping::Install()
    {
        TextShaperFactory::Register(&HarfBuzzShaping::CreateShaper);
        InstalledSlot() = true;
    }

    void HarfBuzzShaping::Uninstall()
    {
        TextShaperFactory::Unregister(&HarfBuzzShaping::CreateShaper);
        InstalledSlot() = false;
    }

    bool HarfBuzzShaping::IsInstalled() { return InstalledSlot(); }

    ITextShaper* HarfBuzzShaping::CreateShaper(IAllocator& allocator) { return allocator.New<HarfBuzzTextShaper>(); }
}
