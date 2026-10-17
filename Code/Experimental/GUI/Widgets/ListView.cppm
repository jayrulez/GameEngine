// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :list_view partition
//
// ListView: a virtualized, single-column model-backed list. Modeled on eepp's UIListView. The
// virtualization, scrolling, and selection all live in AbstractItemView; ListView only says how
// a row is built (an ItemRow holding one Label) and bound (the label's text = the cell's data).
// Multi-selection is inherited (SetSelectionMode(Multi) + Ctrl/Shift).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:list_view;

import foundation.core;  // RefPtr, MakeRef, Move
import foundation.fonts; // CachedFont
import :rect;
import :label;
import :model_index;
import :model;
import :abstract_item_view;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    // A list row: an ItemRow containing a single (hit-transparent) Label.
    class ListRow : public ItemRow
    {
        RTTI_OBJECT(ListRow, ItemRow)
    public:
        ListRow()
        {
            SetTag(core::StringView(u8"listviewrow"));
            m_label = core::MakeRef<Label>(MemoryAllocator());
            m_label->SetTag(core::StringView(u8"listcell")); // container-owned, not a generic label
            StyleWriteScope defaults(*m_label); // its own values, which a sheet returns to
            m_label->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_label->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            m_label->SetHitTestVisible(false);
            AddChild(m_label.Get());
        }
        [[nodiscard]] Label* GetLabel() const noexcept { return m_label.Get(); }

    private:
        RefPtr<Label> m_label;
    };

    class ListView : public AbstractItemView
    {
        RTTI_OBJECT(ListView, AbstractItemView)
    public:
        ListView() { SetTag(core::StringView(u8"listview")); }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (const RefPtr<ItemRow>& r : RowPool())
                static_cast<ListRow*>(r.Get())->GetLabel()->SetFont(font);
            RequestRelayout();
        }
        void SetRowTextColor(Color color)
        {
            m_textColor = color;
            for (const RefPtr<ItemRow>& r : RowPool())
                static_cast<ListRow*>(r.Get())->GetLabel()->SetTextColor(color);
        }
        // `listview { color }` propagates to the rows (container-owned styling).
        void SetThemeTextColor(Color color) override { SetRowTextColor(color); }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }

        // Back-compat row-oriented aliases over the base's item-index selection.
        [[nodiscard]] i32 GetSelectedRow() const noexcept { return GetSelectedItem(); }
        void SetSelectedRow(i32 row) { SetSelectedItem(row); }

    protected:
        [[nodiscard]] RefPtr<ItemRow> CreateItemRow() override
        {
            auto row = core::MakeRef<ListRow>(MemoryAllocator());
            row->GetLabel()->SetFont(m_font);
            row->GetLabel()->SetTextColor(m_textColor);
            return row;
        }
        void BindItemRow(ItemRow& row, i32 item, f32 rowWidth) override
        {
            Label* label = static_cast<ListRow&>(row).GetLabel();
            label->SetText(GetModel()->Data(MakeModelIndex(item)).ToString().AsView());
            label->SetPosition(core::Float2{0.0f, 0.0f});
            label->SetSize(core::Float2{rowWidth, GetRowHeight()});
        }

    private:
        fonts::CachedFont* m_font = nullptr;
        Color m_textColor{0.88f, 0.90f, 0.94f, 1.0f};
    };

    RTTI_DEFINE_OBJECT(ListRow, "rtti::gui")
    RTTI_DEFINE_OBJECT(ListView, "rtti::gui")
}
