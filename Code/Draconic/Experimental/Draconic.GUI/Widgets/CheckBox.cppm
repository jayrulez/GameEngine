// Draconic GUI - :check_box partition
//
// CheckBox: a toggleable box. A lean Draconic-native control modeled on eepp's UICheckBox
// (role only). Clicking toggles the checked state and fires OnCheckedChanged; OnDraw renders
// a rounded box outline with a filled inner mark when checked. Hover/press reaction comes for
// free from UINode's control state (so a CSS background can highlight it). Pair it with a
// Label in a horizontal LinearLayout for the usual "[x] caption" look.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:check_box;

import draconic.foundation; // Color, Function, Move, Max
import draconic.vg;   // CornerRadii
import :rect;
import :event;
import :draw_context;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class CheckBox : public UIWidget
    {
        DRACONIC_OBJECT(CheckBox, UIWidget)
    public:
        CheckBox()
        {
            SetTag(foundation::StringView(u8"checkbox"));
            SetTabFocusable(true);
        }

        [[nodiscard]] bool IsChecked() const noexcept { return m_checked; }
        void SetChecked(bool checked)
        {
            if (checked == m_checked)
                return;
            m_checked = checked;
            Invalidate();
            if (m_onChanged)
                m_onChanged(m_checked);
        }
        void Toggle() { SetChecked(!m_checked); }

        void SetOnCheckedChanged(foundation::Function<void(bool)> callback)
        {
            m_onChanged = foundation::Move(callback);
        }

        void SetBoxColor(Color color)
        {
            m_boxColor = color;
            Invalidate();
        }
        void SetCheckColor(Color color)
        {
            m_checkColor = color;
            Invalidate();
        }

        // Theming parts: checkbox::box (outline) / ::mark (inner fill).
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"box"));
            out.PushBack(foundation::StringView(u8"mark"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"box"))
                SetBoxColor(color);
            else if (part == foundation::StringView(u8"mark"))
                SetCheckColor(color);
        }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            (void)event;
            Toggle();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect box = GetContentBounds();
            ctx.VG().StrokeRoundedRect(box.ToRectangle(), vg::CornerRadii(3.0f), m_boxColor, 2.0f);
            if (m_checked)
            {
                const f32 inset = 4.0f;
                const Rect inner{box.x + inset, box.y + inset,
                                 foundation::Max(0.0f, box.width - inset * 2.0f),
                                 foundation::Max(0.0f, box.height - inset * 2.0f)};
                ctx.VG().FillRoundedRect(inner.ToRectangle(), vg::CornerRadii(2.0f), m_checkColor);
            }
        }

        bool m_checked = false;
        Color m_boxColor{0.60f, 0.65f, 0.72f, 1.0f};
        Color m_checkColor{0.31f, 0.63f, 0.85f, 1.0f};
        foundation::Function<void(bool)> m_onChanged;
    };

    DRACONIC_DEFINE_OBJECT(CheckBox, "draconic::gui")
}
