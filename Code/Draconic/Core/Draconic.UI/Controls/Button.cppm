// Draconic UI - :button partition
//
// Text button (the most common button type). Ported from Sedulous.UI/src/Controls/Button.bf. Text
// measuring/drawing is LIVE now that the Fonts service is wired (font-size height fallback for the
// no-service path); the button label is centered in the padded content rect.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:button;

import draconic.foundation;
import draconic.fonts; // CachedFont, TextAlignment, VerticalAlignment
import :button_base;
import :view;
import :property;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;
import :control_state;
import :palette;

using namespace draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class Button : public ButtonBase
    {
        DRACONIC_OBJECT(Button, ButtonBase)
    public:
        Property<String> Text;
        Property<Optional<f32>> FontSize;
        Property<String> FontFamily;

        explicit Button(StringView text)
        {
            Text.SetOwner(this);
            FontSize.SetOwner(this);
            FontFamily.SetOwner(this, InvalidationKind::Visual);
            Text.SetSilent(String(text));
        }

        /// Set the button text.
        void SetText(StringView text)
        {
            Text.SetValue(String(text));
            Invalidate();
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const Thickness pad =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{12.0f, 8.0f});
            const BoxConstraints inner = constraints.Deflate(pad).Loosen();
            const f32 fontSize = FontSize.Value().HasValue()
                                     ? FontSize.Value().Value()
                                     : ResolveStyleFloat(StyleProperty::FontSize, 16.0f);

            f32 textW = 0, textH = 0;
            const StringView text = Text.Value();
            if (text.Size() > 0 && Context != nullptr && Context->FontService() != nullptr)
            {
                if (fonts::CachedFont* font = Context->FontService()->GetFont(
                        ResolveStyleFontFamily(FontFamily.Value()), fontSize))
                {
                    textW = font->font->MeasureString(text);
                    textH = font->font->Metrics().lineHeight;
                }
            }
            else
            {
                textH = fontSize;
            }

            MeasuredSize = Float2{
                constraints.ConstrainWidth(Min(textW, inner.MaxWidth) + pad.TotalHorizontal()),
                constraints.ConstrainHeight(Min(textH, inner.MaxHeight) + pad.TotalVertical())};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const ControlState state = GetControlState();
            DrawButtonBackground(ctx, bounds, state);

            const StringView text = Text.Value();
            if (text.Size() > 0 && ctx.FontService() != nullptr)
            {
                const f32 fontSize = FontSize.Value().HasValue()
                                         ? FontSize.Value().Value()
                                         : ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
                if (fonts::CachedFont* font = ctx.FontService()->GetFont(
                        ResolveStyleFontFamily(FontFamily.Value()), fontSize))
                {
                    const Thickness pad =
                        ResolveStyleThickness(StyleProperty::Padding, Thickness{12.0f, 8.0f});
                    Color textColor = ResolveStyleColor(
                        StyleProperty::TextColor,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    if (HasFlag(state, ControlState::Disabled))
                    {
                        textColor = Palette::ComputeDisabled(textColor);
                    }
                    const Rectangle textRect{pad.Left, pad.Top, Width() - pad.TotalHorizontal(),
                                             Height() - pad.TotalVertical()};
                    // Ellipsize when the label doesn't fit (e.g. a button shrunk by its layout).
                    const String shown = fonts::TruncateToWidth(*font->font, text, textRect.width);
                    ctx.VG().DrawText(shown.AsView(), font, textRect, fonts::TextAlignment::Center,
                                      fonts::VerticalAlignment::Middle, textColor);
                }
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(Button, "draconic::ui")
}
