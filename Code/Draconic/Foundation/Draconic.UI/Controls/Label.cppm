// Draconic UI - :label partition
//
// Text display view with alignment, word wrap, and ellipsis. Ported from Sedulous.UI/src/Controls/
// Label.bf. Now that the Fonts service is wired into UIContext (Context->FontService()) and VG has
// text drawing, measuring/drawing are LIVE (font-size fallbacks remain for the no-service path).
// Uses fonts::TextAlignment / VerticalAlignment. Beef String.Split('\n') -> a local line splitter
// (StringView slices into the stable Text string); String.RawChars ellipsis loop -> byte scan.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:label;

import draconic.foundation;
import draconic.fonts; // CachedFont, TextAlignment, VerticalAlignment, GlyphPosition
import :view;
import :property;
import :box_constraints;
import :style_property;
import :draw_context;
import :palette;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class Label : public View
    {
        DRACONIC_OBJECT(Label, View)
    public:
        Property<String> Text;
        Property<fonts::TextAlignment> HAlign{fonts::TextAlignment::Left};
        Property<fonts::VerticalAlignment> VAlign{fonts::VerticalAlignment::Middle};
        Property<bool> WordWrap{false};
        Property<bool> Ellipsis{false};
        Property<Optional<f32>> FontSize;
        Property<String> FontFamily;
        Property<Optional<foundation::Color>> TextColor;

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
                    if (WordWrap.Value() && font->shaper != nullptr)
                    {
                        const f32 maxWidth =
                            (constraints.MaxWidth < kFloatMax) ? constraints.MaxWidth : 10000.0f;
                        textW = maxWidth;
                        Array<fonts::GlyphPosition> positions;
                        f32 totalH = 0;
                        if (font->shaper
                                ->ShapeTextWrapped(*font->font, text, maxWidth, positions, totalH)
                                .IsOk())
                        {
                            textH = totalH;
                        }
                    }
                    else if (HasNewlines(text))
                    {
                        const f32 lineHeight = font->font->Metrics().lineHeight;
                        f32 maxW = 0;
                        i32 lineCount = 0;
                        Array<StringView> lines;
                        SplitLines(text, lines);
                        for (const StringView& line : lines)
                        {
                            maxW = Max(maxW, font->font->MeasureString(line));
                            lineCount++;
                        }
                        textW = maxW;
                        textH = lineHeight * static_cast<f32>(lineCount);
                    }
                    else
                    {
                        textW = font->font->MeasureString(text);
                        textH = font->font->Metrics().lineHeight;
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

            Color textColor = TextColor.Value().HasValue()
                                  ? TextColor.Value().Value()
                                  : ResolveStyleColor(StyleProperty::TextColor,
                                                      Color{220.0f / 255.0f, 225.0f / 255.0f,
                                                            235.0f / 255.0f, 1.0f});
            if (!IsEffectivelyEnabled())
            {
                textColor = Palette::ComputeDisabled(textColor);
            }

            const fonts::TextAlignment h = HAlign.Value();
            const fonts::VerticalAlignment v = VAlign.Value();

            if (WordWrap.Value())
            {
                f32 y = 0;
                if (v != fonts::VerticalAlignment::Top && font->shaper != nullptr)
                {
                    const f32 totalH = ctx.VG().MeasureTextWrapped(text, font, Width());
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
                Array<StringView> lines;
                SplitLines(text, lines);
                const f32 totalH = lineHeight * static_cast<f32>(lines.Size());
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
                for (const StringView& line : lines)
                {
                    ctx.VG().DrawText(line, font, Rectangle{0, yy, Width(), lineHeight}, h,
                                      fonts::VerticalAlignment::Top, textColor);
                    yy += lineHeight;
                }
            }
            else if (Ellipsis.Value())
            {
                const String shown = fonts::TruncateToWidth(*font->font, text, Width());
                ctx.VG().DrawText(shown.AsView(), font, Rectangle{0, 0, Width(), Height()}, h, v,
                                  textColor);
            }
            else
            {
                ctx.VG().DrawText(text, font, Rectangle{0, 0, Width(), Height()}, h, v, textColor);
            }
        }

    private:
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

    DRACONIC_DEFINE_OBJECT(Label, "draconic::ui")
}
