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
                hb_font_t* hbFont = FontFor(font);
                if (hbFont == nullptr)
                {
                    return m_fallback.ShapeText(font, text, startX, startY, outPositions);
                }
                outPositions.Clear();
                Array<u32> codepoints;
                Decode(text, codepoints);
                return ShapeCodepoints(font, hbFont, codepoints, 0, startX, startY, outPositions);
            }

            [[nodiscard]] Status ShapeTextWrapped(IFont& font, StringView text, f32 maxWidth,
                                                  Array<GlyphPosition>& outPositions, f32& outTotalHeight) override
            {
                hb_font_t* hbFont = FontFor(font);
                if (hbFont == nullptr)
                {
                    return m_fallback.ShapeTextWrapped(font, text, maxWidth, outPositions, outTotalHeight);
                }
                outPositions.Clear();
                outTotalHeight = 0.0f;
                const f32 lineHeight = font.Metrics().lineHeight;
                Array<u32> codepoints;
                Decode(text, codepoints);

                f32 y = 0.0f;
                usize start = 0;
                Array<GlyphPosition> paragraph;
                while (start <= codepoints.Size())
                {
                    // One paragraph: up to the next line break (which draws nothing).
                    usize end = start;
                    while (end < codepoints.Size() && codepoints[end] != U'\n')
                        ++end;
                    Array<u32> run;
                    for (usize i = start; i < end; ++i)
                    {
                        if (codepoints[i] != U'\r')
                            run.PushBack(codepoints[i]);
                        else
                            run.PushBack(U' '); // keep the character count; a CR draws as a space
                    }
                    paragraph.Clear();
                    (void)ShapeCodepoints(font, hbFont, run, static_cast<i32>(start), 0.0f, 0.0f, paragraph);
                    y = WrapParagraph(paragraph, codepoints, maxWidth, lineHeight, y, outPositions);
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

        private:
            // Shape `codepoints` (the characters from `clusterBase` on) and append their glyphs at
            // (startX, startY); the advance comes back.
            Result<f32> ShapeCodepoints(IFont& font, hb_font_t* hbFont, const Array<u32>& codepoints,
                                        i32 clusterBase, f32 startX, f32 startY, Array<GlyphPosition>& out)
            {
                if (codepoints.IsEmpty())
                    return 0.0f;
                hb_buffer_t* buffer = hb_buffer_create();
                hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
                hb_buffer_add_utf32(buffer, codepoints.Data(), static_cast<int>(codepoints.Size()), 0,
                                    static_cast<int>(codepoints.Size()));
                hb_buffer_guess_segment_properties(buffer);
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
                    out.PushBack(glyph);
                    x += glyph.advance;
                }
                hb_buffer_destroy(buffer);
                return x - startX;
            }

            // Lay one shaped paragraph into lines no wider than `maxWidth`, breaking after the
            // last space that fits (or before an overflowing glyph when a word alone is too wide).
            // Returns the y of its last line.
            static f32 WrapParagraph(Array<GlyphPosition>& glyphs, const Array<u32>& text, f32 maxWidth,
                                     f32 lineHeight, f32 y, Array<GlyphPosition>& out)
            {
                usize lineStart = 0;
                while (lineStart < glyphs.Size())
                {
                    const f32 origin = glyphs[lineStart].x;
                    usize lastBreak = glyphs.Size(); // the glyph after which the line may end
                    usize end = lineStart;
                    for (; end < glyphs.Size(); ++end)
                    {
                        const GlyphPosition& g = glyphs[end];
                        const bool space = g.cluster >= 0 && static_cast<usize>(g.cluster) < text.Size() &&
                                           text[static_cast<usize>(g.cluster)] == U' ';
                        if (g.x + g.advance - origin > maxWidth && end > lineStart && !space)
                            break;
                        if (space)
                            lastBreak = end;
                    }
                    usize next = end;
                    if (end < glyphs.Size() && lastBreak < glyphs.Size() && lastBreak >= lineStart)
                        next = lastBreak + 1; // wrap after the space
                    for (usize i = lineStart; i < next; ++i)
                    {
                        GlyphPosition g = glyphs[i];
                        g.x -= origin;
                        g.y = y;
                        out.PushBack(g);
                    }
                    lineStart = next;
                    if (lineStart < glyphs.Size())
                        y += lineHeight;
                }
                return y;
            }

            static void Decode(StringView text, Array<u32>& out)
            {
                out.Clear();
                usize i = 0;
                while (i < text.Size())
                    out.PushBack(DecodeCodepoint(text, i));
            }

            // The HarfBuzz font for `font`, made once per font file (a shaper serves one font);
            // null when the font has no file to read.
            hb_font_t* FontFor(const IFont& font)
            {
                const Span<const u8> data = font.SourceData();
                if (data.IsEmpty())
                    return nullptr;
                if (m_hbFont != nullptr && m_data == data.Data() && m_size == data.Size())
                    return m_hbFont;
                Release();
                hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(data.Data()),
                                                 static_cast<unsigned int>(data.Size()), HB_MEMORY_MODE_READONLY,
                                                 nullptr, nullptr);
                hb_face_t* face = hb_face_create(blob, 0);
                hb_blob_destroy(blob); // the face holds it
                m_hbFont = hb_font_create(face); // scale = units per em: positions in font units
                hb_face_destroy(face);           // the font holds it
                m_data = data.Data();
                m_size = data.Size();
                return m_hbFont;
            }

            void Release()
            {
                if (m_hbFont != nullptr)
                    hb_font_destroy(m_hbFont);
                m_hbFont = nullptr;
                m_data = nullptr;
                m_size = 0;
            }

            TrueTypeTextShaper m_fallback;
            hb_font_t* m_hbFont = nullptr;
            const u8* m_data = nullptr; // identity of the file m_hbFont reads (never dereferenced)
            usize m_size = 0;
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
