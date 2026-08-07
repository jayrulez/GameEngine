// Draconic UI - :checkbox partition
//
// Toggle checkbox with a text label. Ported from Sedulous.UI/src/Controls/CheckBox.bf (a View, not a
// ToggleButton). Box chrome (part drawable or fallback rounded rect + checkmark) and the text label are
// LIVE now that the Fonts service + VG are wired. Toggle/state/event logic is faithful.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:checkbox;

import draconic.foundation;
import draconic.vg;
import draconic.fonts; // CachedFont, TextAlignment, VerticalAlignment
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

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class CheckBox : public View
    {
        DRACONIC_OBJECT(CheckBox, View)
    public:
        Property<bool> IsChecked{false};
        Property<String> Text;
        Property<Optional<f32>> FontSize;
        Property<String> FontFamily;
        Property<Optional<foundation::Color>> TextColor;
        Event<void(CheckBox*, bool)> OnCheckedChanged;

        CheckBox() { Init(); }
        explicit CheckBox(StringView text)
        {
            Init();
            Text.SetSilent(String(text));
        }
        CheckBox(StringView text, bool isChecked)
        {
            Init();
            Text.SetSilent(String(text));
            IsChecked.SetSilent(isChecked);
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
            const f32 boxSize =
                ResolvePartFloat(u8"box", StyleProperty::Width, GetControlState(), 18.0f);
            const f32 spacing = ResolveStyleFloat(StyleProperty::Spacing, 6.0f);
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

            const f32 totalW = boxSize + ((textW > 0) ? spacing + textW : 0);
            const f32 totalH = Max(boxSize, textH);
            MeasuredSize =
                Float2{constraints.ConstrainWidth(totalW), constraints.ConstrainHeight(totalH)};
        }
        void OnDraw(UIDrawContext& ctx) override
        {
            ControlState state = GetControlState();
            if (IsChecked.Value())
            {
                state |= ControlState::Checked;
            }
            const f32 boxSize = ResolvePartFloat(u8"box", StyleProperty::Width, state, 18.0f);
            const f32 spacing = ResolveStyleFloat(StyleProperty::Spacing, 6.0f);
            const f32 fontSize = FontSize.Value().HasValue()
                                     ? FontSize.Value().Value()
                                     : ResolveStyleFloat(StyleProperty::FontSize, 16.0f);

            const f32 boxY = (Height() - boxSize) * 0.5f;
            const Rectangle boxRect{0, boxY, boxSize, boxSize};

            if (Drawable* boxDrawable =
                    ResolvePartDrawable(u8"box", StyleProperty::Background, state))
            {
                boxDrawable->Draw(ctx, boxRect, state);
                if (IsChecked.Value())
                {
                    if (Drawable* checkIcon =
                            ResolvePartDrawable(u8"checkmark", StyleProperty::Background, state))
                    {
                        checkIcon->Draw(ctx, boxRect);
                    }
                }
            }
            else if (IsChecked.Value())
            {
                DrawFallbackChecked(ctx, boxRect, boxSize);
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
                    const f32 textX = boxSize + spacing;
                    const Rectangle textRect{textX, 0, Width() - textX, Height()};
                    ctx.VG().DrawText(text, font, textRect, fonts::TextAlignment::Left,
                                      fonts::VerticalAlignment::Middle, textColor);
                }
            }
        }

    private:
        static void DrawFallbackUnchecked(UIDrawContext& ctx, const Rectangle& boxRect)
        {
            ctx.VG().FillRect(boxRect, Color{30.0f / 255.0f, 32.0f / 255.0f, 42.0f / 255.0f, 1.0f});
            ctx.VG().StrokeRect(
                boxRect, Color{100.0f / 255.0f, 105.0f / 255.0f, 120.0f / 255.0f, 1.0f}, 1.0f);
        }
        static void DrawFallbackChecked(UIDrawContext& ctx, const Rectangle& boxRect, f32 boxSize)
        {
            ctx.VG().FillRect(boxRect,
                              Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
            const f32 cx = boxRect.x + boxSize * 0.5f;
            const f32 cy = boxRect.y + boxSize * 0.5f;
            const f32 s = boxSize * 0.3f;
            ctx.VG().BeginPath();
            ctx.VG().MoveTo(cx - s, cy);
            ctx.VG().LineTo(cx - s * 0.3f, cy + s * 0.7f);
            ctx.VG().LineTo(cx + s, cy - s * 0.5f);
            ctx.VG().Stroke(Color::White, 2.0f);
        }

        void Init()
        {
            IsChecked.SetOwner(this, InvalidationKind::Visual);
            Text.SetOwner(this);
            FontSize.SetOwner(this);
            FontFamily.SetOwner(this, InvalidationKind::Visual);
            TextColor.SetOwner(this, InvalidationKind::Visual);
            CheckBox* self = this;
            IsChecked.Changed.Add(Event<void(bool)>::Handler{
                [self](bool val) { self->OnCheckedChanged.Invoke(self, val); }});
            IsFocusable = true;
            IsTabStop = true;
            Cursor = CursorType::Hand;
        }
    };

    DRACONIC_DEFINE_OBJECT(CheckBox, "draconic::ui")
}
