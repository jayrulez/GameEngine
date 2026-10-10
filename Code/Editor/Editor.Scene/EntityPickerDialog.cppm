// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :entity_picker_dialog partition.
//
// EntityPickerDialog: a modal entity picker for EntityRef component fields - the entity twin of
// editor.app's AssetPickerDialog. Shows the scene as a filterable TREE (same shape as the hierarchy
// view: a Node snapshot over ui::TreeView, so expand/collapse and the "matches keep their ancestors"
// filter come from the shared tree machinery - nothing reinvented). Replaces the flat context menu,
// which is unusable in scenes with many entities.
//
//   - the tree mirrors the scene hierarchy; the filter box prunes to matches + their ancestors
//   - single-click selects, double-click or [Select] confirms, [Clear] picks none, [Cancel] dismisses
//   - the result is delivered through OnPicked(guid) (nil = cleared), fired BEFORE the dialog closes

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:entity_picker_dialog;

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.scene;
import editor.core; // NameOrGuidMatches, FilterAsGuid (the editor's filter fields)

using namespace foundation::core;

export namespace editor
{
    namespace ui = foundation::ui;
    namespace scene = foundation::scene;

    class EntityPickerDialog final : public ui::Dialog
    {
        RTTI_OBJECT(EntityPickerDialog, ui::Dialog)
    public:
        /// The pick result: an entity guid, or nil for [Clear]. Fired once, before close.
        Function<void(const Guid&)> OnPicked;

