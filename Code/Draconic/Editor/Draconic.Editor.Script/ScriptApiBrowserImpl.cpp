// Draconic::EditorScript - the `draconic.editor.script` module.
//
// ScriptApiBrowserView implementation: the openable API panel on a script page. A filter
// box over a TreeView of the language's bound API (namespace/class > members), built by
// BuildScriptApiTree from the page's shared ScriptApiSurface. Double-clicking a row hands
// its insert text to OnInsert. Rebuilds are DEFERRED: filter edits mark dirty, Update()
// (the page's OnUpdate) does the TreeView::SetAdapter churn outside event dispatch.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.editor.script;

import draconic.foundation;
import draconic.ui;
import draconic.script;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace script = draconic::script;

    namespace
    {
        [[nodiscard]] bool LessThan(StringView a, StringView b)
        {
            const usize common = a.Size() < b.Size() ? a.Size() : b.Size();
            for (usize i = 0; i < common; ++i)
            {
                if (a[i] != b[i])
                {
                    return a[i] < b[i];
                }
            }
            return a.Size() < b.Size();
        }

        [[nodiscard]] bool ContainsIgnoreCase(StringView haystack, StringView needle)
        {
            if (needle.IsEmpty())
            {
                return true;
            }
            if (haystack.Size() < needle.Size())
            {
                return false;
            }
            const auto lower = [](char8_t c) -> char8_t
            { return (c >= u8'A' && c <= u8'Z') ? static_cast<char8_t>(c + 32) : c; };
            for (usize start = 0; start + needle.Size() <= haystack.Size(); ++start)
            {
                usize i = 0;
                while (i < needle.Size() && lower(haystack[start + i]) == lower(needle[i]))
                {
                    ++i;
                }
                if (i == needle.Size())
                {
                    return true;
                }
            }
            return false;
        }
    }

    ScriptApiTree BuildScriptApiTree(const Array<script::ScriptApiType>& types,
                                     StringView filter)
    {
        // Alphabetical type order (index sort - the surface stays untouched).
        Array<usize> typeOrder;
        for (usize i = 0; i < types.Size(); ++i)
        {
            typeOrder.PushBack(i);
        }
        typeOrder.Sort(
            [&types](usize a, usize b)
            { return LessThan(types[a].scriptName.AsView(), types[b].scriptName.AsView()); });

        ScriptApiTree tree;
        for (usize typeIndex : typeOrder)
        {
            const script::ScriptApiType& type = types[typeIndex];
            const bool typeMatches = ContainsIgnoreCase(type.scriptName.AsView(), filter);

            Array<usize> memberOrder;
            for (usize i = 0; i < type.members.Size(); ++i)
            {
                if (typeMatches ||
                    ContainsIgnoreCase(type.members[i].name.AsView(), filter))
                {
                    memberOrder.PushBack(i);
                }
            }
            if (!typeMatches && memberOrder.IsEmpty())
            {
                continue; // neither the type nor any member survived the filter
            }
            memberOrder.Sort(
                [&type](usize a, usize b)
                { return LessThan(type.members[a].name.AsView(), type.members[b].name.AsView()); });

            ScriptApiTreeNode typeNode;
            typeNode.label = String(type.scriptName.AsView());
            if (IsEditorOnlyBinding(type.typeId))
            {
                typeNode.label.Append(u8" [editor]");
            }
            typeNode.insertText = String(type.scriptName.AsView());
            typeNode.depth = 0;
            const i32 typeNodeIndex = static_cast<i32>(tree.nodes.Size());
            tree.nodes.PushBack(Move(typeNode));
            tree.roots.PushBack(typeNodeIndex);

            for (usize memberIndex : memberOrder)
            {
                const script::ScriptApiMember& member = type.members[memberIndex];
                ScriptApiTreeNode memberNode;
                memberNode.label = String(member.signature.IsEmpty()
                                              ? member.name.AsView()
                                              : member.signature.AsView());
                memberNode.insertText = String(member.name.AsView());
                memberNode.depth = 1;
                const i32 memberNodeIndex = static_cast<i32>(tree.nodes.Size());
                tree.nodes.PushBack(Move(memberNode));
                tree.nodes[static_cast<usize>(typeNodeIndex)].children.PushBack(
                    memberNodeIndex);
            }
        }
        return tree;
    }

    // Node ids ARE indices into m_tree.nodes; -1 is the virtual root.
    class ScriptApiBrowserView::TreeAdapter final : public ui::ITreeAdapter
    {
    public:
        explicit TreeAdapter(ScriptApiBrowserView& owner) : m_owner(&owner) {}

        [[nodiscard]] i32 RootCount() const override
        {
            return static_cast<i32>(m_owner->m_tree.roots.Size());
        }
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override
        {
            if (nodeId == -1)
            {
                return RootCount();
            }
            return InRange(nodeId) ? static_cast<i32>(Node(nodeId).children.Size()) : 0;
        }
        [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override
        {
            if (childIndex < 0)
            {
                return -1;
            }
            if (parentId == -1)
            {
                return childIndex < RootCount()
                           ? m_owner->m_tree.roots[static_cast<usize>(childIndex)]
                           : -1;
            }
            if (!InRange(parentId))
            {
                return -1;
            }
            const Array<i32>& kids = Node(parentId).children;
            return childIndex < static_cast<i32>(kids.Size())
                       ? kids[static_cast<usize>(childIndex)]
                       : -1;
        }
        [[nodiscard]] i32 GetDepth(i32 nodeId) const override
        {
            return InRange(nodeId) ? Node(nodeId).depth : 0;
        }
        [[nodiscard]] bool HasChildren(i32 nodeId) const override
        {
            return GetChildCount(nodeId) > 0;
        }
        [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
        {
            auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
            auto label = MakeRef<ui::Label>(DefaultAllocator());
            label->FontSize.SetValue(12.0f);
            auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            grow->Grow = 1.0f;
            row->AddView(label.Get(), grow);
            return RefPtr<ui::View>(row.Get());
        }
        void BindView(ui::View* view, i32 nodeId, i32 depth, bool) override
        {
            auto* row = Cast<ui::FlexLayout>(view);
            if (row == nullptr || row->ChildCount() == 0 || !InRange(nodeId))
            {
                return;
            }
            auto* label = Cast<ui::Label>(row->GetChildAt(0));
            label->SetText(Node(nodeId).label.AsView());
            // Left-pad past the chevron column; ContentInset tracks the tree's IndentWidth.
            row->Padding = ui::Thickness{m_owner->m_treeView->ContentInset(depth), 0, 0, 0};
        }

    private:
        [[nodiscard]] bool InRange(i32 nodeId) const
        {
            return nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_tree.nodes.Size());
        }
        [[nodiscard]] const ScriptApiTreeNode& Node(i32 nodeId) const
        {
            return m_owner->m_tree.nodes[static_cast<usize>(nodeId)];
        }

        ScriptApiBrowserView* m_owner;
    };

    ScriptApiBrowserView::ScriptApiBrowserView()
    {
        auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
        column->Direction = ui::Orientation::Vertical;
        column->Spacing = 4.0f;

        m_filter = MakeRef<ui::EditText>(DefaultAllocator());
        m_filter->SetPlaceholder(u8"Filter API");
        ScriptApiBrowserView* self = this;
        m_filter->OnTextChanged.Add([self](ui::EditText*) { self->m_dirty = true; });
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            column->AddView(m_filter.Get(), lp);
        }

        m_treeView = MakeRef<ui::TreeView>(DefaultAllocator());
        m_treeView->OnItemClick.Add(
            [self](ui::TreeView::ItemClickInfo info)
            {
                if (info.ClickCount < 2 || !self->OnInsert)
                {
                    return;
                }
                if (info.NodeId >= 0 &&
                    info.NodeId < static_cast<i32>(self->m_tree.nodes.Size()))
                {
                    self->OnInsert(
                        self->m_tree.nodes[static_cast<usize>(info.NodeId)].insertText.AsView());
                }
            });
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Grow = 1.0f;
            lp->Width = ui::SizeSpec::Match();
            column->AddView(m_treeView.Get(), lp);
        }

        m_adapter = UniquePtr<TreeAdapter>(DefaultAllocator().New<TreeAdapter>(*this),
                                           DefaultAllocator());
        m_root = column;
    }

    ScriptApiBrowserView::~ScriptApiBrowserView()
    {
        // The view must never outlive the adapter it borrows.
        m_treeView->SetAdapter(nullptr);
    }

    void ScriptApiBrowserView::Update()
    {
        // A hidden panel neither builds the surface nor the tree; opening it (the toggle
        // flips Visibility only) makes the next Update pay the one-time build.
        if (!m_dirty || m_root->Visibility != ui::Visibility::Visible ||
            m_surface == nullptr)
        {
            return;
        }
        m_dirty = false;
        Rebuild();
    }

    void ScriptApiBrowserView::Rebuild()
    {
        m_tree = BuildScriptApiTree(m_surface->Types(), m_filter->Text());
        m_treeView->SetAdapter(m_adapter.Get()); // rebuilds the flat list + rows
    }
}
