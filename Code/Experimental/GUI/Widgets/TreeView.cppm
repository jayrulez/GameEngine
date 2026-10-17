// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tree_view partition
//
// TreeView: a virtualized, model-backed tree. Modeled on eepp's UITreeView. It flattens the
// currently-visible (expanded) nodes of a tree Model into a list and virtualizes that list
// through AbstractItemView - so a huge tree costs only the on-screen rows. Each row shows an
// indent (by depth), a clickable expand/collapse arrow (when the node has children), and the
// node's text. Expansion state is tracked by node id (ModelIndex::InternalId), so it survives
// re-flattening; the primary selection is remapped by id across expand/collapse.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:tree_view;

import foundation.core;  // RefPtr, MakeRef, Array, HashMap, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :rect;
import :event;
import :draw_context;
import :disclosure; // the chevron an expander draws too
import :label;
import :ui_widget;
import :model_index;
import :model;
import :abstract_item_view;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    // A clickable expand/collapse arrow: points right when collapsed, down when expanded.
    class TreeArrow : public UIWidget
    {
        RTTI_OBJECT(TreeArrow, UIWidget)
    public:
        TreeArrow() { SetTag(core::StringView(u8"treearrow")); }
        void SetExpanded(bool expanded)
        {
            if (m_expanded != expanded)
            {
                m_expanded = expanded;
                Invalidate();
            }
        }
        void SetColor(Color color)
        {
            m_color = color;
            Invalidate();
        }
        void SetOnClicked(core::Function<void()> callback) { m_onClicked = core::Move(callback); }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            if (m_onClicked)
                m_onClicked();
        }
        void OnDraw(DrawContext& ctx, const Rect&) override { DrawDisclosure(ctx, GetLocalBounds(), m_expanded, m_color); }

    private:
        bool m_expanded = false;
        Color m_color{0.80f, 0.84f, 0.90f, 1.0f};
        core::Function<void()> m_onClicked;
    };

    // A tree row: an ItemRow with an arrow (clickable) + a hit-transparent label.
    class TreeRow : public ItemRow
    {
        RTTI_OBJECT(TreeRow, ItemRow)
    public:
        TreeRow()
        {
            SetTag(core::StringView(u8"treerow"));
            m_arrow = core::MakeRef<TreeArrow>(MemoryAllocator());
            AddChild(m_arrow.Get());
            m_label = core::MakeRef<Label>(MemoryAllocator());
            m_label->SetTag(core::StringView(u8"treecell"));
            StyleWriteScope defaults(*m_label); // its own values, which a sheet returns to
            m_label->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_label->SetHitTestVisible(false);
            AddChild(m_label.Get());
        }
        [[nodiscard]] Label* GetLabel() const noexcept { return m_label.Get(); }
        [[nodiscard]] TreeArrow* GetArrow() const noexcept { return m_arrow.Get(); }

        void Configure(f32 indent, f32 arrowSize, f32 rowHeight, f32 rowWidth, bool hasChildren,
                       bool expanded)
        {
            m_arrow->SetVisible(hasChildren);
            m_arrow->SetExpanded(expanded);
            m_arrow->SetPosition(core::Float2{indent, 0.0f});
            m_arrow->SetSize(core::Float2{arrowSize, rowHeight});
            const f32 textX = indent + arrowSize;
            m_label->SetPosition(core::Float2{textX + 2.0f, 0.0f});
            m_label->SetSize(core::Float2{core::Max(0.0f, rowWidth - textX - 4.0f), rowHeight});
        }

    private:
        RefPtr<TreeArrow> m_arrow;
        RefPtr<Label> m_label;
    };

    class TreeView : public AbstractItemView
    {
        RTTI_OBJECT(TreeView, AbstractItemView)
    public:
        TreeView()
        {
            SetTag(core::StringView(u8"treeview"));
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetTextColor(Color{0.88f, 0.90f, 0.94f, 1.0f});
        }
        void SetIndentWidth(f32 width)
        {
            m_indentWidth = core::Max(0.0f, width);
            RequestRelayout();
        }

        // Expansion by the flat item index (a visible row).
        void ExpandItem(i32 item) { SetExpanded(item, true); }
        void CollapseItem(i32 item) { SetExpanded(item, false); }
        void ToggleItem(i32 item)
        {
            if (item >= 0 && item < static_cast<i32>(m_flat.Size()))
                SetExpanded(item, !IsExpandedId(m_flat[static_cast<usize>(item)].Index.InternalId));
        }
        [[nodiscard]] bool IsItemExpanded(i32 item) const
        {
            return item >= 0 && item < static_cast<i32>(m_flat.Size()) &&
                   IsExpandedId(m_flat[static_cast<usize>(item)].Index.InternalId);
        }
        [[nodiscard]] i32 ItemDepth(i32 item) const
        {
            return (item >= 0 && item < static_cast<i32>(m_flat.Size()))
                       ? m_flat[static_cast<usize>(item)].Depth
                       : 0;
        }

    protected:
        [[nodiscard]] usize ItemCount() const override { return m_flat.Size(); }
        [[nodiscard]] ModelIndex ItemToModelIndex(i32 item) const override
        {
            return (item >= 0 && item < static_cast<i32>(m_flat.Size()))
                       ? m_flat[static_cast<usize>(item)].Index
                       : ModelIndex{};
        }
        void OnModelChanged() override { Reflatten(); }

        // Its rows show its font and colour (a sheet's inheritance gives them the same).
        void OnTextStyleChange() override
        {
            for (const RefPtr<ItemRow>& r : RowPool())
                ShareTextStyle(*static_cast<TreeRow*>(r.Get())->GetLabel());
            RequestRelayout();
        }

        [[nodiscard]] RefPtr<ItemRow> CreateItemRow() override
        {
            auto row = core::MakeRef<TreeRow>(MemoryAllocator());
            ShareTextStyle(*row->GetLabel());
            TreeView* self = this;
            TreeRow* raw = row.Get();
            row->GetArrow()->SetOnClicked([self, raw]() { self->ToggleItem(raw->GetItemIndex()); });
            return row;
        }

        void BindItemRow(ItemRow& row, i32 item, f32 rowWidth) override
        {
            const FlatEntry& entry = m_flat[static_cast<usize>(item)];
            TreeRow& treeRow = static_cast<TreeRow&>(row);
            treeRow.GetLabel()->SetText(GetModel()->Data(entry.Index).ToString().AsView());
            treeRow.Configure(static_cast<f32>(entry.Depth) * m_indentWidth, m_arrowSize,
                              GetRowHeight(), rowWidth, entry.HasChildren,
                              IsExpandedId(entry.Index.InternalId));
        }

        // Left collapses (or moves to parent); Right expands (or moves to first child).
        bool OnExtraKey(KeyCode key) override
        {
            const i32 item = GetSelectedItem();
            if (item < 0 || item >= static_cast<i32>(m_flat.Size()))
                return false;
            if (key == KeyCode::Right)
            {
                if (m_flat[static_cast<usize>(item)].HasChildren && !IsItemExpanded(item))
                {
                    ExpandItem(item);
                    return true;
                }
                return false;
            }
            if (key == KeyCode::Left)
            {
                if (m_flat[static_cast<usize>(item)].HasChildren && IsItemExpanded(item))
                {
                    CollapseItem(item);
                    return true;
                }
                return false;
            }
            return false;
        }

    private:
        struct FlatEntry
        {
            ModelIndex Index;
            i32 Depth;
            bool HasChildren;
        };

        [[nodiscard]] bool IsExpandedId(i64 id) const { return m_expanded.Find(id) != nullptr; }

        void SetExpanded(i32 item, bool expanded)
        {
            if (item < 0 || item >= static_cast<i32>(m_flat.Size()))
                return;
            const i64 id = m_flat[static_cast<usize>(item)].Index.InternalId;

            // Remember the selected node's id so we can remap after the flat list changes.
            const i32 selItem = GetSelectedItem();
            const i64 selId = selItem >= 0 ? ItemToModelIndex(selItem).InternalId : -1;

            if (expanded)
                m_expanded.InsertOrAssign(id, true);
            else
                m_expanded.Remove(id);
            Reflatten();

            if (selId >= 0)
            {
                const i32 newItem = FlatIndexOfId(selId);
                if (newItem >= 0)
                    SelectItemSilent(newItem);
                else
                    ClearSelection();
            }
            RequestRelayout();
        }

        [[nodiscard]] i32 FlatIndexOfId(i64 id) const
        {
            for (usize i = 0; i < m_flat.Size(); ++i)
                if (m_flat[i].Index.InternalId == id)
                    return static_cast<i32>(i);
            return -1;
        }

        void Reflatten()
        {
            m_flat.Clear();
            if (GetModel() != nullptr)
                FlattenLevel(ModelIndex{}, 0);
        }
        void FlattenLevel(const ModelIndex& parent, i32 depth)
        {
            IModel* model = GetModel();
            const usize rows = model->RowCount(parent);
            for (usize r = 0; r < rows; ++r)
            {
                const ModelIndex index = model->Index(static_cast<i32>(r), 0, parent);
                const bool hasChildren = model->HasChildren(index);
                m_flat.PushBack(FlatEntry{index, depth, hasChildren});
                if (hasChildren && IsExpandedId(index.InternalId))
                    FlattenLevel(index, depth + 1);
            }
        }

        Array<FlatEntry> m_flat;       // currently-visible nodes, in display order
        HashMap<i64, bool> m_expanded; // node id -> expanded (presence = expanded)
        f32 m_indentWidth = 16.0f;
        f32 m_arrowSize = 16.0f;
    };

    RTTI_DEFINE_OBJECT(TreeArrow, "rtti::gui")
    RTTI_DEFINE_OBJECT(TreeRow, "rtti::gui")
    RTTI_DEFINE_OBJECT(TreeView, "rtti::gui")
}
