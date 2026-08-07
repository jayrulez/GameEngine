// Draconic GUI - :combo_box partition
//
// ComboBox: a control that shows the current selection and, when clicked, drops down a list of
// choices. Modeled on eepp's UIDropDownList (role only), built by composition: the dropdown is
// a ListBox added as a top-level popup (over everything) via the EventDispatcher's popup
// support, so a click outside or Escape dismisses it. Picking an item sets the selection and
// closes the dropdown.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:combo_box;

import draconic.foundation;  // RefPtr, MakeRef, Array, String, Function, Move, Min, Float2
import draconic.fonts; // CachedFont
import draconic.vg;    // PathBuilder
import :rect;
import :event;
import :draw_context;
import :text;
import :node;
import :rectangle_drawable;
import :ui_widget;
import :list_box;
import :event_dispatcher;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class ComboBox : public UIWidget
    {
        DRACONIC_OBJECT(ComboBox, UIWidget)
    public:
        ComboBox()
        {
            SetTag(foundation::StringView(u8"combobox"));
            SetTabFocusable(true);
            // A visible box so it reads as a control even before it is opened.
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_boxColor));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            m_text.SetFont(font);
            if (m_dropdown)
                m_dropdown->SetFont(font);
        }
        void SetTextColor(Color color)
        {
            m_text.SetColor(color);
            Invalidate();
        }

        // Theming hooks (CSS color / font-family reach the text + the dropdown arrow).
        void SetThemeTextColor(Color color) override
        {
            SetTextColor(color);
            m_arrowColor = color;
        }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }

        void AddItem(foundation::StringView text)
        {
            m_items.PushBack(foundation::String(text));
            if (m_dropdown)
                m_dropdown->AddItem(text);
        }
        [[nodiscard]] usize ItemCount() const noexcept { return m_items.Size(); }

        [[nodiscard]] i32 GetSelectedIndex() const noexcept { return m_selected; }
        [[nodiscard]] foundation::StringView GetSelectedText() const
        {
            return (m_selected >= 0 && static_cast<usize>(m_selected) < m_items.Size())
                       ? m_items[static_cast<usize>(m_selected)].AsView()
                       : foundation::StringView{};
        }
        void SetSelectedIndex(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_items.Size()))
                return;
            if (index == m_selected)
                return;
            m_selected = index;
            m_text.SetString(m_items[static_cast<usize>(index)].AsView());
            Invalidate();
            if (m_onChanged)
                m_onChanged(index);
        }
        void SetOnSelectionChanged(foundation::Function<void(i32)> callback)
        {
            m_onChanged = foundation::Move(callback);
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
        void SetItemHeight(f32 height) { m_itemHeight = height; }

    protected:
        void OnMouseClick(const MouseEvent&) override
        {
            if (m_open)
                CloseDropdown();
            else
                OpenDropdown();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            // Selection text (leaving room for the arrow on the right).
            m_text.Draw(ctx, Rect{b.x, b.y, foundation::Max(0.0f, b.width - 16.0f), b.height});
            // A small down-arrow at the right.
            const f32 aw = 8.0f, ah = 5.0f;
            const f32 ax = b.x + b.width - aw - 4.0f;
            const f32 ay = b.y + (b.height - ah) * 0.5f;
            vg::PathBuilder pb;
            pb.MoveTo(ax, ay);
            pb.LineTo(ax + aw, ay);
            pb.LineTo(ax + aw * 0.5f, ay + ah);
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), m_arrowColor);
        }

    private:
        void BuildDropdown()
        {
            if (m_dropdown)
                return;
            m_dropdown = foundation::MakeRef<ListBox>(foundation::DefaultAllocator());
            m_dropdown->SetFont(m_font);
            m_dropdown->SetItemHeight(m_itemHeight);
            for (const foundation::String& item : m_items)
                m_dropdown->AddItem(item.AsView());
            ComboBox* self = this;
            m_dropdown->SetOnSelectionChanged([self](i32 i) { self->OnPicked(i); });
        }

        void OpenDropdown()
        {
            Node* root = GetRootNode();
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (root == nullptr || dispatcher == nullptr || m_items.IsEmpty())
                return;
            BuildDropdown();

            const foundation::Float2 below = ConvertToWorldSpace(foundation::Float2{0.0f, GetSize().y});
            const f32 h =
                foundation::Min(static_cast<f32>(m_items.Size()) * m_itemHeight, m_dropdownMaxHeight);
            m_dropdown->SetPosition(below);
            m_dropdown->SetSize(foundation::Float2{GetSize().x, h});
            root->AddChild(m_dropdown.Get());

            m_open = true;
            ComboBox* self = this;
            dispatcher->OpenPopup(m_dropdown.Get(), this, [self]() { self->CloseDropdown(); });
        }

        void CloseDropdown()
        {
            if (!m_open)
                return;
            m_open = false;
            if (m_dropdown)
                m_dropdown->RemoveFromParent();
        }

        void OnPicked(i32 index)
        {
            SetSelectedIndex(index);
            if (EventDispatcher* dispatcher = GetEventDispatcher())
                dispatcher->ClosePopup();
            else
                CloseDropdown();
        }

        Array<foundation::String> m_items;
        Text m_text;
        RefPtr<ListBox> m_dropdown;
        fonts::CachedFont* m_font = nullptr;
        i32 m_selected = -1;
        bool m_open = false;
        f32 m_itemHeight = 24.0f;
        f32 m_dropdownMaxHeight = 160.0f;
        Color m_boxColor{0.18f, 0.20f, 0.25f, 1.0f};
        Color m_arrowColor{0.75f, 0.80f, 0.86f, 1.0f};
        foundation::Function<void(i32)> m_onChanged;
    };

    DRACONIC_DEFINE_OBJECT(ComboBox, "draconic::gui")
}
