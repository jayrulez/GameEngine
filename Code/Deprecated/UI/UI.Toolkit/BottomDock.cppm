// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :bottom_dock partition
//
// BottomDock: a Godot-style collapsible bottom strip. A thin tab bar is visible unless
// HideWhenCollapsed and nothing is open; above it sits a content region that is shown only when a
// tab is expanded. Tabs come and go at run time (AddTab, RemoveTab). Clicking the active tab collapses back to
// just the bar; clicking another tab switches to it and expands. The dock itself does not resize the
// viewport - it emits OnExpandedChanged, and the host (which places the dock as one pane of a SplitView)
// collapses/expands that pane via SplitView::SetPaneCollapsed, so the stored split ratio survives.
//
// Domain-agnostic + reusable: tabs carry a caller-owned content View* (the "Animation" dopesheet is the
// first occupant; Output/Debug-style tabs can join later). N-tab shaped from the start.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:bottom_dock;

import foundation.core;
import foundation.ui;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{
    /// A collapsible bottom strip: an always-visible tab bar + an expandable content region above it.
    class BottomDock : public ::foundation::ui::FlexLayout
    {
        RTTI_OBJECT(BottomDock, FlexLayout)
    public:
        using View = ::foundation::ui::View;

        /// Fires when the expanded state changes (true = expanded). The host wires this to
        /// SplitView::SetPaneCollapsed(pane, !expanded) so the dock's pane grows/shrinks with it.
        Event<void(bool)> OnExpandedChanged;

        BottomDock() { BuildChrome(); }

        /// Collapsed means GONE, the tab bar included, rather than down to the bar: a host that
        /// opens tabs from elsewhere (a toolbar toggle, a tool that docks a panel) gives the
        /// whole height back while nothing is open. A collapsed dock then measures nothing, so a
        /// split pane holding it takes no space.
        [[nodiscard]] bool HideWhenCollapsed() const noexcept { return m_hideWhenCollapsed; }
        void SetHideWhenCollapsed(bool hide)
        {
            m_hideWhenCollapsed = hide;
            SyncBar();
        }

        [[nodiscard]] bool HasTab(StringView id) const noexcept { return IndexOf(id) >= 0; }

        /// Register a tab. `content` is BORROWED (the caller owns the RefPtr and outlives the dock).
        /// The first tab added becomes the default target of an expand when none is active.
        void AddTab(StringView id, StringView label, View* content)
        {
            auto button = MakeRef<::foundation::ui::Button>(MemoryAllocator(), label);
            button->FontSize.SetValue(Optional<f32>{11.0f});
            // By id, not index: a removed tab shifts the ones after it.
            BottomDock* self = this;
            button->OnClick.Add([self, tabId = String(id)](::foundation::ui::ButtonBase*)
                                { self->OnTabClicked(self->IndexOf(tabId.AsView())); });
            {
                ::foundation::ui::LayoutStyle lp;
                lp.Width = ::foundation::ui::SizeSpec::Fixed(::foundation::ui::Unit::Dp(96.0f));
                lp.Height = ::foundation::ui::SizeSpec::Match();
                m_tabBar->AddView(button.Get(), lp);
            }
            if (content != nullptr)
            {
                content->Visibility = ::foundation::ui::Visibility::Gone; // shown only when active+expanded
                ::foundation::ui::LayoutStyle lp;
                lp.Width = ::foundation::ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                m_contentHost->AddView(content, lp);
            }

            Tab tab;
            tab.id = String(id);
            tab.label = String(label);
            tab.content = content;
            tab.button = button;
            m_tabs.PushBack(Move(tab));
        }

        /// Takes a tab away, its button and the dock's hold on its content. Removing the active
        /// tab collapses the dock. Unknown ids are a no-op.
        void RemoveTab(StringView id)
        {
            const i32 index = IndexOf(id);
            if (index < 0)
            {
                return;
            }
            const bool wasActive = index == m_activeIndex;
            Tab& tab = m_tabs[static_cast<usize>(index)];
            m_tabBar->RemoveView(tab.button.Get());
            if (tab.content != nullptr)
            {
                m_contentHost->RemoveView(tab.content);
            }
            m_tabs.RemoveAt(static_cast<usize>(index));
            if (wasActive)
            {
                m_activeIndex = -1;
                SetExpanded(false);
            }
            else if (m_activeIndex > index)
            {
                --m_activeIndex;
            }
            Invalidate();
        }

        [[nodiscard]] bool IsExpanded() const noexcept { return m_expanded; }
        [[nodiscard]] StringView ActiveTabId() const noexcept
        {
            return (m_activeIndex >= 0 && m_activeIndex < static_cast<i32>(m_tabs.Size()))
                       ? m_tabs[static_cast<usize>(m_activeIndex)].id.AsView()
                       : StringView{};
        }
        [[nodiscard]] usize TabCount() const noexcept { return m_tabs.Size(); }

        /// Drive a tab's toggle behavior by id (as if its bar button were clicked): expand+switch to it,
        /// or collapse if it is already the active+expanded tab. No-op if the id is unknown.
        void ClickTab(StringView id)
        {
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                if (m_tabs[i].id.AsView() == id)
                {
                    OnTabClicked(static_cast<i32>(i));
                    return;
                }
            }
        }

        /// Switch to a tab by id and expand (no-op if the id is unknown).
        void ActivateTab(StringView id)
        {
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                if (m_tabs[i].id.AsView() == id)
                {
                    m_activeIndex = static_cast<i32>(i);
                    SetExpanded(true);
                    return;
                }
            }
        }

        /// Collapse to just the bar, or expand the active tab (defaulting to the first if none active).
        void SetExpanded(bool expanded)
        {
            if (expanded && m_activeIndex < 0 && !m_tabs.IsEmpty())
            {
                m_activeIndex = 0;
            }
            if (expanded && m_activeIndex < 0)
            {
                return; // nothing to open
            }
            if (m_expanded == expanded)
            {
                SyncContent(); // still sync (e.g. ActivateTab switched the active tab while expanded)
                return;
            }
            m_expanded = expanded;
            if (m_contentHost.Get() != nullptr)
            {
                m_contentHost->Visibility =
                    expanded ? ::foundation::ui::Visibility::Visible : ::foundation::ui::Visibility::Gone;
            }
            SyncContent();
            SyncBar();
            Invalidate();
            OnExpandedChanged.Invoke(m_expanded);
        }

    private:
        struct Tab
        {
            String id;
            String label;
            View* content = nullptr; // borrowed
            RefPtr<::foundation::ui::Button> button;
        };

        void BuildChrome()
        {
            Direction = ::foundation::ui::Orientation::Vertical;
            Spacing = 0.0f;

            // Content region (grows; hidden while collapsed) ABOVE the always-visible bar.
            m_contentHost = MakeRef<::foundation::ui::FlexLayout>(MemoryAllocator());
            m_contentHost->Direction = ::foundation::ui::Orientation::Vertical;
            m_contentHost->Visibility = ::foundation::ui::Visibility::Gone; // default collapsed
            {
                ::foundation::ui::LayoutStyle lp;
                lp.Width = ::foundation::ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                AddView(m_contentHost.Get(), lp);
            }

            m_tabBar = MakeRef<::foundation::ui::FlexLayout>(MemoryAllocator());
            m_tabBar->Direction = ::foundation::ui::Orientation::Horizontal;
            m_tabBar->Spacing = 2.0f;
            m_tabBar->Padding = ::foundation::ui::Thickness{4, 2};
            {
                ::foundation::ui::LayoutStyle lp;
                lp.Width = ::foundation::ui::SizeSpec::Match();
                lp.Height = ::foundation::ui::SizeSpec::Fixed(::foundation::ui::Unit::Dp(kBarHeight));
                AddView(m_tabBar.Get(), lp);
            }
        }

        void OnTabClicked(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_tabs.Size()))
            {
                return;
            }
            if (m_expanded && m_activeIndex == index)
            {
                SetExpanded(false); // clicking the active tab collapses
                return;
            }
            m_activeIndex = index;
            SetExpanded(true); // switch to it + expand
        }

        [[nodiscard]] i32 IndexOf(StringView id) const noexcept
        {
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                if (m_tabs[i].id.AsView() == id)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        // The bar shows unless collapsed means gone.
        void SyncBar()
        {
            if (m_tabBar.Get() != nullptr)
            {
                m_tabBar->Visibility = (m_hideWhenCollapsed && !m_expanded)
                                           ? ::foundation::ui::Visibility::Gone
                                           : ::foundation::ui::Visibility::Visible;
            }
            Invalidate();
        }

        // Only the active tab's content is visible, and only while expanded.
        void SyncContent()
        {
            for (usize i = 0; i < m_tabs.Size(); ++i)
            {
                View* content = m_tabs[i].content;
                if (content == nullptr)
                {
                    continue;
                }
                const bool show = m_expanded && (static_cast<i32>(i) == m_activeIndex);
                content->Visibility =
                    show ? ::foundation::ui::Visibility::Visible : ::foundation::ui::Visibility::Gone;
            }
        }

        static constexpr f32 kBarHeight = 26.0f;

        RefPtr<::foundation::ui::FlexLayout> m_contentHost;
        RefPtr<::foundation::ui::FlexLayout> m_tabBar;
        Array<Tab> m_tabs;
        i32 m_activeIndex = -1;
        bool m_expanded = false;
        bool m_hideWhenCollapsed = false;
    };

    RTTI_DEFINE_OBJECT(BottomDock, "rtti::ui::toolkit")
}
