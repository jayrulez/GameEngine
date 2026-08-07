// Draconic UI - :expander partition
//
// Collapsible container with a clickable header and expandable body. Ported from
// Sedulous.UI/src/Controls/Expander.bf. (IsExpanded property -> IsExpanded()/SetIsExpanded(); header
// text is live now that the Fonts service is wired; chevron/header chrome kept.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:expander;

import draconic.foundation;
import draconic.vg;
import draconic.fonts; // CachedFont, TextAlignment, VerticalAlignment
import :view;
import :event;
import :property;
import :control_state;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;
import :drawable;
import :event_args;
import :input_enums;
import :enums;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class Expander : public ViewGroup
    {
        DRACONIC_OBJECT(Expander, ViewGroup)
    public:
        Property<f32> HeaderHeight{28.0f};
        Property<f32> ContentSpacing{4.0f};
        Event<void(Expander*, bool)> OnExpandedChanged;

        Expander()
        {
            IsFocusable = true;
            Cursor = CursorType::Hand;
            HeaderHeight.SetOwner(this);
            ContentSpacing.SetOwner(this);
        }
        explicit Expander(StringView headerText) : Expander() { m_headerText = String(headerText); }

        [[nodiscard]] bool IsExpanded() const noexcept { return m_isExpanded; }
        void SetIsExpanded(bool value)
        {
            if (m_isExpanded == value)
            {
                return;
            }
            m_isExpanded = value;
            if (m_content != nullptr)
            {
                m_content->Visibility = m_isExpanded ? Visibility::Visible : Visibility::Gone;
            }
            Invalidate();
            OnExpandedChanged.Invoke(this, m_isExpanded);
        }

        /// Set the expandable body content.
        void SetContent(View* content, LayoutParamsPtr lp = {})
        {
            if (m_content != nullptr)
            {
                RemoveView(m_content, true);
            }
            m_content = content;
            if (content != nullptr)
            {
                content->Visibility = m_isExpanded ? Visibility::Visible : Visibility::Gone;
                AddView(content, Move(lp));
            }
        }

        /// Right-aligned action widgets in the header (e.g. copy / remove icon buttons). They dispatch
        /// before the header toggle (a click that a child handles sets e.Handled, so OnMouseDown skips
        /// Toggle) - so clicking an action runs it, clicking empty header space toggles. Optional.
        void SetHeaderActions(View* actions, LayoutParamsPtr lp = {})
        {
            if (m_headerActions != nullptr)
            {
                RemoveView(m_headerActions, true);
            }
            m_headerActions = actions;
            if (actions != nullptr)
            {
                AddView(actions, Move(lp));
            }
        }

        void Toggle() { SetIsExpanded(!m_isExpanded); }
        void Expand() { SetIsExpanded(true); }
        void Collapse() { SetIsExpanded(false); }
        void SetHeaderText(StringView text)
        {
            m_headerText = String(text);
            Invalidate();
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Handled || e.Button != MouseButton::Left)
            {
                return;
            }
            const f32 screenX = Context != nullptr ? Context->GetInputManager()->MouseX() : 0.0f;
            const f32 screenY = Context != nullptr ? Context->GetInputManager()->MouseY() : 0.0f;
            const Float2 local = ScreenToLocal(Float2{screenX, screenY});
            if (local.y >= 0 && local.y <= HeaderHeight.Value())
            {
                Toggle();
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            switch (e.Key)
            {
            case KeyCode::Space:
            case KeyCode::Return:
                Toggle();
                e.Handled = true;
                break;
            case KeyCode::Right:
                if (!m_isExpanded)
                {
                    SetIsExpanded(true);
                    e.Handled = true;
                }
                break;
            case KeyCode::Left:
                if (m_isExpanded)
                {
                    SetIsExpanded(false);
                    e.Handled = true;
                }
                break;
            default:
                break;
            }
        }
        void OnActivate() override { Toggle(); }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle headerRect{0, 0, Width(), HeaderHeight.Value()};
            const ControlState headerState = GetControlState();
            if (Drawable* header =
                    ResolvePartDrawable(u8"header", StyleProperty::Background, headerState))
            {
                header->Draw(ctx, headerRect, headerState);
            }
            else
            {
                ctx.VG().FillRect(headerRect,
                                  Color{50.0f / 255.0f, 55.0f / 255.0f, 68.0f / 255.0f, 1.0f});
            }

            const f32 arrowSize = 8.0f, arrowX = 8.0f, arrowCY = HeaderHeight.Value() * 0.5f;
            ControlState chevronState = headerState;
            if (m_isExpanded)
            {
                chevronState |= ControlState::Checked;
            }
            const Color arrowColor =
                ResolvePartColor(u8"chevron", StyleProperty::TextColor, chevronState,
                                 Color{180.0f / 255.0f, 185.0f / 255.0f, 200.0f / 255.0f, 1.0f});
            if (Drawable* chevron =
                    ResolvePartDrawable(u8"chevron", StyleProperty::Background, chevronState))
            {
                chevron->Draw(ctx,
                              Rectangle{arrowX, arrowCY - arrowSize * 0.5f, arrowSize, arrowSize});
            }
            else
            {
                ctx.VG().BeginPath();
                if (m_isExpanded)
                {
                    ctx.VG().MoveTo(arrowX, arrowCY - arrowSize * 0.25f);
                    ctx.VG().LineTo(arrowX + arrowSize * 0.5f, arrowCY + arrowSize * 0.25f);
                    ctx.VG().LineTo(arrowX + arrowSize, arrowCY - arrowSize * 0.25f);
                }
                else
                {
                    ctx.VG().MoveTo(arrowX + arrowSize * 0.25f, arrowCY - arrowSize * 0.5f);
                    ctx.VG().LineTo(arrowX + arrowSize * 0.75f, arrowCY);
                    ctx.VG().LineTo(arrowX + arrowSize * 0.25f, arrowCY + arrowSize * 0.5f);
                }
                ctx.VG().Stroke(arrowColor, 2.0f);
            }

            if (m_headerText.Size() > 0 && ctx.FontService() != nullptr)
            {
                const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 16.0f);
                if (fonts::CachedFont* font =
                        ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize))
                {
                    const Color textColor = ResolveStyleColor(
                        StyleProperty::TextColor,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    const f32 textX = arrowX + arrowSize + 8.0f;
                    ctx.VG().DrawText(
                        m_headerText, font,
                        Rectangle{textX, 0, Width() - textX - 4.0f, HeaderHeight.Value()},
                        fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle, textColor);
                }
            }
            DrawChildren(ctx);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            f32 contentH = 0;
            if (m_content != nullptr && m_content->Visibility != Visibility::Gone)
            {
                const BoxConstraints inner = constraints.Deflate(Padding).Loosen();
                const Thickness margin =
                    m_content->LayoutParams ? m_content->LayoutParams->Margin : Thickness{};
                m_content->Measure(inner.Deflate(margin));
                contentH =
                    ContentSpacing.Value() + m_content->MeasuredSize.y + margin.TotalVertical();
            }
            if (m_headerActions != nullptr && m_headerActions->Visibility != Visibility::Gone)
            {
                m_headerActions->Measure(constraints.Loosen());
            }
            MeasuredSize = Float2{constraints.ConstrainWidth(constraints.MaxWidth),
                                  constraints.ConstrainHeight(HeaderHeight.Value() + contentH)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_headerActions != nullptr && m_headerActions->Visibility != Visibility::Gone)
            {
                const f32 aw = m_headerActions->MeasuredSize.x;
                const f32 ah = m_headerActions->MeasuredSize.y;
                const f32 pad = 4.0f;
                m_headerActions->Layout(Max(0.0f, width - aw - pad),
                                        Max(0.0f, (HeaderHeight.Value() - ah) * 0.5f), aw, ah);
            }
            if (m_content != nullptr && m_content->Visibility != Visibility::Gone)
            {
                const Thickness margin =
                    m_content->LayoutParams ? m_content->LayoutParams->Margin : Thickness{};
                const f32 contentTop = HeaderHeight.Value() + ContentSpacing.Value();
                m_content->Layout(margin.Left, contentTop + margin.Top,
                                  Max(0.0f, width - margin.TotalHorizontal()),
                                  Max(0.0f, height - contentTop - margin.TotalVertical()));
            }
        }

    private:
        String m_headerText{};
        View* m_content = nullptr;       // owned by m_children (via AddView)
        View* m_headerActions = nullptr; // right-aligned header widgets; owned by m_children
        bool m_isExpanded = true;
    };

    DRACONIC_DEFINE_OBJECT(Expander, "draconic::ui")
}
