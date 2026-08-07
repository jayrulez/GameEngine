// Draconic GUI - :list_view partition
//
// ListView: a virtualized, single-column model-backed list. Modeled on eepp's UIListView. The
// virtualization, scrolling, and selection all live in AbstractItemView; ListView only says how
// a row is built (an ItemRow holding one Label) and bound (the label's text = the cell's data).
// Multi-selection is inherited (SetSelectionMode(Multi) + Ctrl/Shift).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:list_view;

import draconic.foundation;  // RefPtr, MakeRef, Move
import draconic.fonts; // CachedFont
import :rect;
import :label;
import :model_index;
import :model;
import :abstract_item_view;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    // A list row: an ItemRow containing a single (hit-transparent) Label.
    class ListRow : public ItemRow
    {
        DRACONIC_OBJECT(ListRow, ItemRow)
    public:
        ListRow()
        {
            SetTag(foundation::StringView(u8"listviewrow"));
            m_label = foundation::MakeRef<Label>(foundation::DefaultAllocator());
            m_label->SetTag(foundation::StringView(u8"listcell")); // container-owned, not a generic label
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
        DRACONIC_OBJECT(ListView, AbstractItemView)
    public:
        ListView() { SetTag(foundation::StringView(u8"listview")); }

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
            auto row = foundation::MakeRef<ListRow>(foundation::DefaultAllocator());
            row->GetLabel()->SetFont(m_font);
            row->GetLabel()->SetTextColor(m_textColor);
            return row;
        }
        void BindItemRow(ItemRow& row, i32 item, f32 rowWidth) override
        {
            Label* label = static_cast<ListRow&>(row).GetLabel();
            label->SetText(GetModel()->Data(MakeModelIndex(item)).ToString().AsView());
            label->SetPosition(foundation::Float2{0.0f, 0.0f});
            label->SetSize(foundation::Float2{rowWidth, GetRowHeight()});
        }

    private:
        fonts::CachedFont* m_font = nullptr;
        Color m_textColor{0.88f, 0.90f, 0.94f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(ListRow, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(ListView, "draconic::gui")
}
