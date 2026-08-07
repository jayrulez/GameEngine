// Draconic::FontsTTF - draconic.fonts.ttf:font partition
//
// TrueType/OpenType IFont backed by stb_truetype. Owns the raw font bytes and
// an stbtt_fontinfo, caches per-codepoint GlyphInfo, and extracts the family
// name from the name table. Ported from Sedulous.Fonts.TTF/TrueTypeFont.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "stb_truetype.h"

export module draconic.fonts.ttf:font;

import draconic.foundation;
import draconic.fonts;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Backend tag returned by TrueTypeFont::BackendTypeId() so the TTF atlas
    // baker can recover the concrete type without RTTI.
    inline constexpr u32 kTrueTypeFontTypeId = 0x54544646u; // 'TTFF'

    class TrueTypeFont final : public IFont
    {
    public:
        TrueTypeFont() = default;
        ~TrueTypeFont() override = default;

        TrueTypeFont(const TrueTypeFont&) = delete;
        TrueTypeFont& operator=(const TrueTypeFont&) = delete;

        // Initialize from font bytes (takes ownership of fontData). Returns
        // FontLoadResult::Success or a specific failure.
        [[nodiscard]] FontLoadResult Initialize(Array<u8>&& fontData, f32 pixelHeight)
        {
            m_fontData = Move(fontData);
            m_pixelHeight = pixelHeight;

            const unsigned char* ptr = m_fontData.Data();

            // For TTC collections, use the first font.
            const int offset = stbtt_GetFontOffsetForIndex(ptr, 0);
            if (offset < 0)
                return FontLoadResult::InvalidFormat;

            if (stbtt_InitFont(&m_fontInfo, ptr, offset) == 0)
                return FontLoadResult::CorruptedData;

            m_scale = stbtt_ScaleForPixelHeight(&m_fontInfo, pixelHeight);

            int ascent = 0, descent = 0, lineGap = 0;
            stbtt_GetFontVMetrics(&m_fontInfo, &ascent, &descent, &lineGap);

            m_metrics = FontMetrics(ascent * m_scale, descent * m_scale, lineGap * m_scale,
                                    pixelHeight, m_scale);

            ExtractFamilyName(&m_fontInfo, m_familyName);
            return FontLoadResult::Success;
        }

        // Raw font bytes - the atlas baker needs these for stb's pack API.
        [[nodiscard]] const unsigned char* RawData() const { return m_fontData.Data(); }
        // Byte length of the raw font data (the MSDF baker inits stb_truetype directly from it).
        [[nodiscard]] usize RawDataSize() const { return m_fontData.Size(); }

        // --- IFont ---------------------------------------------------------
        [[nodiscard]] u32 BackendTypeId() const override { return kTrueTypeFontTypeId; }
        [[nodiscard]] StringView FamilyName() const override { return m_familyName; }
        [[nodiscard]] FontMetrics Metrics() const override { return m_metrics; }
        [[nodiscard]] f32 PixelHeight() const override { return m_pixelHeight; }

        [[nodiscard]] GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            if (const GlyphInfo* cached = m_glyphCache.Find(codepoint))
                return *cached;

            GlyphInfo info;
            info.codepoint = codepoint;
            info.glyphIndex = stbtt_FindGlyphIndex(&m_fontInfo, codepoint);

            if (info.glyphIndex > 0)
            {
                int advanceWidth = 0, leftSideBearing = 0;
                stbtt_GetGlyphHMetrics(&m_fontInfo, info.glyphIndex, &advanceWidth,
                                       &leftSideBearing);
                info.advanceWidth = advanceWidth * m_scale;
                info.leftSideBearing = leftSideBearing * m_scale;

                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                stbtt_GetGlyphBitmapBox(&m_fontInfo, info.glyphIndex, m_scale, m_scale, &x0, &y0,
                                        &x1, &y1);
                info.boundingBox = Rectangle(static_cast<f32>(x0), static_cast<f32>(y0),
                                             static_cast<f32>(x1 - x0), static_cast<f32>(y1 - y0));
                info.hasBitmap = (x1 - x0) > 0 && (y1 - y0) > 0;
            }

            m_glyphCache.InsertOrAssign(codepoint, info);
            return info;
        }

        [[nodiscard]] f32 GetKerning(i32 firstCodepoint, i32 secondCodepoint) const override
        {
            const int kern =
                stbtt_GetCodepointKernAdvance(&m_fontInfo, firstCodepoint, secondCodepoint);
            return kern * m_scale;
        }

        [[nodiscard]] bool HasGlyph(i32 codepoint) const override
        {
            return stbtt_FindGlyphIndex(&m_fontInfo, codepoint) > 0;
        }

        [[nodiscard]] f32 MeasureString(StringView text) const override
        {
            f32 width = 0;
            i32 prevCodepoint = 0;
            usize i = 0;
            while (i < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, i));
                const GlyphInfo info = GetGlyphInfo(codepoint);
                if (prevCodepoint != 0)
                    width += GetKerning(prevCodepoint, codepoint);
                width += info.advanceWidth;
                prevCodepoint = codepoint;
            }
            return width;
        }

        [[nodiscard]] f32 MeasureString(StringView text,
                                        Array<GlyphPosition>& outPositions) const override
        {
            f32 x = 0;
            i32 prevCodepoint = 0;
            i32 index = 0;
            usize i = 0;
            outPositions.Clear();
            while (i < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, i));
                const GlyphInfo info = GetGlyphInfo(codepoint);
                if (prevCodepoint != 0)
                    x += GetKerning(prevCodepoint, codepoint);
                outPositions.PushBack(
                    GlyphPosition(index, codepoint, x, 0, info.advanceWidth, info));
                x += info.advanceWidth;
                prevCodepoint = codepoint;
                ++index;
            }
            return x;
        }

    private:
        // Reads the family name (nameID 1) from the name table. Tries
        // Microsoft Unicode English, then any language, then Macintosh Roman;
        // falls back to a placeholder. MS strings are big-endian UTF-16.
        static void ExtractFamilyName(const stbtt_fontinfo* font, String& outName)
        {
            outName.Clear();
            constexpr int NAME_ID_FAMILY = 1;

            if (TryReadUtf16Name(font, 3, 1, 0x0409, NAME_ID_FAMILY, outName))
                return;
            if (TryReadUtf16NameAnyLanguage(font, 3, 1, NAME_ID_FAMILY, outName))
                return;
            if (TryReadLatin1Name(font, 1, 0, 0, NAME_ID_FAMILY, outName))
                return;

            outName = String(u8"TrueType Font");
        }

        static bool TryReadUtf16Name(const stbtt_fontinfo* font, int platformID, int encodingID,
                                     int languageID, int nameID, String& outName)
        {
            int byteLen = 0;
            const char* bytes =
                stbtt_GetFontNameString(font, &byteLen, platformID, encodingID, languageID, nameID);
            if (bytes == nullptr || byteLen <= 0)
                return false;
            AppendBigEndianUtf16(bytes, byteLen, outName);
            return !outName.IsEmpty();
        }

        static bool TryReadUtf16NameAnyLanguage(const stbtt_fontinfo* font, int platformID,
                                                int encodingID, int nameID, String& outName)
        {
            const int languages[] = {0x0809, 0x0c09, 0x1009, 0x1409, 0};
            for (const int lang : languages)
                if (TryReadUtf16Name(font, platformID, encodingID, lang, nameID, outName))
                    return true;
            return false;
        }

        static bool TryReadLatin1Name(const stbtt_fontinfo* font, int platformID, int encodingID,
                                      int languageID, int nameID, String& outName)
        {
            int byteLen = 0;
            const char* bytes =
                stbtt_GetFontNameString(font, &byteLen, platformID, encodingID, languageID, nameID);
            if (bytes == nullptr || byteLen <= 0)
                return false;
            outName.Clear();
            for (int i = 0; i < byteLen; ++i)
            {
                const u8 b = static_cast<u8>(bytes[i]);
                if (b == 0)
                    continue;
                AppendUtf8(outName, b); // Latin-1 == first 256 code points
            }
            return !outName.IsEmpty();
        }

        // Decodes big-endian UTF-16 bytes from the name table into UTF-8 String.
        static void AppendBigEndianUtf16(const char* bytes, int byteLen, String& out)
        {
            out.Clear();
            const u8* p = reinterpret_cast<const u8*>(bytes);
            int i = 0;
            while (i + 1 < byteLen)
            {
                const u16 cp = static_cast<u16>((static_cast<u16>(p[i]) << 8) | p[i + 1]);
                i += 2;
                if (cp == 0)
                    continue;
                AppendUtf8(out, cp); // BMP only; lone surrogates pass through as their unit value
            }
        }

        // Appends a (BMP) codepoint to a UTF-8 String.
        static void AppendUtf8(String& out, u32 cp)
        {
            if (cp < 0x80u)
            {
                out.PushBack(static_cast<utf8char>(cp));
            }
            else if (cp < 0x800u)
            {
                out.PushBack(static_cast<utf8char>(0xC0u | (cp >> 6)));
                out.PushBack(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
            else
            {
                out.PushBack(static_cast<utf8char>(0xE0u | (cp >> 12)));
                out.PushBack(static_cast<utf8char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.PushBack(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
        }

        String m_familyName;
        FontMetrics m_metrics;
        f32 m_pixelHeight = 0;
        Array<u8> m_fontData;
        stbtt_fontinfo m_fontInfo{};
        f32 m_scale = 0;
        mutable HashMap<i32, GlyphInfo> m_glyphCache;
    };
}
