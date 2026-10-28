// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :toggle_switch partition
//
// iOS-style toggle switch (track + knob). Ported from Sedulous.UI/src/Controls/ToggleSwitch.bf (a View).
// Track/knob + the text label are LIVE now that the Fonts service + VG are wired; toggle/state/event faithful.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:toggle_switch;

import foundation.core;
import foundation.vg;
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

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui
{
    class ToggleSwitch : public View
    {
        RTTI_OBJECT(ToggleSwitch, View)
    public:
        Property<bool> IsChecked{false};
        Property<String> Text;
        Property<f32> TrackWidth{44.0f};
        Property<f32> TrackHeight{24.0f};
        Property<f32> KnobSize{20.0f};
        Event<void(ToggleSwitch*, bool)> OnCheckedChanged;

        ToggleSwitch() { Init(); }
        explicit ToggleSwitch(StringView text)
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
            if (e.Button == MouseButton::Left)
            {
                IsChecked.SetValue(!IsChecked.Value());
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Key == KeyCode::Space || e.Key == KeyCode::Return)
            {
                IsChecked.SetValue(!IsChecked.Value());
                e.Handled = true;
            }
        }
        void OnActivate() override
        {
            if (IsEffectivelyEnabled())
            {
                IsChecked.SetValue(!IsChecked.Value());
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
            f32 textW = 0, textH = 0;
            const StringView text = Text.Value();
            if (text.Size() > 0 && Context != nullptr && Context->FontService() != nullptr)
            {
                if (fonts::CachedFont* font =
                        Context->FontService()->GetFont(ResolveStyleFontFamily(), fontSize))
                {
                    textW = font->font->MeasureString(text);
                    textH = font->font->Metrics().lineHeight;
                }
            }
            const f32 totalW = TrackWidth.Value() + ((textW > 0) ? kTextSpacing + textW : 0);
            const f32 totalH = Max(TrackHeight.Value(), textH);
            MeasuredSize =
                Float2{constraints.ConstrainWidth(totalW), constraints.ConstrainHeight(totalH)};
        }
        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 trackY = (Height() - TrackHeight.Value()) * 0.5f;
            const Rectangle trackRect{0, trackY, TrackWidth.Value(), TrackHeight.Value()};
            ControlState trackState = GetControlState();
            if (IsChecked.Value())
            {
                trackState |= ControlState::Checked;
            }

            if (Drawable* track =
                    ResolvePartDrawable(u8"track", StyleProperty::Background, trackState))
            {
                track->Draw(ctx, trackRect);
            }
            else
            {
                ctx.VG().FillRect(
                    trackRect, IsChecked.Value()
                                   ? Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f}
                                   : Color{42.0f / 255.0f, 44.0f / 255.0f, 54.0f / 255.0f, 1.0f});
                ctx.VG().StrokeRect(
                    trackRect,
                    ResolveStyleColor(StyleProperty::BorderColor,
                                      Color{65.0f / 255.0f, 70.0f / 255.0f, 85.0f / 255.0f, 1.0f}),
                    1.0f);
            }

            const f32 knobPad = (TrackHeight.Value() - KnobSize.Value()) * 0.5f;
            const f32 knobX =
                IsChecked.Value() ? (TrackWidth.Value() - KnobSize.Value() - knobPad) : knobPad;
            const Rectangle knobRect{knobX, trackY + knobPad, KnobSize.Value(), KnobSize.Value()};
            if (Drawable* knob =
                    ResolvePartDrawable(u8"knob", StyleProperty::Background, trackState))
            {
                knob->Draw(ctx, knobRect);
            }
            else
            {
                ctx.VG().FillRect(knobRect,
                                  Color{230.0f / 255.0f, 230.0f / 255.0f, 235.0f / 255.0f, 1.0f});
            }

            const StringView text = Text.Value();
            if (text.Size() > 0 && ctx.FontService() != nullptr)
            {
                const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
                if (fonts::CachedFont* font =
                        ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize))
                {
                    const Color textColor = ResolveStyleColor(
                        StyleProperty::TextColor,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    const f32 textX = TrackWidth.Value() + kTextSpacing;
                    ctx.VG().DrawText(text, font, Rectangle{textX, 0, Width() - textX, Height()},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      textColor);
                }
            }
        }

    private:
        static constexpr f32 kTextSpacing = 8.0f;

        void Init()
        {
            IsChecked.SetOwner(this, InvalidationKind::Visual);
            Text.SetOwner(this);
            TrackWidth.SetOwner(this);
            TrackHeight.SetOwner(this);
            KnobSize.SetOwner(this, InvalidationKind::Visual);
            ToggleSwitch* self = this;
            IsChecked.Changed.Add(Event<void(bool)>::Handler{
                [self](bool val) { self->OnCheckedChanged.Invoke(self, val); }});
            IsFocusable = true;
            IsTabStop = true;
            Cursor = CursorType::Hand;
        }
    };

    RTTI_DEFINE_OBJECT(ToggleSwitch, "rtti::ui")
}
