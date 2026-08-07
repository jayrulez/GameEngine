// Draconic GUI - :radio partition
//
// RadioButton + RadioGroup: a mutually-exclusive selection. Modeled on eepp's UIRadioButton
// (role only). A RadioButton is like a CheckBox but circular and one-way: clicking selects it,
// and a RadioGroup ensures exactly one member is selected at a time (selecting one clears the
// rest). Hover/press reaction comes for free from UINode's control state. Pair each with a
// Label in a horizontal LinearLayout for the usual "(o) caption" look.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:radio;

import draconic.foundation; // Color, Function, Move, Array
import :rect;
import :event;
import :draw_context;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class RadioGroup; // controller defined below; a button routes selection through it

    class RadioButton : public UIWidget
    {
        DRACONIC_OBJECT(RadioButton, UIWidget)
        friend class RadioGroup;

    public:
        RadioButton()
        {
            SetTag(foundation::StringView(u8"radio"));
            SetTabFocusable(true);
        }

        [[nodiscard]] bool IsSelected() const noexcept { return m_selected; }

        // Select this button (routes through the group so the others clear). Selecting an
        // already-selected button is a no-op.
        void Select();
        void SetSelected(bool selected)
        {
            if (selected)
                Select();
            else
                SetSelectedState(false);
        }

        // Fired when this button becomes selected.
        void SetOnSelected(foundation::Function<void()> callback) { m_onSelected = foundation::Move(callback); }

        void SetRingColor(Color color)
        {
            m_ringColor = color;
            Invalidate();
        }
        void SetDotColor(Color color)
        {
            m_dotColor = color;
            Invalidate();
        }

        // Theming parts: radio::ring (outline) / ::dot (inner fill).
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"ring"));
            out.PushBack(foundation::StringView(u8"dot"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"ring"))
                SetRingColor(color);
            else if (part == foundation::StringView(u8"dot"))
                SetDotColor(color);
        }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            (void)event;
            Select();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect box = GetContentBounds();
            const f32 r = foundation::Min(box.width, box.height) * 0.5f;
            const foundation::Float2 c{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
            ctx.VG().StrokeCircle(c, r - 1.0f, m_ringColor, 2.0f);
            if (m_selected)
                ctx.VG().FillCircle(c, r * 0.5f, m_dotColor);
        }

    private:
        // Set the visual/selected flag directly (the group drives this); fires OnSelected on a
        // false->true transition.
        void SetSelectedState(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            Invalidate();
            if (m_selected && m_onSelected)
                m_onSelected();
        }

        RadioGroup* m_group = nullptr; // non-owning; set by RadioGroup::Add
        bool m_selected = false;
        Color m_ringColor{0.60f, 0.65f, 0.72f, 1.0f};
        Color m_dotColor{0.31f, 0.63f, 0.85f, 1.0f};
        foundation::Function<void(void)> m_onSelected;
    };

    // Controller (not a Node) that keeps exactly one of its member buttons selected.
    class RadioGroup
    {
    public:
        // Register a button (non-owning). It becomes part of the mutual-exclusion set.
        void Add(RadioButton* button)
        {
            if (button == nullptr)
                return;
            button->m_group = this;
            m_buttons.PushBack(button);
        }

        // Select `button` (must be a member): clears the others, selects it, and fires the
        // group callback with its index.
        void Select(RadioButton* button)
        {
            i32 index = -1;
            for (usize i = 0; i < m_buttons.Size(); ++i)
            {
                const bool isTarget = (m_buttons[i] == button);
                m_buttons[i]->SetSelectedState(isTarget);
                if (isTarget)
                    index = static_cast<i32>(i);
            }
            if (index != m_selectedIndex)
            {
                m_selectedIndex = index;
                if (m_onChanged)
                    m_onChanged(index);
            }
        }

        [[nodiscard]] i32 GetSelectedIndex() const noexcept { return m_selectedIndex; }
        [[nodiscard]] RadioButton* GetSelected() const
        {
            return (m_selectedIndex >= 0 && static_cast<usize>(m_selectedIndex) < m_buttons.Size())
                       ? m_buttons[static_cast<usize>(m_selectedIndex)]
                       : nullptr;
        }
        [[nodiscard]] usize Count() const noexcept { return m_buttons.Size(); }

        void SetOnSelectionChanged(foundation::Function<void(i32)> callback)
        {
            m_onChanged = foundation::Move(callback);
        }

    private:
        foundation::Array<RadioButton*> m_buttons; // non-owning (the tree owns the buttons)
        i32 m_selectedIndex = -1;
        foundation::Function<void(i32)> m_onChanged;
    };

    inline void RadioButton::Select()
    {
        if (m_group != nullptr)
            m_group->Select(this);
        else
            SetSelectedState(true);
    }

    DRACONIC_DEFINE_OBJECT(RadioButton, "draconic::gui")
}
