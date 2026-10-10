// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :hierarchy partition.
//
// SceneHierarchyView: the entity tree INSIDE a scene page (multi-scene rule - one per page,
// never a global panel). A DraggableTreeView over a rebuilt snapshot of the live scene
// (Scene::Revision() gates the rebuild, so command execute/undo/redo all refresh it for free),
// wired to the page's SceneEditContext:
//   - click selects (per-page Guid selection, synced both ways with the tree's SelectionModel);
//   - right-click context menu: Create Child / Rename / Delete on rows, Create Entity on empty;
//   - rows are EditableLabels: a slow second click renames in place, a double-click frames the
//     entity in the viewport (OnFrameEntity) (single clicks pass
//     through to selection by design), F2 / context-menu Rename triggers the same edit,
//     Delete deletes;
//   - drag a row INTO another = reparent; drag to a row EDGE = sibling reorder (insert-before
//     boundary, incl. top of the list and end-of-root-list below the last row);
//   - a header row holds [+] (create root entity - always reachable even when rows fill the
//     pane and swallow every right-click) and a filter box (matches keep their ancestors).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:hierarchy;

import foundation.core;
import foundation.fonts;
import foundation.scene;
import foundation.ui;
import foundation.ui.toolkit;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    namespace ui = foundation::ui;
    namespace scene = foundation::scene;

    class SceneHierarchyView : public ui::ViewGroup
    {
        RTTI_OBJECT(SceneHierarchyView, ui::ViewGroup)
    public:
        /// Cross-page clipboard home (optional - Copy/Paste menu items appear when set).
        void SetEditorContext(EditorContext* context) noexcept { m_editor = context; }

        /// The "Copy ID" context-menu action: puts the entity's persistent guid (the id the
        /// scene file, prefab deltas and scripts name it by) on the OS text clipboard.
        /// False when no UI context / clipboard is attached (a bare view in a test).
        bool CopyEntityId(const Guid& id);

        /// The context menus are the scene editor's actions (create, duplicate, delete, the
        /// prefab flows, the clipboard) over `subject`, the page this hierarchy belongs to;
        /// without a registry the menus carry only the view's own items (rename, copy id).
        void SetActions(EditorActionRegistry* actions, EditorPage* subject) noexcept
        {
            m_actions = actions;
            m_subject = subject;
        }

        explicit SceneHierarchyView(SceneEditContext& edit) : m_edit(&edit)
        {
            // Inset like the other page trees, so a selected row's highlight stops short of the
            // panel's edges.
            auto column = MakeRef<ui::FlexLayout>(MemoryAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 4.0f;
            column->Padding = ui::Thickness{6, 4};

            // Header: [+] create root entity | filter box.
            auto header = MakeRef<ui::FlexLayout>(MemoryAllocator());
            header->Direction = ui::Orientation::Horizontal;
            header->Spacing = 4.0f;
            auto addButton = MakeRef<ui::Button>(MemoryAllocator(), StringView(u8"+"));
            {
                SceneEditContext* editPtr = m_edit;
                addButton->OnClick.Add([editPtr](ui::ButtonBase*)
                                       { (void)editPtr->CreateEntity(u8"Entity"); });
                header->AddView(addButton.Get());
            }
            m_filterEdit = MakeRef<ui::EditText>(MemoryAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter...");
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                header->AddView(m_filterEdit.Get(), grow);
            }
            column->AddView(header.Get());

            m_adapter = MakeUnique<Adapter>(MemoryAllocator(), *this);
            m_tree = MakeRef<ui::toolkit::DraggableTreeView>(MemoryAllocator());
            m_tree->SetItemHeight(22.0f);
            m_tree->SetAdapter(m_adapter.Get());
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                column->AddView(m_tree.Get(), grow);
            }

            AddView(column.Get());

            WireEvents();
        }

        /// Per-frame: rebuild the snapshot when the scene changed, keep selection in sync.
        void Refresh();

        /// Begin the in-place rename of an entity (F2 / context menu; a slow second click on the
        /// row does the same via the EditableLabel itself).
        void BeginRename(const Guid& entity);

        /// A double-click on a row: the page frames its entity in the viewport.
        Function<void(const Guid&)> OnFrameEntity;

        [[nodiscard]] ui::toolkit::DraggableTreeView* Tree() const noexcept { return m_tree.Get(); }

        /// Names the entity a drag of flat row `data.SourcePosition` carries.
        void DecorateDrag(ui::toolkit::TreeDragData& data);
        [[nodiscard]] usize NodeCount() const noexcept { return m_nodes.Size(); }

        // Right-click on empty space (below the rows): create a root entity.
        void OnMouseDown(ui::MouseEventArgs& e) override;

        // Fill the available space (wrap-to-children would fight the virtualized tree).
        void OnMeasure(ui::BoxConstraints constraints) override;
        void OnLayout(f32, f32, f32 width, f32 height) override;

    private:
        struct Node
        {
            Guid id;
            String name;
            i32 depth = 0;
            Array<i32> children;
        };

        // A row IS an EditableLabel (depth-indented via TextOffsetX): a slow second click edits
        // in place (a double-click frames the entity instead), single clicks deliberately pass through to the list's selection, and
        // Enter/Escape commit/cancel. The adapter created it, so static_cast recovery is safe.
        class Row final : public ui::EditableLabel
        {
        public:
            void Bind(const Guid& entity, StringView name, f32 textInset, bool prefabMember,
                      bool effectivelyActive)
            {
                m_entity = entity;
                SetText(name);
                // Indent past the expander-chevron column; textInset comes from
                // TreeView::ContentInset(depth) so it tracks IndentWidth (never a drifting literal).
                TextOffsetX.SetValue(textInset);
                // Every prefab-instance member reads distinctly (the Unity-blue convention);
                // the text itself stays clean so in-place renames never absorb a marker.
                // Effectively-inactive entities DIM - alpha over
                // whichever color the row would otherwise have, so prefab-blue dims too.
                Optional<Color> color;
                if (prefabMember)
                {
                    color = Color{0.45f, 0.72f, 1.0f, 1.0f};
                }
                if (!effectivelyActive)
                {
                    Color base = color.HasValue() ? color.Value() : Color{1.0f, 1.0f, 1.0f, 1.0f};
                    base.a = 0.45f;
                    color = base;
                }
                TextColor.SetValue(color);
            }
            [[nodiscard]] const Guid& Entity() const noexcept { return m_entity; }

        private:
            Guid m_entity;
        };

        class Adapter final : public ui::toolkit::IReorderableTreeAdapter
        {
        public:
            explicit Adapter(SceneHierarchyView& owner) : m_owner(&owner) {}

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
                           ? static_cast<i32>(
                                 m_owner->m_nodes[static_cast<usize>(nodeId)].children.Size())
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
                auto row = MakeRef<Row>(editor::EditorRootAllocator());
                row->FontSize.SetValue(
                    Optional<f32>{12.0f}); // match the inspector's dense 12px text
                row->DoubleClickToEdit.SetValue(false); // a double-click frames the entity
                SceneEditContext* edit = m_owner->m_edit;
                Row* raw = row.Get();
                row->OnRenameCommitted.Add([edit, raw](ui::EditableLabel*, StringView newName)
                                           { edit->RenameEntity(raw->Entity(), newName); });
                return RefPtr<ui::View>(row.Get());
            }
            void BindView(ui::View* view, i32 nodeId, i32 depth, bool) override
            {
                if (!InRange(nodeId))
                {
                    return;
                }
                const Node& node = m_owner->m_nodes[static_cast<usize>(nodeId)];
                scene::PrefabMemberInfo member;
                const bool prefabMember =
                    scene::FindPrefabMember(m_owner->m_edit->Scene(), node.id, member);
                scene::Scene& sceneRef = m_owner->m_edit->Scene();
                const bool effectivelyActive =
                    sceneRef.IsEffectivelyActive(sceneRef.FindEntity(node.id));
                static_cast<Row*>(view)->Bind(node.id, node.name.AsView(),
                                              m_owner->m_tree->ContentInset(depth), prefabMember,
                                              effectivelyActive);
            }

            // Between-rows reorder: `toPosition` is the insert-before BOUNDARY (0..count;
            // count = end of the root list). Maps to a MoveEntityBefore command.
            [[nodiscard]] bool CanMove(i32 fromPosition, i32 toPosition) override
            {
                const Guid from = m_owner->GuidAtFlat(fromPosition);
                if (from == Guid{})
                {
                    return false;
                }
                if (toPosition >= m_owner->FlatCount())
                {
                    return true;
                } // end of root list
                const Guid before = m_owner->GuidAtFlat(toPosition);
                if (before == Guid{} || before == from)
                {
                    return false;
                }
                // Cycle: the slot's parent lies inside the moved entity's subtree.
                const scene::EntityHandle parent =
                    m_owner->m_edit->Scene().GetParent(m_owner->m_edit->Resolve(before));
                if (parent.IsAssigned() && m_owner->m_edit->IsSelfOrAncestor(
                                               m_owner->m_edit->Scene().GetEntityId(parent), from))
                {
                    return false;
                }
                return true;
            }
            void MoveItem(i32 fromPosition, i32 toPosition) override
            {
                const Guid from = m_owner->GuidAtFlat(fromPosition);
                if (from == Guid{})
                {
                    return;
                }
                const Guid before =
                    (toPosition < m_owner->FlatCount()) ? m_owner->GuidAtFlat(toPosition) : Guid{};
                m_owner->m_edit->MoveEntityBefore(from, before);
            }

            // Drop-INTO reparents (cycle-guarded here for the hover feedback; the command
            // re-checks on execute).
            [[nodiscard]] bool CanDropInto(i32 fromPosition, i32 toPosition) override
            {
                const Guid from = m_owner->GuidAtFlat(fromPosition);
                const Guid to = m_owner->GuidAtFlat(toPosition);
                if (from == Guid{} || to == Guid{} || from == to)
                {
                    return false;
                }
                return !m_owner->m_edit->IsSelfOrAncestor(to, from); // target under source = cycle
            }
            void DropInto(i32 fromPosition, i32 toPosition) override
            {
                const Guid from = m_owner->GuidAtFlat(fromPosition);
                const Guid to = m_owner->GuidAtFlat(toPosition);
                if (from != Guid{} && to != Guid{})
                {
                    m_owner->m_edit->ReparentEntity(from, to);
                }
            }

        private:
            [[nodiscard]] bool InRange(i32 nodeId) const
            {
                return nodeId >= 0 && nodeId < static_cast<i32>(m_owner->m_nodes.Size());
            }
            SceneHierarchyView* m_owner;
        };

        void WireEvents();

        // ASCII-case-insensitive substring match (v1 filter; UTF-8 folding later if needed).
        [[nodiscard]] static bool MatchesFilter(StringView name, StringView filter);

        // True if the entity or ANY descendant matches (so ancestors of matches stay visible).
        [[nodiscard]] bool SubtreeMatches(scene::Scene& scene, scene::EntityHandle e) const;

        // Collapse state is keyed by entity Guid so it survives rebuilds: before the snapshot
        // is thrown away, fold the current expand state into m_collapsed (entities absent from
        // the snapshot - e.g. filtered out - keep their remembered state).
        void CaptureCollapseState();

        void RebuildSnapshot();

        i32 AddNode(scene::Scene& scene, scene::EntityHandle e, i32 depth);

        [[nodiscard]] Guid GuidOfNode(i32 nodeId) const;

        [[nodiscard]] Guid GuidAtFlat(i32 flatPosition) const;

        [[nodiscard]] i32 FlatCount() const;

        void SyncSelectionToTree();

        SceneEditContext* m_edit;

        EditorActionRegistry* m_actions = nullptr; // borrowed; the menus' actions

        EditorPage* m_subject = nullptr;           // borrowed; the page they run over          // borrowed (the page owns it)
        EditorContext* m_editor = nullptr; // borrowed; clipboard home and copy toasts (optional)
        RefPtr<ui::toolkit::DraggableTreeView> m_tree;
        RefPtr<ui::EditText> m_filterEdit;
        UniquePtr<Adapter> m_adapter;
        Array<Node> m_nodes; // pre-order snapshot of the scene (nodeId = index)
        Array<i32> m_roots;
        HashSet<Guid> m_collapsed; // entities the user collapsed (survives rebuilds)
        u64 m_revision = ~0ull;
        String m_filter;
        bool m_syncing = false;
    };

    RTTI_DEFINE_OBJECT(SceneHierarchyView, "rtti::editor::editor")
}
