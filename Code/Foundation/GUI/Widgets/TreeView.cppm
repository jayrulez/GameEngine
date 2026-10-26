// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tree_view partition
//
// TreeView: a virtualised tree over a model (eepp's UITreeView in role). It flattens the open
// nodes into the items ItemView realises, so a huge tree costs only the rows in view. A row shows
// its indent, a disclosure chevron (the expander's) that opens and closes it, its icon where the
// model gives one (ModelRole::Icon, with ModelRole::IconClass a class on it, as a type's category
// tints it), and its text.
// Expansion is a set of node ids, as the selection is, so both survive the flattening; closing a
// node that holds the selection selects the node. Right opens a node (or goes to its first
// child), Left closes it (or goes to its parent). In markup its children are nested
// `<TreeItem text="..." expanded="true" icon="..." icon-class="...">`s, which become a model of
// its own.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:tree_view;

import foundation.core; // RefPtr, MakeRef, Array, HashSet, HashMap, Function, Move, Max
import :rect;
import :event;
import :draw_context;
import :disclosure; // the chevron an expander draws too
import :node;
import :label;
import :text; // TextHAlign
import :ui_widget;
import :layout_style;
import :image; // a row's icon
import :model_index;
import :model;
import :item_view;
import :tree_model;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    // A node's chevron: right when closed, down when open; a click opens or closes the node.
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
        void SetOnClicked(core::Function<void()> callback) { m_onClicked = core::Move(callback); }

    protected:
        // On the press (as a row selects on it), and taken: the row under it does not select.
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button != MouseButton::Left)
                return;
            event.Handled = true;
            if (m_onClicked)
                m_onClicked();
        }
        // The click after the press is the chevron's too: a row that folds on a click of its own
        // (a property grid's header) would fold back what the press folded.
        void OnMouseClick(const MouseEvent& event) override
        {
            if (event.Button == MouseButton::Left)
                event.Handled = true;
        }
        void OnDraw(DrawContext& ctx, const Rect&) override { DrawDisclosure(ctx, GetLocalBounds(), m_expanded, m_color, IsRightToLeft()); }

    private:
        bool m_expanded = false;
        Color m_color{0.80f, 0.84f, 0.90f, 1.0f};
        core::Function<void()> m_onClicked;
    };

    // A tree row's content: its indent, its chevron, its icon (`treeicon`, when it has one) and its
    // text (`treecell`).
    class TreeRowContent : public UIWidget
    {
        RTTI_OBJECT(TreeRowContent, UIWidget)
    public:
        TreeRowContent()
        {
            SetTag(core::StringView(u8"treerowcontent"));
            m_arrow = core::MakeRef<TreeArrow>(MemoryAllocator());
            AddChild(m_arrow.Get());
            m_label = core::MakeRef<Label>(MemoryAllocator());
            m_label->SetTag(core::StringView(u8"treecell"));
            StyleWriteScope defaults(*m_label); // its own values, which a sheet returns to
            m_label->SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            m_label->SetHitTestVisible(false);
            AddChild(m_label.Get());
        }
        [[nodiscard]] Label* GetLabel() const noexcept { return m_label.Get(); }
        [[nodiscard]] TreeArrow* GetArrow() const noexcept { return m_arrow.Get(); }
        /// Its icon, made when it first has one (none: null).
        [[nodiscard]] Image* GetIcon() const noexcept { return m_icon.Get(); }
        void Configure(f32 indent, f32 arrowSize, bool hasChildren, bool expanded)
        {
            m_indent = indent;
            m_arrowSize = arrowSize;
            m_arrow->SetVisible(hasChildren);
            m_arrow->SetExpanded(expanded);
            RequestLayout();
        }
        /// Its icon: `source` an image's (empty: none), `iconClass` a class on it, `size` its square.
        void SetIcon(core::StringView source, core::StringView iconClass, f32 size)
        {
            m_iconSize = source.IsEmpty() ? 0.0f : size;
            if (source.IsEmpty())
            {
                if (m_icon)
                    m_icon->SetDisplayed(false);
                RequestLayout();
                return;
            }
            if (!m_icon)
            {
                m_icon = core::MakeRef<Image>(MemoryAllocator());
                m_icon->SetTag(core::StringView(u8"treeicon"));
                m_icon->SetHitTestVisible(false);
                AddChildAt(m_icon.Get(), 1); // after the arrow, before the text
            }
            m_icon->SetDisplayed(true);
            if (m_icon->GetSource().AsView() != source)
                m_icon->SetSource(core::String(source));
            if (m_iconClass.AsView() != iconClass)
            {
                if (m_iconClass.Size() != 0)
                    m_icon->RemoveClass(m_iconClass.AsView());
                m_iconClass = core::String(iconClass);
                if (m_iconClass.Size() != 0)
                    m_icon->AddClass(m_iconClass.AsView());
            }
            RequestLayout();
        }
        template <typename Fn>
        static void ForEachClass(core::StringView classes, Fn&& fn)
        {
            usize at = 0;
            while (at < classes.Size())
            {
                while (at < classes.Size() && classes[at] == u8' ')
                    ++at;
                usize end = at;
                while (end < classes.Size() && classes[end] != u8' ')
                    ++end;
                if (end > at)
                    fn(classes.SubStr(at, end - at));
                at = end;
            }
        }
        /// Classes the model gives the row (space separated; empty: none), in place of those it gave
        /// before: a state a sheet styles (dimmed, a member of something).
        void SetRowClasses(core::StringView classes)
        {
            if (m_rowClasses.AsView() == classes)
                return;
            ForEachClass(m_rowClasses.AsView(), [this](core::StringView cls) { RemoveClass(cls); });
            m_rowClasses = core::String(classes);
            ForEachClass(m_rowClasses.AsView(), [this](core::StringView cls) { AddClass(cls); });
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2 available) override
        {
            const f32 textX = TextStart();
            const core::Float2 text = m_label->Measure(LayoutConstraints{core::Float2{0.0f, 0.0f}, core::Float2{core::Max(0.0f, available.x - textX), kUnbounded},
                                                                         core::Float2{-1.0f, -1.0f}});
            return core::Float2{textX + text.x, core::Max(core::Max(text.y, m_arrowSize), m_iconSize)};
        }

    protected:
        void OnLayout() override
        {
            // Indented from the inline start, the arrow before the text.
            const Rect box = GetContentBounds();
            const bool rtl = IsRightToLeft();
            m_arrow->Arrange(MirroredIn(Rect{box.x + m_indent, box.y, m_arrowSize, box.height}, box, rtl));
            if (m_icon && m_iconSize > 0.0f)
            {
                const f32 top = box.y + (box.height - m_iconSize) * 0.5f;
                m_icon->Arrange(MirroredIn(Rect{box.x + m_indent + m_arrowSize + 2.0f, top, m_iconSize, m_iconSize}, box, rtl));
            }
            const f32 textX = TextStart();
            m_label->Arrange(MirroredIn(Rect{box.x + textX, box.y, core::Max(0.0f, box.width - textX - 2.0f), box.height}, box, rtl));
        }
        void OnTextStyleChange() override { ShareTextStyle(*m_label); }

    private:
        // Where its text starts: after the indent, the arrow's column and its icon with a gap.
        [[nodiscard]] f32 TextStart() const noexcept
        {
            const f32 icon = m_iconSize > 0.0f ? m_iconSize + kIconGap : 0.0f;
            return m_indent + m_arrowSize + 2.0f + icon;
        }

        core::RefPtr<TreeArrow> m_arrow;
        core::RefPtr<Image> m_icon;
        core::RefPtr<Label> m_label;
        core::String m_iconClass;
        core::String m_rowClasses; // the model's (RowClass)
        f32 m_indent = 0.0f;
        f32 m_arrowSize = 16.0f;
        f32 m_iconSize = 0.0f; // none shown
        static constexpr f32 kIconGap = 4.0f;
    };

    /// A node of a tree written in markup: its text, whether it starts open, and its children.
    class TreeItem : public UIWidget
    {
        RTTI_OBJECT(TreeItem, UIWidget)
    public:
        TreeItem()
        {
            SetTag(core::StringView(u8"treeitem"));
            SetDisplayed(false); // data, not drawn
        }
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name != core::StringView(u8"text"))
                return UIWidget::SetMarkupAttribute(name, value);
            m_text = core::String(value);
            return true;
        }
        [[nodiscard]] core::String GetText() const { return m_text; }
        void SetText(const core::String& text) { m_text = text; }
        /// Its icon: an image's source, and a class on it a sheet styles.
        [[nodiscard]] core::String GetIcon() const { return m_icon; }
        void SetIcon(const core::String& source) { m_icon = source; }
        [[nodiscard]] core::String GetIconClass() const { return m_iconClass; }
        void SetIconClass(const core::String& iconClass) { m_iconClass = iconClass; }
        void SetExpanded(bool expanded) noexcept { m_expanded = expanded; }
        [[nodiscard]] bool IsExpanded() const noexcept { return m_expanded; }

    private:
        core::String m_text;
        core::String m_icon;
        core::String m_iconClass;
        bool m_expanded = false;
    };

    class TreeView : public ItemView
    {
        RTTI_OBJECT(TreeView, ItemView)
    public:
        TreeView() : m_expanded(MemoryAllocator()), m_itemOfId(MemoryAllocator())
        {
            SetTag(core::StringView(u8"treeview"));
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetTextColor(Color{0.88f, 0.90f, 0.94f, 1.0f});
            // Sideways too, as a table across its columns: its rows are as wide as its widest
            // (RowWidth), and without it a row wider than the view was only cut at its edge.
            EditLayoutStyle([](LayoutStyle& s) { s.OverflowX = Overflow::Auto; });
        }
        // Its own model goes before the view's base would let go of it: let go first.
        ~TreeView() override { SetModel(nullptr); }

        /// A level's indent (`indent-width`) and the column a row keeps for its arrow (`arrow-size`),
        /// the arrow drawn to fit it: a sheet sets them (an editor's density).
        void SetIndentWidth(f32 width)
        {
            m_indentWidth = core::Max(0.0f, width);
            ItemsChanged();
        }
        [[nodiscard]] f32 GetIndentWidth() const noexcept { return m_indentWidth; }
        void SetArrowSize(f32 size)
        {
            m_arrowSize = core::Max(0.0f, size);
            ItemsChanged();
        }
        [[nodiscard]] f32 GetArrowSize() const noexcept { return m_arrowSize; }
        /// A row's icon's square (`icon-size`), where the model gives one.
        void SetIconSize(f32 size)
        {
            m_iconSize = core::Max(0.0f, size);
            ItemsChanged();
        }
        [[nodiscard]] f32 GetIconSize() const noexcept { return m_iconSize; }

        // Expansion by item (a visible row).
        void ExpandItem(i32 item) { SetExpanded(item, true); }
        void CollapseItem(i32 item) { SetExpanded(item, false); }
        void ToggleItem(i32 item) { SetExpanded(item, !IsItemExpanded(item)); }
        [[nodiscard]] bool IsItemExpanded(i32 item) const
        {
            return item >= 0 && item < static_cast<i32>(m_flat.Size()) && m_expanded.Contains(m_flat[static_cast<usize>(item)].Id);
        }
        [[nodiscard]] i32 ItemDepth(i32 item) const
        {
            return item >= 0 && item < static_cast<i32>(m_flat.Size()) ? m_flat[static_cast<usize>(item)].Depth : 0;
        }
        /// Expansion by node id (open or not, shown or not).
        [[nodiscard]] bool IsIdExpanded(ItemId id) const { return m_expanded.Contains(id); }
        void SetIdExpanded(ItemId id, bool expanded)
        {
            if (expanded)
                (void)m_expanded.Insert(id);
            else
                (void)m_expanded.Remove(id);
            Reflatten();
            ItemsChanged();
        }

        /// Markup's children are its items (nested TreeItems), made into a model of its own.
        [[nodiscard]] Node* GetChildHost() override
        {
            if (!m_markupHost)
            {
                TreeView* self = this;
                m_markupHost = core::MakeRef<MarkupChildHost>(MemoryAllocator(), [self](Node& host) { self->AdoptItems(host); });
            }
            return m_markupHost.Get();
        }

    protected:
        [[nodiscard]] usize ItemCount() const override { return m_flat.Size(); }
        [[nodiscard]] ModelIndex ItemToModelIndex(i32 item) const override
        {
            return item >= 0 && item < static_cast<i32>(m_flat.Size()) ? m_flat[static_cast<usize>(item)].Index : ModelIndex{};
        }
        [[nodiscard]] ItemId ItemIdAt(i32 item) const override
        {
            return item >= 0 && item < static_cast<i32>(m_flat.Size()) ? m_flat[static_cast<usize>(item)].Id : kNoItem;
        }
        [[nodiscard]] i32 ItemOfId(ItemId id) const override
        {
            const i32* item = m_itemOfId.Find(id);
            return item != nullptr ? *item : -1;
        }
        [[nodiscard]] bool IsFlat() const override { return false; }
        // A drag resting on a closed node opens it (E7).
        bool OnDragHoverOpen(core::Float2 local) override
        {
            const i32 item = ItemAt(ConvertToWorldSpace(local));
            if (item < 0 || !m_flat[static_cast<usize>(item)].HasChildren || IsItemExpanded(item))
                return false;
            ExpandItem(item);
            return true;
        }
        void OnModelChanged() override { Reflatten(); }
        [[nodiscard]] core::StringView RowTag() const override { return core::StringView(u8"treerow"); }

        [[nodiscard]] core::RefPtr<Node> CreateContent(u32) override
        {
            auto content = core::MakeRef<TreeRowContent>(MemoryAllocator());
            TreeView* self = this;
            TreeRowContent* raw = content.Get();
            content->GetArrow()->SetOnClicked([self, raw]()
            {
                if (const ItemRow* row = core::Cast<ItemRow>(raw->GetParent()))
                    self->ToggleItem(row->GetItemIndex());
            });
            return content;
        }
        void BindContent(Node& content, i32 item, const ModelIndex& index) override
        {
            const FlatEntry& entry = m_flat[static_cast<usize>(item)];
            TreeRowContent& row = static_cast<TreeRowContent&>(content);
            row.GetLabel()->SetText(GetModel()->Data(index).ToString().AsView());
            const core::String icon = GetModel()->Data(index, ModelRole::Icon).ToString();
            row.SetIcon(icon.AsView(), GetModel()->Data(index, ModelRole::IconClass).ToString().AsView(), m_iconSize);
            row.SetRowClasses(GetModel()->Data(index, ModelRole::RowClass).ToString().AsView());
            row.Configure(static_cast<f32>(entry.Depth) * m_indentWidth, m_arrowSize, entry.HasChildren, m_expanded.Contains(entry.Id));
            NoteRowWidth(content);
        }
        // Rows as wide as the widest one bound, or the view when it is wider: a deep level or a
        // long name scrolls sideways, where it had been cut at the view's edge.
        [[nodiscard]] f32 RowWidth(f32 width) const override { return core::Max(width, m_widest); }
        // The rows in view measured again once placed: one bound before the sheet reached it (a
        // row just made has no font yet, its name no width) is measured as it is drawn.
        void OnLayoutDecorations() override
        {
            for (const core::RefPtr<ItemRow>& row : LiveRows())
                NoteRowWidth(*row);
        }

        // The arrow toward the inline end (Right, Left in right to left) opens a node, or on an open
        // one goes to its first child; toward the start closes an open one, or goes to its parent.
        bool OnExtraKey(KeyCode key, u32 modifiers) override
        {
            const i32 item = GetCurrentItem();
            if (modifiers != 0 || item < 0 || item >= static_cast<i32>(m_flat.Size()))
                return false;
            const FlatEntry& entry = m_flat[static_cast<usize>(item)];
            const i32 along = InlineStep(key);
            if (along > 0 && entry.HasChildren)
            {
                if (!IsItemExpanded(item))
                    ExpandItem(item);
                else
                {
                    SelectByUser(item + 1, 0);
                    ScrollToItem(item + 1);
                }
                return true;
            }
            if (along < 0)
            {
                if (entry.HasChildren && IsItemExpanded(item))
                {
                    CollapseItem(item);
                    return true;
                }
                const i32 parent = ParentItem(item);
                if (parent >= 0)
                {
                    SelectByUser(parent, 0);
                    ScrollToItem(parent);
                    return true;
                }
            }
            return false;
        }

    private:
        void AdoptItems(Node& host)
        {
            if (!m_ownModel)
                m_ownModel = core::MakeUnique<TreeModel>(MemoryAllocator());
            while (host.ChildCount() != 0)
            {
                core::RefPtr<Node> child(host.GetChildAt(0));
                host.RemoveChild(child.Get());
                if (const TreeItem* item = core::Cast<TreeItem>(child.Get()))
                    AddItem(*item, TreeModel::kRoot);
            }
            if (GetModel() != m_ownModel.Get())
                SetModel(m_ownModel.Get());
            Reflatten();
            ItemsChanged();
        }
        void AddItem(const TreeItem& item, i32 parent)
        {
            const i32 id = m_ownModel->AddNode(parent, item.GetText().AsView());
            if (item.GetIcon().Size() != 0)
                m_ownModel->SetIcon(id, item.GetIcon().AsView(), item.GetIconClass().AsView());
            if (item.IsExpanded())
                (void)m_expanded.Insert(static_cast<ItemId>(id));
            for (usize i = 0; i < item.ChildCount(); ++i)
                if (const TreeItem* child = core::Cast<TreeItem>(item.GetChildAt(i)))
                    AddItem(*child, id);
        }

        struct FlatEntry
        {
            ModelIndex Index;
            ItemId Id;
            i32 Depth;
            bool HasChildren;
        };

        void SetExpanded(i32 item, bool expanded)
        {
            if (item < 0 || item >= static_cast<i32>(m_flat.Size()))
                return;
            const ItemId id = m_flat[static_cast<usize>(item)].Id;
            if (expanded == m_expanded.Contains(id))
                return;
            if (expanded)
                (void)m_expanded.Insert(id);
            else
            {
                // Closing a node that holds the keyboard's row moves it, selected, to the node.
                const i32 current = GetCurrentItem();
                const bool holdsCurrent = current > item && IsUnder(current, item);
                (void)m_expanded.Remove(id);
                if (holdsCurrent)
                    SetSelectedItem(item);
            }
            Reflatten();
            ItemsChanged();
        }
        // Whether flat item `item` lies under flat item `ancestor`.
        [[nodiscard]] bool IsUnder(i32 item, i32 ancestor) const
        {
            const i32 depth = m_flat[static_cast<usize>(ancestor)].Depth;
            for (i32 i = ancestor + 1; i < static_cast<i32>(m_flat.Size()); ++i)
            {
                if (m_flat[static_cast<usize>(i)].Depth <= depth)
                    return false;
                if (i == item)
                    return true;
            }
            return false;
        }
        [[nodiscard]] i32 ParentItem(i32 item) const
        {
            const i32 depth = m_flat[static_cast<usize>(item)].Depth;
            for (i32 i = item - 1; i >= 0; --i)
                if (m_flat[static_cast<usize>(i)].Depth < depth)
                    return i;
            return -1;
        }

        // The widest row bound so far, kept while the model is the same one (a model built again on
        // each change, the hierarchy's, would else snap the sideways scroll back as rows rebind).
        void NoteRowWidth(Node& content)
        {
            if (GetModel() != m_widestOf)
            {
                m_widestOf = GetModel();
                m_widest = 0.0f;
            }
            Node* row = core::Cast<ItemRow>(&content) != nullptr ? &content : (content.GetParent() != nullptr ? content.GetParent() : &content); // the row's own padding too
            const f32 width = row->Measure(LayoutConstraints{core::Float2{0.0f, 0.0f}, core::Float2{kUnbounded, kUnbounded}, core::Float2{-1.0f, -1.0f}}).x;
            if (width > m_widest + 0.5f)
            {
                m_widest = width;
                RequestLayout();
            }
        }

        void Reflatten()
        {
            m_flat.Clear();
            m_itemOfId.Clear();
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
                const ItemId id = model->IdOf(index);
                const bool hasChildren = model->HasChildren(index);
                m_itemOfId.InsertOrAssign(id, static_cast<i32>(m_flat.Size()));
                m_flat.PushBack(FlatEntry{index, id, depth, hasChildren});
                if (hasChildren && m_expanded.Contains(id))
                    FlattenLevel(index, depth + 1);
            }
        }

        Array<FlatEntry> m_flat;          // the open nodes, in display order
        core::HashSet<ItemId> m_expanded; // the open nodes' ids
        HashMap<ItemId, i32> m_itemOfId;  // an id's flat item
        f32 m_indentWidth = 16.0f;
        f32 m_arrowSize = 16.0f;
        f32 m_iconSize = 16.0f; // a row's icon's square
        f32 m_widest = 0.0f;              // the widest row bound (NoteRowWidth)
        const IModel* m_widestOf = nullptr; // the model it was measured over
        core::UniquePtr<TreeModel> m_ownModel; // markup's items
        core::RefPtr<Node> m_markupHost;       // where markup's items land, then taken in
    };

    RTTI_DEFINE_OBJECT(TreeArrow, "rtti::gui")
    RTTI_DEFINE_OBJECT(TreeRowContent, "rtti::gui")
}
