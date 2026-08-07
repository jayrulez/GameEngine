// Draconic GUI - :tree_view partition
//
// TreeView: a virtualized, model-backed tree. Modeled on eepp's UITreeView. It flattens the
// currently-visible (expanded) nodes of a tree Model into a list and virtualizes that list
// through AbstractItemView - so a huge tree costs only the on-screen rows. Each row shows an
// indent (by depth), a clickable expand/collapse arrow (when the node has children), and the
// node's text. Expansion state is tracked by node id (ModelIndex::InternalId), so it survives
// re-flattening; the primary selection is remapped by id across expand/collapse.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:tree_view;

import draconic.foundation;  // RefPtr, MakeRef, Array, HashMap, Function, Move, Max, Float2
import draconic.fonts; // CachedFont
import draconic.vg;    // PathBuilder
import :rect;
import :event;
import :draw_context;
import :label;
import :ui_widget;
import :model_index;
import :model;
import :abstract_item_view;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    // A clickable expand/collapse arrow: points right when collapsed, down when expanded.
    class TreeArrow : public UIWidget
    {
        DRACONIC_OBJECT(TreeArrow, UIWidget)
    public:
        TreeArrow() { SetTag(foundation::StringView(u8"treearrow")); }
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
        void SetOnClicked(foundation::Function<void()> callback) { m_onClicked = foundation::Move(callback); }

    protected:
        void OnMouseClick(const MouseEvent&) override
        {
            if (m_onClicked)
                m_onClicked();
        }
        void OnDraw(DrawContext& ctx, const Rect&) override
        {
            const Rect b = GetLocalBounds();
            const f32 s = 4.0f; // half-size
            const f32 cx = b.x + b.width * 0.5f;
            const f32 cy = b.y + b.height * 0.5f;
            vg::PathBuilder pb;
            if (m_expanded)
            {
                pb.MoveTo(cx - s, cy - s * 0.5f);
                pb.LineTo(cx + s, cy - s * 0.5f);
                pb.LineTo(cx, cy + s * 0.75f);
            }
            else
            {
                pb.MoveTo(cx - s * 0.5f, cy - s);
                pb.LineTo(cx + s * 0.75f, cy);
                pb.LineTo(cx - s * 0.5f, cy + s);
            }
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), m_color);
        }

    private:
        bool m_expanded = false;
        Color m_color{0.80f, 0.84f, 0.90f, 1.0f};
        foundation::Function<void()> m_onClicked;
    };

    // A tree row: an ItemRow with an arrow (clickable) + a hit-transparent label.
    class TreeRow : public ItemRow
    {
        DRACONIC_OBJECT(TreeRow, ItemRow)
    public:
        TreeRow()
        {
            SetTag(foundation::StringView(u8"treerow"));
            m_arrow = foundation::MakeRef<TreeArrow>(foundation::DefaultAllocator());
            AddChild(m_arrow.Get());
            m_label = foundation::MakeRef<Label>(foundation::DefaultAllocator());
            m_label->SetTag(foundation::StringView(u8"treecell"));
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
            m_arrow->SetPosition(foundation::Float2{indent, 0.0f});
            m_arrow->SetSize(foundation::Float2{arrowSize, rowHeight});
            const f32 textX = indent + arrowSize;
            m_label->SetPosition(foundation::Float2{textX + 2.0f, 0.0f});
            m_label->SetSize(foundation::Float2{foundation::Max(0.0f, rowWidth - textX - 4.0f), rowHeight});
        }

    private:
        RefPtr<TreeArrow> m_arrow;
        RefPtr<Label> m_label;
    };

    class TreeView : public AbstractItemView
    {
        DRACONIC_OBJECT(TreeView, AbstractItemView)
    public:
        TreeView() { SetTag(foundation::StringView(u8"treeview")); }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (const RefPtr<ItemRow>& r : RowPool())
                static_cast<TreeRow*>(r.Get())->GetLabel()->SetFont(font);
            RequestRelayout();
        }
        void SetRowTextColor(Color color)
        {
            m_textColor = color;
            for (const RefPtr<ItemRow>& r : RowPool())
                static_cast<TreeRow*>(r.Get())->GetLabel()->SetTextColor(color);
        }
        void SetIndentWidth(f32 width)
        {
            m_indentWidth = foundation::Max(0.0f, width);
            RequestRelayout();
        }
        void SetThemeTextColor(Color color) override { SetRowTextColor(color); }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }

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

        [[nodiscard]] RefPtr<ItemRow> CreateItemRow() override
        {
            auto row = foundation::MakeRef<TreeRow>(foundation::DefaultAllocator());
            row->GetLabel()->SetFont(m_font);
            row->GetLabel()->SetTextColor(m_textColor);
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
        fonts::CachedFont* m_font = nullptr;
        f32 m_indentWidth = 16.0f;
        f32 m_arrowSize = 16.0f;
        Color m_textColor{0.88f, 0.90f, 0.94f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(TreeArrow, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(TreeRow, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(TreeView, "draconic::gui")
}
