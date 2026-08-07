// Draconic GUI - :tab_widget partition
//
// TabWidget: a row of tab buttons over a content area that shows the selected tab's panel.
// Modeled on eepp's UITabWidget (role only), built by composition: a horizontal LinearLayout
// of Buttons (the tab bar) plus a clipped content host that shows exactly one panel at a time.
// Clicking a tab (or SelectTab) swaps the visible panel and highlights the active tab.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:tab_widget;

import draconic.foundation;  // RefPtr, MakeRef, Array, Function, Move, Max
import draconic.fonts; // CachedFont
import :rect;
import :node;
import :button;
import :linear_layout;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    class TabWidget : public UIWidget
    {
        DRACONIC_OBJECT(TabWidget, UIWidget)
    public:
        TabWidget()
        {
            SetTag(foundation::StringView(u8"tabwidget"));
            m_tabBar = foundation::MakeRef<LinearLayout>(foundation::DefaultAllocator());
            m_tabBar->SetOrientation(Orientation::Horizontal);
            m_tabBar->SetSpacing(2.0f);
            AddChild(m_tabBar.Get());

            m_contentHost = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
            m_contentHost->SetClipChildren(true);
            AddChild(m_contentHost.Get());
        }

        // Add a tab with a title and its content panel (added to the content host, which takes
        // ownership). The first tab added becomes selected.
        void AddTab(foundation::StringView title, Node* content)
        {
            const i32 index = static_cast<i32>(m_tabs.Size());

            auto button = foundation::MakeRef<Button>(foundation::DefaultAllocator());
            button->SetText(title);
            button->SetFont(m_font);
            button->AddClass(
                foundation::StringView(u8"tab")); // styled as a tab; `.tab.selected` = active
            button->SetSize(foundation::Float2{m_tabWidth, m_tabBarHeight});
            TabWidget* self = this;
            button->SetOnClick([self, index]() { self->SelectTab(index); });
            m_tabBar->AddChild(button.Get());

            if (content != nullptr)
            {
                content->SetVisible(false);
                m_contentHost->AddChild(content);
            }

            m_tabs.PushBack(Tab{button.Get(), content});
            Relayout();
            if (m_tabs.Size() == 1)
                SelectTab(0);
        }

        [[nodiscard]] usize TabCount() const noexcept { return m_tabs.Size(); }
        [[nodiscard]] i32 GetSelectedIndex() const noexcept { return m_selected; }

        void SelectTab(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_tabs.Size()))
                return;
            m_selected = index;
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                const bool active = (static_cast<i32>(i) == index);
                if (m_tabs[i].Content != nullptr)
                    m_tabs[i].Content->SetVisible(active);
                // The active tab carries a `selected` class so the theme distinguishes it
                // (`.tab.selected`), surviving the per-frame style re-apply.
                if (active)
                    m_tabs[i].TabButton->AddClass(foundation::StringView(u8"selected"));
                else
                    m_tabs[i].TabButton->RemoveClass(foundation::StringView(u8"selected"));
            }
            if (m_onChanged)
                m_onChanged(index);
        }
        void SetOnTabChanged(foundation::Function<void(i32)> callback)
        {
            m_onChanged = foundation::Move(callback);
        }

        // The content panel of a tab (for populating it after AddTab).
        [[nodiscard]] Node* GetTabContent(usize index) const
        {
            return index < m_tabs.Size() ? m_tabs[index].Content : nullptr;
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (const Tab& t : m_tabs)
                t.TabButton->SetFont(font);
        }
        void SetTabBarHeight(f32 height)
        {
            m_tabBarHeight = foundation::Max(1.0f, height);
            Relayout();
        }
        void SetTabWidth(f32 width)
        {
            m_tabWidth = foundation::Max(1.0f, width);
            for (const Tab& t : m_tabs)
                t.TabButton->SetSize(foundation::Float2{width, m_tabBarHeight});
            Relayout();
        }

    protected:
        void OnSizeChange() override { Relayout(); }

    private:
        void Relayout()
        {
            const Rect box = GetContentBounds();
            m_tabBar->SetPosition(foundation::Float2{box.x, box.y});
            m_tabBar->SetSize(foundation::Float2{box.width, m_tabBarHeight});

            m_contentHost->SetPosition(foundation::Float2{box.x, box.y + m_tabBarHeight});
            const foundation::Float2 hostSize{box.width, foundation::Max(0.0f, box.height - m_tabBarHeight)};
            m_contentHost->SetSize(hostSize);
            for (const Tab& t : m_tabs)
            {
                if (t.Content == nullptr)
                    continue;
                t.Content->SetPosition(foundation::Float2{0.0f, 0.0f});
                t.Content->SetSize(hostSize);
            }
        }

        struct Tab
        {
            Button* TabButton;
            Node* Content;
        };

        RefPtr<LinearLayout> m_tabBar;
        RefPtr<UIWidget> m_contentHost;
        Array<Tab> m_tabs; // buttons owned by the bar, content by the host
        fonts::CachedFont* m_font = nullptr;
        i32 m_selected = -1;
        f32 m_tabBarHeight = 32.0f;
        f32 m_tabWidth = 100.0f;
        foundation::Function<void(i32)> m_onChanged;
    };

    DRACONIC_DEFINE_OBJECT(TabWidget, "draconic::gui")
}
