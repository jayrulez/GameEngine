// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tab_widget partition
//
// TabWidget: a row of tab buttons over a content area that shows the selected tab's panel.
// Modeled on eepp's UITabWidget (role only), built by composition: a horizontal LinearLayout
// of Buttons (the tab bar) plus a clipped content host that shows exactly one panel at a time.
// Clicking a tab (or SelectTab) swaps the visible panel and highlights the active tab.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:tab_widget;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, Max
import foundation.fonts; // CachedFont
import :rect;
import :node;
import :button;
import :linear_layout;
import :ui_widget;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class TabWidget : public UIWidget
    {
        RTTI_OBJECT(TabWidget, UIWidget)
    public:
        TabWidget()
        {
            SetTag(core::StringView(u8"tabwidget"));
            m_tabBar = core::MakeRef<LinearLayout>(MemoryAllocator());
            m_tabBar->SetOrientation(Orientation::Horizontal);
            m_tabBar->SetSpacing(2.0f);
            AddChild(m_tabBar.Get());

            m_contentHost = core::MakeRef<UIWidget>(MemoryAllocator());
            m_contentHost->SetClipChildren(true);
            AddChild(m_contentHost.Get());
        }

        // Add a tab with a title and its content panel (added to the content host, which takes
        // ownership). The first tab added becomes selected.
        void AddTab(core::StringView title, Node* content)
        {
            const i32 index = static_cast<i32>(m_tabs.Size());

            auto button = core::MakeRef<Button>(MemoryAllocator());
            button->SetText(title);
            button->SetFont(m_font);
            button->AddClass(
                core::StringView(u8"tab")); // styled as a tab; `.tab.selected` = active
            button->SetSize(core::Float2{m_tabWidth, m_tabBarHeight});
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
                    m_tabs[i].TabButton->AddClass(core::StringView(u8"selected"));
                else
                    m_tabs[i].TabButton->RemoveClass(core::StringView(u8"selected"));
            }
            if (m_onChanged)
                m_onChanged(index);
        }
        void SetOnTabChanged(core::Function<void(i32)> callback)
        {
            m_onChanged = core::Move(callback);
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
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); } // CSS font-family and font-size
        [[nodiscard]] fonts::CachedFont* GetFont() const { return m_font; }
        void SetTabBarHeight(f32 height)
        {
            m_tabBarHeight = core::Max(1.0f, height);
            Relayout();
        }
        void SetTabWidth(f32 width)
        {
            m_tabWidth = core::Max(1.0f, width);
            for (const Tab& t : m_tabs)
                t.TabButton->SetSize(core::Float2{width, m_tabBarHeight});
            Relayout();
        }

    protected:
        void OnSizeChange() override { Relayout(); }

    private:
        void Relayout()
        {
            const Rect box = GetContentBounds();
            m_tabBar->SetPosition(core::Float2{box.x, box.y});
            m_tabBar->SetSize(core::Float2{box.width, m_tabBarHeight});

            m_contentHost->SetPosition(core::Float2{box.x, box.y + m_tabBarHeight});
            const core::Float2 hostSize{box.width, core::Max(0.0f, box.height - m_tabBarHeight)};
            m_contentHost->SetSize(hostSize);
            for (const Tab& t : m_tabs)
            {
                if (t.Content == nullptr)
                    continue;
                t.Content->SetPosition(core::Float2{0.0f, 0.0f});
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
        core::Function<void(i32)> m_onChanged;
    };

    RTTI_DEFINE_OBJECT(TabWidget, "rtti::gui")
}
