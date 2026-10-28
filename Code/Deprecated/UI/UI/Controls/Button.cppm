// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :button partition
//
// Text button (the most common button type). Ported from Sedulous.UI/src/Controls/Button.bf. Text
// measuring/drawing is LIVE now that the Fonts service is wired (font-size height fallback for the
// no-service path); the button label is centered in the padded content rect.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:button;

import foundation.core;
import foundation.fonts; // CachedFont, TextAlignment, VerticalAlignment
import :button_base;
import :view;
import :property;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;
import :control_state;
import :palette;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui
{
    class Button : public ButtonBase
    {
        RTTI_OBJECT(Button, ButtonBase)
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

        /// Set the button text. The property invalidates on a change only: a caller that sets the
        /// same text every frame (a toolbar's state sync) must not re-lay-out the window each frame.
        void SetText(StringView text) { Text.SetValue(String(text)); }

    protected:
        [[nodiscard]] Thickness DefaultStylePadding() const override
        {
            return Thickness{12.0f, 8.0f};
        }

        // Content-only measure: the base handles chrome, so padding is not deflated and
        // re-inflated here.
        [[nodiscard]] Float2 OnMeasureContent(BoxConstraints contentConstraints) override
        {
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
            return Float2{Min(textW, contentConstraints.MaxWidth),
                          Min(textH, contentConstraints.MaxHeight)};
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
                    // Content offset = the SAME chrome the base deflated at measure (padding +
                    // border) - text stays clear of a themed border.
                    const Thickness pad = ResolveBoxMetrics().Chrome();
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

    RTTI_DEFINE_OBJECT(Button, "rtti::ui")
}