        EntityPickerDialog(scene::Scene& sceneRef, const Guid& current)
            : ui::Dialog(u8"Select entity"), m_scene(&sceneRef), m_current(current)
        {
            MinWidth.SetValue(360.0f);
            MinHeight.SetValue(380.0f);
            MaxWidth.SetValue(520.0f);
            MaxHeight.SetValue(560.0f);

            auto column = MakeRef<ui::FlexLayout>(MemoryAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6;

            m_filterEdit = MakeRef<ui::EditText>(MemoryAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter...");
            {
                EntityPickerDialog* self = this;
                m_filterEdit->OnTextChanged.Add(
                    [self](ui::EditText* edit)
                    {
                        self->m_filter = String(edit->Text());
                        self->RebuildTree();
                    });
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                column->AddView(m_filterEdit.Get(), lp);
            }

            m_adapter = MakeUnique<Adapter>(MemoryAllocator(), *this);
            m_tree = MakeRef<ui::TreeView>(MemoryAllocator());
            m_tree->SetItemHeight(20.0f);
            m_tree->SetAdapter(m_adapter.Get());
            {
                EntityPickerDialog* self = this;
                m_tree->OnItemClick.Add(
                    [self](ui::TreeView::ItemClickInfo info)
                    {
                        if (info.NodeId >= 0 && info.NodeId < static_cast<i32>(self->m_nodes.Size()))
                        {
                            self->m_selected = info.NodeId;
                            if (info.ClickCount >= 2)
                            {
                                self->Confirm(self->m_nodes[static_cast<usize>(info.NodeId)].id);
                            }
                        }
                    });
                ui::LayoutStyle lp;
                lp.Width = ui::SizeSpec::Match();
                lp.FlexGrow = 1.0f;
                column->AddView(m_tree.Get(), lp);
            }
            SetContent(column.Get());

            {
                EntityPickerDialog* self = this;
                ui::Button* select = AddButton(u8"Select", ui::DialogResult::None);
                select->OnClick.Add(
                    [self](ui::ButtonBase*)
                    {
                        if (self->m_selected >= 0 &&
                            self->m_selected < static_cast<i32>(self->m_nodes.Size()))
                        {
                            self->Confirm(self->m_nodes[static_cast<usize>(self->m_selected)].id);
                        }
                    });
                ui::Button* clear = AddButton(u8"Clear", ui::DialogResult::None);
                clear->OnClick.Add([self](ui::ButtonBase*) { self->Confirm(Guid{}); });
                AddButton(u8"Cancel", ui::DialogResult::Cancel);
            }

            RebuildTree();
        }

        ~EntityPickerDialog() override { m_tree->SetAdapter(nullptr); }

    private:
        struct Node
        {
            Guid id;
            String name;
            i32 depth = 0;
            Array<i32> children;
        };

        // Read-only tree adapter over the Node snapshot (same shape as the hierarchy view's, minus
        // the reorder/drag surface). Row = a plain indented Label.
        class Adapter final : public ui::ITreeAdapter
        {
        public:
            explicit Adapter(EntityPickerDialog& owner) : m_owner(&owner) {}
            [[nodiscard]] i32 RootCount() const override
            {
                return static_cast<i32>(m_owner->m_roots.Size());
            }
            [[nodiscard]] i32 GetChildCount(i32 nodeId) const override
            {
                if (nodeId == -1)
                {
                    return RootCount();
                }
                return InRange(nodeId)
                           ? static_cast<i32>(m_owner->m_nodes[static_cast<usize>(nodeId)].children.Size())
                           : 0;
            }
            [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override
            {
                if (parentId == -1)
                {
                    return (childIndex >= 0 && childIndex < RootCount())
                               ? m_owner->m_roots[static_cast<usize>(childIndex)]
                               : -1;
                }
                if (!InRange(parentId))
                {
                    return -1;
                }
                const Array<i32>& kids = m_owner->m_nodes[static_cast<usize>(parentId)].children;
                return (childIndex >= 0 && childIndex < static_cast<i32>(kids.Size()))
                           ? kids[static_cast<usize>(childIndex)]
                           : -1;
            }
            [[nodiscard]] i32 GetDepth(i32 nodeId) const override
            {
                return InRange(nodeId) ? m_owner->m_nodes[static_cast<usize>(nodeId)].depth : 0;
            }
            [[nodiscard]] bool HasChildren(i32 nodeId) const override
            {
                return GetChildCount(nodeId) > 0;
            }
            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto row = MakeRef<ui::FlexLayout>(foundation::core::DefaultAllocator());
                auto label = MakeRef<ui::Label>(foundation::core::DefaultAllocator());
                label->FontSize.SetValue(Optional<f32>{12.0f});
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
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
                if (label == nullptr)
                {
                    return;
                }
                label->SetText(m_owner->m_nodes[static_cast<usize>(nodeId)].name.AsView());
                // Indent past the expander chevron; ContentInset(depth) tracks the tree's IndentWidth.
                row->Padding = ui::Thickness{m_owner->m_tree->ContentInset(depth), 0, 0, 0};
            }

        private:
            [[nodiscard]] bool InRange(i32 nodeId) const
            {
                return nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_nodes.Size());
            }
            EntityPickerDialog* m_owner;
        };

        void Confirm(const Guid& id)
        {
            if (OnPicked)
            {
                OnPicked(id);
            }
            Close(ui::DialogResult::OK);
        }

        // True if the entity or ANY descendant matches (so ancestors of a match stay visible).
        [[nodiscard]] bool SubtreeMatches(scene::EntityHandle e) const
        {
            if (NameOrGuidMatches(m_scene->GetEntityName(e), m_scene->GetEntityId(e), m_filter.AsView()))
            {
                return true;
            }
            for (scene::EntityHandle c = m_scene->GetFirstChild(e); c.IsAssigned();
                 c = m_scene->GetNextSibling(c))
            {
                if (SubtreeMatches(c))
                {
                    return true;
                }
            }
            return false;
        }

        i32 AddNode(scene::EntityHandle e, i32 depth)
        {
            Node node;
            node.id = m_scene->GetEntityId(e);
            node.name = String(m_scene->GetEntityName(e));
            node.depth = depth;
            const i32 nodeId = static_cast<i32>(m_nodes.Size());
            m_nodes.PushBack(static_cast<Node&&>(node));
            for (scene::EntityHandle c = m_scene->GetFirstChild(e); c.IsAssigned();
                 c = m_scene->GetNextSibling(c))
            {
                if (!SubtreeMatches(c))
                {
                    continue;
                }
                const i32 child = AddNode(c, depth + 1); // index stays valid across the push
                m_nodes[static_cast<usize>(nodeId)].children.PushBack(child);
            }
            return nodeId;
        }

        void RebuildTree()
        {
            m_nodes.Clear();
            m_roots.Clear();
            m_selected = -1;
            for (scene::EntityHandle r = m_scene->GetFirstRoot(); r.IsAssigned();
                 r = m_scene->GetNextSibling(r))
            {
                if (SubtreeMatches(r))
                {
                    m_roots.PushBack(AddNode(r, 0));
                }
            }
            // Pre-select the entity a whole guid names, else the current target, if it survived the
            // filter.
            Guid wanted = m_current;
            Guid typed;
            if (FilterAsGuid(m_filter.AsView(), typed))
            {
                wanted = typed;
            }
            for (usize i = 0; i < m_nodes.Size(); ++i)
            {
                if (m_nodes[i].id == wanted)
                {
                    m_selected = static_cast<i32>(i);
                    break;
                }
            }
            // Default everything expanded (like the hierarchy view) so the structure is visible; the
            // FlattenedTreeAdapter under the tree owns the actual expand/collapse state. Expand() only
            // marks parents (it no-ops on leaves), so a final RebuildVisibleList() is required to walk
            // the now-populated source into the flat list - without it a flat (all-leaf) scene, and
            // the initial build (adapter was set while the node list was still empty), show nothing.
            if (ui::FlattenedTreeAdapter* flat = m_tree->FlatAdapter())
            {
                for (usize i = 0; i < m_nodes.Size(); ++i)
                {
                    flat->Expand(static_cast<i32>(i)); // parents only
                }
                flat->RebuildVisibleList(); // repopulate from the source + notify the list
                // Show the pre-selection as selected, scrolled to (Select confirms it).
                for (i32 pos = 0; pos < flat->ItemCount() && m_selected >= 0; ++pos)
                {
                    if (flat->GetNodeId(pos) == m_selected)
                    {
                        m_tree->Selection().Select(pos);
                        m_tree->InternalListView()->ScrollToPosition(pos);
                        break;
                    }
                }
            }
        }

    public:
        /// For tests: the filter field, and the entity Select would confirm (nil: none).
        [[nodiscard]] ui::EditText& FilterField() const noexcept { return *m_filterEdit; }
        [[nodiscard]] Guid SelectedEntity() const
        {
            return (m_selected >= 0 && m_selected < static_cast<i32>(m_nodes.Size()))
                       ? m_nodes[static_cast<usize>(m_selected)].id
                       : Guid{};
        }
        [[nodiscard]] usize ShownCount() const noexcept { return m_nodes.Size(); }

    private:

        scene::Scene* m_scene; // borrowed
        Guid m_current;
        RefPtr<ui::TreeView> m_tree;
        RefPtr<ui::EditText> m_filterEdit;
        UniquePtr<Adapter> m_adapter;
        Array<Node> m_nodes;
        Array<i32> m_roots;
        i32 m_selected = -1;
        String m_filter;
    };

    RTTI_DEFINE_OBJECT(EntityPickerDialog, "rtti::editor::entity_picker_dialog")
}
