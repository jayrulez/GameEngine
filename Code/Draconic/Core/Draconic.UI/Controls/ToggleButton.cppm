// Draconic UI - :toggle_button partition
//
// Stateful button that toggles checked/unchecked, with a content view (a Label by default). Ported
// from Sedulous.UI/src/Controls/ToggleButton.bf. Content is RefPtr-owned; its text renders through the
// Label content view (live now that the Fonts service is wired).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:toggle_button;

import draconic.foundation;
import draconic.vg;
import :button_base;
import :label;
import :view;
import :event;
import :property;
import :control_state;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;
import :drawable;
import :palette;
import :event_args;
import :input_enums;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    class ToggleButton : public ButtonBase
    {
        DRACONIC_OBJECT(ToggleButton, ButtonBase)
    public:
        Property<bool> IsChecked{false};
        Event<void(ToggleButton*, bool)> OnCheckedChanged;

        ToggleButton() { Wire(); }
        explicit ToggleButton(StringView text)
        {
            Wire();
            m_content = foundation::MakeRef<Label>(foundation::DefaultAllocator(), text);
        }

        [[nodiscard]] View* Content() const noexcept { return m_content.Get(); }
        void SetContent(RefPtr<View> content)
        {
            m_content = Move(content);
            Invalidate();
        }

        [[nodiscard]] ControlState GetControlState() const override
        {
            ControlState state = ControlState::Normal;
            if (!IsEffectivelyEnabled())
            {
                state |= ControlState::Disabled;
            }
            if (IsPressed())
            {
                state |= ControlState::Pressed;
            }
            if (IsFocused())
            {
                state |= ControlState::Focused;
            }
            if (IsHovered())
            {
                state |= ControlState::Hover;
            }
            if (IsChecked.Value())
            {
                state |= ControlState::Checked;
            }
            return state;
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left && IsPressed())
            {
                if (IsHovered())
                {
                    IsChecked.SetValue(!IsChecked.Value());
                }
            }
            ButtonBase::OnMouseUp(e);
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
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
            const Thickness pad =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{12.0f, 8.0f});
            const BoxConstraints inner = constraints.Deflate(pad).Loosen();
            f32 cw = 0, ch = 0;
            if (m_content)
            {
                if (m_content->Context == nullptr && Context != nullptr)
                {
                    Context->AttachView(m_content.Get());
                }
                SyncContentFont();
                m_content->Measure(inner);
                cw = m_content->MeasuredSize.x;
                ch = m_content->MeasuredSize.y;
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(cw + pad.TotalHorizontal()),
                                  constraints.ConstrainHeight(ch + pad.TotalVertical())};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (!m_content)
            {
                return;
            }
            const Thickness pad =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{12.0f, 8.0f});
            const f32 contentW = width - pad.TotalHorizontal();
            const f32 contentH = height - pad.TotalVertical();
            const f32 cx = pad.Left + (contentW - m_content->MeasuredSize.x) * 0.5f;
            const f32 cy = pad.Top + (contentH - m_content->MeasuredSize.y) * 0.5f;
            m_content->Layout(cx, cy, m_content->MeasuredSize.x, m_content->MeasuredSize.y);
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const ControlState state = GetControlState();
            if (IsChecked.Value())
            {
                if (Drawable* checkedBg = ResolveStyleDrawable(StyleProperty::CheckedBackground))
                {
                    checkedBg->Draw(ctx, bounds, state);
                }
                else if (Drawable* normalBg = ResolveStyleDrawable(StyleProperty::Background))
                {
                    normalBg->Draw(ctx, bounds, state);
                }
                else
                {
                    const f32 radius = ResolveStyleFloat(StyleProperty::CornerRadius, 4.0f);
                    Color color = ResolveStyleColor(
                        StyleProperty::AccentColor,
                        Color{80.0f / 255.0f, 150.0f / 255.0f, 240.0f / 255.0f, 1.0f});
                    if (HasFlag(state, ControlState::Disabled))
                    {
                        color = Palette::ComputeDisabled(color);
                    }
                    else if (HasFlag(state, ControlState::Pressed))
                    {
                        color = Palette::ComputePressed(color);
                    }
                    else if (HasFlag(state, ControlState::Hover))
                    {
                        color = Palette::ComputeHover(color);
                    }
                    ctx.VG().FillRoundedRect(bounds, radius, color);
                }
            }
            else
            {
                DrawButtonBackground(ctx, bounds, state);
            }

            if (m_content)
            {
                ctx.VG().PushState();
                ctx.VG().Translate(m_content->Bounds.x, m_content->Bounds.y);
                m_content->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }

    private:
        void Wire()
        {
            IsChecked.SetOwner(this, InvalidationKind::Visual);
            ToggleButton* self = this;
            IsChecked.Changed.Add(Event<void(bool)>::Handler{
                [self](bool val) { self->OnCheckedChanged.Invoke(self, val); }});
        }

        // The default content is a Label, which resolves FontSize on itself (so it'd pick up the global
        // View default, not the button's ButtonBase FontSize). Push the button's resolved size onto the
        // content Label so toggle buttons match regular buttons. (No-op for non-Label / explicitly-sized
        // content, and guarded so it doesn't re-invalidate every measure.)
        void SyncContentFont()
        {
            if (Label* lbl = Cast<Label>(m_content.Get()))
            {
                const f32 fs = ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
                if (!lbl->FontSize.Value().HasValue() || lbl->FontSize.Value().Value() != fs)
                {
                    lbl->FontSize.SetValue(Optional<f32>{fs});
                }
            }
        }

        RefPtr<View> m_content;
    };

    DRACONIC_DEFINE_OBJECT(ToggleButton, "draconic::ui")
}
