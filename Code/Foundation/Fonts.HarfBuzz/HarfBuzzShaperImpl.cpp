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

            // Runs by font, then by script within each (a character of no particular script, a
            // space or a digit, joins the run around it), each shaped with its direction, script
            // and language; runs are laid one after the other (reordering runs is bidi's).
            [[nodiscard]] Result<f32> Shape(const ShapeRequest& request, Array<GlyphPosition>& outPositions) override
            {
                outPositions.Clear();
                Array<u32> codepoints;
                Array<usize> bytes;
                DecodeWithOffsets(request.text, codepoints, bytes);
                Array<FontRun> runs;
                ItemizeByFont(request.fonts, codepoints, bytes, runs);
                const hb_language_t language =
                    request.language.IsEmpty()
                        ? HB_LANGUAGE_INVALID
                        : hb_language_from_string(reinterpret_cast<const char*>(request.language.Data()),
                                                  static_cast<int>(request.language.Size()));
                f32 x = request.startX;
                Array<GlyphPosition> fallback;
                for (const FontRun& r : runs)
                {
                    IFont* font = request.fonts[r.font];
                    if (font == nullptr)
                        continue;
                    hb_font_t* hbFont = FontFor(*font);
                    const usize first = outPositions.Size();
                    if (hbFont == nullptr)
                    {
                        // No file to read: the basic shaper, run by run.
                        const Result<f32> advance = m_fallback.ShapeText(
                            *font, request.text.SubStr(r.byteStart, r.byteEnd - r.byteStart), x, request.startY,
                            fallback);
                        if (advance.HasValue())
                        {
                            for (GlyphPosition g : fallback)
                            {
                                g.cluster += static_cast<i32>(r.start);
                                outPositions.PushBack(g);
                            }
                            x += advance.Value();
                        }
                    }
                    else
                    {
                        usize start = r.start;
                        while (start < r.end)
                        {
                            hb_script_t script = HB_SCRIPT_INVALID;
                            const usize end = request.script != 0 ? r.end : ScriptRunEnd(codepoints, start, r.end, script);
                            if (request.script != 0)
                                script = hb_script_from_iso15924_tag(request.script);
                            Array<u32> run;
                            for (usize i = start; i < end; ++i)
                                run.PushBack(codepoints[i]);
                            hb_direction_t direction = request.direction == TextDirection::LeftToRight   ? HB_DIRECTION_LTR
                                                       : request.direction == TextDirection::RightToLeft ? HB_DIRECTION_RTL
                                                                                                         : HB_DIRECTION_INVALID;
                            const Result<f32> advance =
                                ShapeCodepoints(*font, hbFont, run, static_cast<i32>(start), x, request.startY,
                                                outPositions, script, direction, language);
                            if (advance.HasValue())
                                x += advance.Value();
                            start = end;
                        }
                    }
                    for (usize i = first; i < outPositions.Size(); ++i)
                        outPositions[i].font = r.font;
                }
                return x - request.startX;
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
                    (void)ShapeCodepoints(font, hbFont, run, static_cast<i32>(start), 0.0f, 0.0f, paragraph,
                                          HB_SCRIPT_INVALID, HB_DIRECTION_INVALID, HB_LANGUAGE_INVALID);
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
