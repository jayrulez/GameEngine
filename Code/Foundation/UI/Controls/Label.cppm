// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :label partition
//
// Text display view with alignment, word wrap, and ellipsis. Ported from Sedulous.UI/src/Controls/
// Label.bf. Now that the Fonts service is wired into UIContext (Context->FontService()) and VG has
// text drawing, measuring/drawing are LIVE (font-size fallbacks remain for the no-service path).
// Uses fonts::TextAlignment / VerticalAlignment. Beef String.Split('\n') -> a local line splitter
// (StringView slices into the stable Text string); String.RawChars ellipsis loop -> byte scan.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:label;

import foundation.core;
import foundation.fonts; // CachedFont, TextAlignment, VerticalAlignment, GlyphPosition
import :view;
import :property;
import :box_constraints;
import :style_property;
import :draw_context;
import :palette;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui
{
    class Label : public View
    {
        RTTI_OBJECT(Label, View)
    public:
        Property<String> Text;
        Property<fonts::TextAlignment> HAlign{fonts::TextAlignment::Left};
        Property<fonts::VerticalAlignment> VAlign{fonts::VerticalAlignment::Middle};
        Property<bool> WordWrap{false};
        Property<bool> Ellipsis{false};
        Property<Optional<f32>> FontSize;
        Property<String> FontFamily;
        Property<Optional<core::Color>> TextColor;

        Label()
        {
            Text.SetOwner(this);
            HAlign.SetOwner(this, InvalidationKind::Visual);
            VAlign.SetOwner(this, InvalidationKind::Visual);
            WordWrap.SetOwner(this);
            Ellipsis.SetOwner(this, InvalidationKind::Visual);
            FontSize.SetOwner(this);
            FontFamily.SetOwner(this, InvalidationKind::Visual);
            TextColor.SetOwner(this, InvalidationKind::Visual);
        }
        explicit Label(StringView text) : Label() { Text.SetSilent(String(text)); }

        /// Set text and return this for chaining.
        Label* SetText(StringView text)
        {
            Text.SetValue(String(text));
            Invalidate();
            return this;
        }

        [[nodiscard]] f32 GetBaseline() const override
        {
            if (Context != nullptr && Context->FontService() != nullptr)
            {
                // ResolveFont/ResolveStyle are logically const but not marked (style resolution is lazy).
                if (fonts::CachedFont* font = const_cast<Label*>(this)->ResolveFont())
                {
                    return font->font->Metrics().ascent;
                }
            }
            return -1.0f;
        }

        /// Single-line truncation is on: the Ellipsis property, or `text-overflow: ellipsis`
        /// from the cascade (the property wins when set).
        [[nodiscard]] bool EffectiveEllipsis()
        {
            if (Ellipsis.Value())
            {
                return true;
            }
            const String overflow = ResolveStyleString(StyleProperty::TextOverflow);
            return overflow == StringView(u8"ellipsis");
        }

        /// The colour the text draws in: the label's own TextColor when set, else the style's
        /// text-color (the one place both drawing and a script's read take it from). A disabled
        /// label dims it when it draws.
        [[nodiscard]] Color ResolvedTextColor()
        {
            return TextColor.Value().HasValue()
                       ? TextColor.Value().Value()
                       : ResolveStyleColor(StyleProperty::TextColor,
                                           Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 fontSize = ResolveFontSize();
            f32 textW = 0, textH = fontSize;

            const StringView text = Text.Value();
            if (text.Size() > 0 && Context != nullptr && Context->FontService() != nullptr)
            {
                if (fonts::CachedFont* font = ResolveFont())
                {
                    // Measure runs every frame today (no layout dirty flags yet) - shaping /
                    // per-line measuring on every pass was the UI's biggest steady-state text
                    // cost. All results are cached in m_measureCache, value-keyed (the EditText
                    // m_glyphsDirty pattern, generalized).
                    const String family = ResolveStyleFontFamily(FontFamily.Value());
                    if (WordWrap.Value() && font->shaper != nullptr)
                    {
                        const f32 maxWidth =
                            (constraints.MaxWidth < kFloatMax) ? constraints.MaxWidth : 10000.0f;
                        textW = maxWidth;
                        if (!m_measureCache.Matches(text, family, fontSize, maxWidth, true))
                        {
                            m_measureCache.Rekey(text, family, fontSize, maxWidth, true);
                            Array<fonts::GlyphPosition> positions;
                            f32 totalH = 0;
                            if (font->shaper
                                    ->ShapeTextWrapped(*font->font, text, maxWidth, positions,
                                                       totalH)
                                    .IsOk())
                            {
                                m_measureCache.textH = totalH;
                            }
                            else
                            {
                                m_measureCache.textH = fontSize;
                            }
                        }
                        textH = m_measureCache.textH;
                    }
                    else if (HasNewlines(text))
                    {
                        if (!m_measureCache.Matches(text, family, fontSize, -1.0f, false))
                        {
                            m_measureCache.Rekey(text, family, fontSize, -1.0f, false);
                            const f32 lineHeight = font->font->Metrics().lineHeight;
                            // Slices borrow the CACHE'S OWN copy - stable until rekeyed.
                            SplitLines(m_measureCache.text.AsView(), m_measureCache.lines);
                            f32 maxW = 0;
                            for (const StringView& line : m_measureCache.lines)
                            {
                                maxW = Max(maxW, font->font->MeasureString(line));
                            }
                            m_measureCache.textW = maxW;
                            m_measureCache.textH =
                                lineHeight * static_cast<f32>(m_measureCache.lines.Size());
                        }
                        textW = m_measureCache.textW;
                        textH = m_measureCache.textH;
                    }
                    else
                    {
                        if (!m_measureCache.Matches(text, family, fontSize, -1.0f, false))
                        {
                            m_measureCache.Rekey(text, family, fontSize, -1.0f, false);
                            m_measureCache.textW = font->font->MeasureString(text);
                            m_measureCache.textH = font->font->Metrics().lineHeight;
                        }
                        textW = m_measureCache.textW;
                        textH = m_measureCache.textH;
                    }
                }
            }

            MeasuredSize =
                Float2{constraints.ConstrainWidth(textW), constraints.ConstrainHeight(textH)};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const StringView text = Text.Value();
            if (text.Size() == 0 || ctx.FontService() == nullptr)
            {
                return;
            }

            fonts::CachedFont* font = ResolveFont(ctx.FontService());
            if (font == nullptr)
            {
                return;
            }

            Color textColor = ResolvedTextColor();
            if (!IsEffectivelyEnabled())
            {
                textColor = Palette::ComputeDisabled(textColor);
            }

            const fonts::TextAlignment h = HAlign.Value();
            const fonts::VerticalAlignment v = VAlign.Value();

            // Draw-side results cache against the ARRANGED width (which can differ from the
            // measure constraint) - a separate entry from m_measureCache so alternating
            // measure/draw widths never ping-pong one slot. (DrawTextWrapped still shapes
            // internally in VG; there is no shared shaped-text cache.)
            const String family = ResolveStyleFontFamily(FontFamily.Value());
            const f32 fontSize = ResolveFontSize();

            if (WordWrap.Value())
            {
                f32 y = 0;
                if (v != fonts::VerticalAlignment::Top && font->shaper != nullptr)
                {
                    if (!m_drawCache.Matches(text, family, fontSize, Width(), true))
                    {
                        m_drawCache.Rekey(text, family, fontSize, Width(), true);
                        m_drawCache.textH = ctx.VG().MeasureTextWrapped(text, font, Width());
                    }
                    const f32 totalH = m_drawCache.textH;
                    if (v == fonts::VerticalAlignment::Middle)
                    {
                        y = (Height() - totalH) * 0.5f;
                    }
                    else if (v == fonts::VerticalAlignment::Bottom)
                    {
                        y = Height() - totalH;
                    }
                }
                ctx.VG().DrawTextWrapped(text, font, Float2{0, y}, Width(), textColor, h);
            }
            else if (HasNewlines(text))
            {
                const f32 lineHeight = font->font->Metrics().lineHeight;
                if (!m_drawCache.Matches(text, family, fontSize, -1.0f, false))
                {
                    m_drawCache.Rekey(text, family, fontSize, -1.0f, false);
                    SplitLines(m_drawCache.text.AsView(), m_drawCache.lines);
                }
                const f32 totalH = lineHeight * static_cast<f32>(m_drawCache.lines.Size());
                f32 startY = 0;
                if (v == fonts::VerticalAlignment::Middle)
                {
                    startY = (Height() - totalH) * 0.5f;
                }
                else if (v == fonts::VerticalAlignment::Bottom)
                {
                    startY = Height() - totalH;
                }
                f32 yy = startY;
                for (const StringView& line : m_drawCache.lines)
                {
                    ctx.VG().DrawText(line, font, Rectangle{0, yy, Width(), lineHeight}, h,
                                      fonts::VerticalAlignment::Top, textColor);
                    yy += lineHeight;
                }
            }
            else if (EffectiveEllipsis())
            {
                if (!m_drawCache.Matches(text, family, fontSize, Width(), false))
                {
                    m_drawCache.Rekey(text, family, fontSize, Width(), false);
                    m_drawCache.ellipsized = fonts::TruncateToWidth(*font->font, text, Width());
                }
                ctx.VG().DrawText(m_drawCache.ellipsized.AsView(), font,
                                  Rectangle{0, 0, Width(), Height()}, h, v, textColor);
            }
            else
            {
                ctx.VG().DrawText(text, font, Rectangle{0, 0, Width(), Height()}, h, v, textColor);
            }
        }

    private:
        /// Value-keyed shaped-text cache (never keyed on font pointers - the freed-then-reused
        /// address rule). Holds its OWN text copy so the line slices stay stable until rekeyed.
        struct ShapedCache
        {
            String text;
            String family;
            f32 fontSize = -1.0f;
            f32 width = -1.0f; // wrap/ellipsis constraint width; -1 when width-independent
            bool wrap = false;
            f32 textW = 0.0f, textH = 0.0f;
            Array<StringView> lines; // borrow this->text
            String ellipsized;

            [[nodiscard]] bool Matches(StringView t, const String& fam, f32 size, f32 w,
                                       bool wr) const
            {
                return wrap == wr && fontSize == size && width == w && family == fam &&
                       text.AsView() == t;
            }
            void Rekey(StringView t, const String& fam, f32 size, f32 w, bool wr)
            {
                text = String(t);
                family = fam;
                fontSize = size;
                width = w;
                wrap = wr;
                textW = 0.0f;
                textH = 0.0f;
                lines.Clear();
                ellipsized.Clear();
            }
        };

        ShapedCache m_measureCache;
        ShapedCache m_drawCache;

        [[nodiscard]] f32 ResolveFontSize()
        {
            return FontSize.Value().HasValue() ? FontSize.Value().Value()
                                               : ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
        }

        [[nodiscard]] fonts::CachedFont* ResolveFont()
        {
            return ResolveFont(Context ? Context->FontService() : nullptr);
        }
        [[nodiscard]] fonts::CachedFont* ResolveFont(fonts::IFontService* service)
        {
            if (service == nullptr)
            {
                return nullptr;
            }
            const String family = ResolveStyleFontFamily(FontFamily.Value());
            return service->GetFont(family, ResolveFontSize());
        }

        [[nodiscard]] static bool HasNewlines(StringView text)
        {
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] == static_cast<utf8char>('\n'))
                {
                    return true;
                }
            }
            return false;
        }

        // Split on '\n' into borrowed slices (valid while `text`'s backing string is alive).
        static void SplitLines(StringView text, Array<StringView>& out)
        {
            usize start = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] == static_cast<utf8char>('\n'))
                {
                    out.PushBack(StringView{text.Data() + start, i - start});
                    start = i + 1;
                }
            }
            out.PushBack(StringView{text.Data() + start, text.Size() - start});
        }
    };

    RTTI_DEFINE_OBJECT(Label, "rtti::ui")
}
