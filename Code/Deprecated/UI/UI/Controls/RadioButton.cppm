// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :radio_button partition
//
// Radio button (cannot be unchecked by click; use RadioGroup for mutual exclusion). Ported from
// Sedulous.UI/src/Controls/RadioButton.bf (a View). Circle chrome (part drawable or fallback) + text
// label are LIVE now that the Fonts service + VG are wired; toggle/state/event faithful.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:radio_button;

import foundation.core;
import foundation.fonts; // CachedFont, TextAlignment, VerticalAlignment
import :view;
import :event;
import :property;
import :control_state;
import :style_property;
import :box_constraints;
import :draw_context;
import :drawable;
import :event_args;
import :input_enums;
import :enums;
import :palette;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui
{
    class RadioButton : public View
    {
        RTTI_OBJECT(RadioButton, View)
    public:
        Property<bool> IsChecked{false};
        Property<String> Text;
        Property<Optional<f32>> FontSize;
        Property<String> FontFamily;
        Property<Optional<core::Color>> TextColor;
        Event<void(RadioButton*, bool)> OnCheckedChanged;

        RadioButton() { Init(); }
        explicit RadioButton(StringView text)
        {
            Init();
            Text.SetSilent(String(text));
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Button == MouseButton::Left && !IsChecked.Value())
            {
                IsChecked.SetValue(true);
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if ((e.Key == KeyCode::Space || e.Key == KeyCode::Return) && !IsChecked.Value())
            {
                IsChecked.SetValue(true);
                e.Handled = true;
            }
        }
        void OnActivate() override
        {
            if (IsEffectivelyEnabled())
            {
                IsChecked.SetValue(true);
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
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
            const f32 circleSize = ResolvePartFloat(u8"box", StyleProperty::Width,
                                                    GetControlState(), kCircleSize);
            const f32 totalW = circleSize + ((textW > 0) ? kCircleTextSpacing + textW : 0);
            const f32 totalH = Max(circleSize, textH);
            MeasuredSize =
                Float2{constraints.ConstrainWidth(totalW), constraints.ConstrainHeight(totalH)};
        }
        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 fontSize = FontSize.Value().HasValue()
                                     ? FontSize.Value().Value()
                                     : ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
            const f32 circleSize = ResolvePartFloat(u8"box", StyleProperty::Width,
                                                    GetControlState(), kCircleSize);
            const f32 r = circleSize * 0.5f;
            const f32 cy = Height() * 0.5f;
            const Rectangle boxRect{0, cy - r, circleSize, circleSize};

            ControlState state = GetControlState();
            if (IsChecked.Value())
            {
                state |= ControlState::Checked;
            }
            if (Drawable* boxDrawable =
                    ResolvePartDrawable(u8"box", StyleProperty::Background, state))
            {
                boxDrawable->Draw(ctx, boxRect, state);
                if (IsChecked.Value())
                {
                    if (Drawable* dotIcon =
                            ResolvePartDrawable(u8"mark", StyleProperty::Background, state))
                    {
                        dotIcon->Draw(ctx, boxRect);
                    }
                }
            }
            else if (IsChecked.Value())
            {
                DrawFallbackChecked(ctx, boxRect, cy);
            }
            else
            {
                DrawFallbackUnchecked(ctx, boxRect);
            }

            const StringView text = Text.Value();
            if (text.Size() > 0 && ctx.FontService() != nullptr)
            {
                if (fonts::CachedFont* font = ctx.FontService()->GetFont(
                        ResolveStyleFontFamily(FontFamily.Value()), fontSize))
                {
                    Color textColor =
                        TextColor.Value().HasValue()
                            ? TextColor.Value().Value()
                            : ResolveStyleColor(
                                  StyleProperty::TextColor,
                                  Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    if (!IsEffectivelyEnabled())
                    {
                        textColor = Palette::ComputeDisabled(textColor);
                    }
                    const f32 textX = circleSize + kCircleTextSpacing;
                    ctx.VG().DrawText(text, font, Rectangle{textX, 0, Width() - textX, Height()},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      textColor);
                }
            }
        }

    private:
        static constexpr f32 kCircleSize = 18.0f;
        static constexpr f32 kCircleTextSpacing = 8.0f;

        static void DrawFallbackUnchecked(UIDrawContext& ctx, const Rectangle& boxRect)
        {
            ctx.VG().FillRect(boxRect, Color{30.0f / 255.0f, 32.0f / 255.0f, 42.0f / 255.0f, 1.0f});
            ctx.VG().StrokeRect(
                boxRect, Color{100.0f / 255.0f, 105.0f / 255.0f, 120.0f / 255.0f, 1.0f}, 1.0f);
        }
        static void DrawFallbackChecked(UIDrawContext& ctx, const Rectangle& boxRect, f32 cy)
        {
            ctx.VG().FillRect(boxRect,
                              Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
            const f32 dotSize = boxRect.width * 0.4f;
            const f32 dotX = boxRect.x + (boxRect.width - dotSize) * 0.5f;
            ctx.VG().FillRect(Rectangle{dotX, cy - dotSize * 0.5f, dotSize, dotSize}, Color::White);
        }

        void Init()
        {
            IsChecked.SetOwner(this, InvalidationKind::Visual);
            Text.SetOwner(this);
            FontSize.SetOwner(this);
            FontFamily.SetOwner(this, InvalidationKind::Visual);
            TextColor.SetOwner(this, InvalidationKind::Visual);
            RadioButton* self = this;
            IsChecked.Changed.Add(Event<void(bool)>::Handler{
                [self](bool val) { self->OnCheckedChanged.Invoke(self, val); }});
            IsFocusable = true;
            IsTabStop = true;
            Cursor = CursorType::Hand;
        }
    };

    RTTI_DEFINE_OBJECT(RadioButton, "rtti::ui")
}
