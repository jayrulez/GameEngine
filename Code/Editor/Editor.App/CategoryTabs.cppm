// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor App - :category_tabs partition
//
// A settings dialog's tabs, one per category: the column a category's fields go in, made the first
// time the category is named (so fields that name the same category share its tab, and the tabs
// follow the order the categories first appear). Each column is padded and sits in its own vertical
// scroll, so a long page scrolls inside the dialog without spilling over its buttons. Preferences
// and Project Settings both build on it, so the two read alike.
module;
#include "Core/Prelude.h"

export module editor.app:category_tabs;

import foundation.core;
import foundation.ui;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;

    class CategoryTabs
    {
    public:
        /// `allocator` is the owning dialog's; the tab view and its pages live in it.
        explicit CategoryTabs(IAllocator& allocator)
        {
            m_tabs = MakeRef<ui::TabView>(allocator);
            m_tabs->TabsClosable.SetValue(false);
        }

        /// The tab view, for the dialog's content.
        [[nodiscard]] ui::TabView& View() const noexcept { return *m_tabs; }

        /// The column of `category`'s tab, made (as the last tab) the first time it is named.
        ui::FlexLayout& Column(StringView category)
        {
            for (const Page& page : m_pages)
            {
                if (page.category.AsView() == category)
                {
                    return *page.column;
                }
            }
            auto column = MakeRef<ui::FlexLayout>(m_tabs->MemoryAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 8;
            column->Padding = ui::Thickness{12, 10};
            auto scroll = MakeRef<ui::ScrollView>(m_tabs->MemoryAllocator());
            scroll->VScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Auto);
            scroll->HScrollBarPolicy.SetValue(ui::ScrollBarPolicy::Never);
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            scroll->AddView(column.Get(), match);
            m_tabs->AddTab(category, scroll.Get());
            m_pages.PushBack(Page{String(category), column.Get()});
            return *column;
        }

        /// The tab of `category`, or -1.
        [[nodiscard]] i32 IndexOf(StringView category) const
        {
            for (usize i = 0; i < m_pages.Size(); ++i)
            {
                if (m_pages[i].category.AsView() == category)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        /// A quiet line of explanation in a page.
        static void AddNote(ui::FlexLayout& column, StringView text)
        {
            auto note = MakeRef<ui::Label>(column.MemoryAllocator(), text);
            note->FontSize.SetValue(11.0f);
            note->WordWrap.SetValue(true);
            note->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.55f, 1.0f}));
            ui::LayoutStyle match;
            match.Width = ui::SizeSpec::Match();
            column.AddView(note.Get(), match);
        }

    private:
        struct Page
        {
            String category;
            ui::FlexLayout* column = nullptr; // the tab view owns it
        };

        RefPtr<ui::TabView> m_tabs;
        Array<Page> m_pages;
    };
}
