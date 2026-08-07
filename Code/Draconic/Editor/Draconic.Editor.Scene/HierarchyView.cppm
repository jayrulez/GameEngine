// Draconic::EditorScene - :hierarchy partition.
//
// SceneHierarchyView: the entity tree INSIDE a scene page (multi-scene rule - one per page,
// never a global panel; §3.6). A DraggableTreeView over a rebuilt snapshot of the live scene
// (Scene::Revision() gates the rebuild, so command execute/undo/redo all refresh it for free),
// wired to the page's SceneEditContext:
//   - click selects (per-page Guid selection, synced both ways with the tree's SelectionModel);
//   - right-click context menu: Create Child / Rename / Delete on rows, Create Entity on empty;
//   - rows are EditableLabels: double-click / slow-click renames in place (single clicks pass
//     through to selection by design), F2 / context-menu Rename triggers the same edit,
//     Delete deletes;
//   - drag a row INTO another = reparent; drag to a row EDGE = sibling reorder (insert-before
//     boundary, incl. top of the list and end-of-root-list below the last row);
//   - a header row holds [+] (create root entity - always reachable even when rows fill the
//     pane and swallow every right-click) and a filter box (matches keep their ancestors).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.scene:hierarchy;

import draconic.foundation;
import draconic.fonts;
import draconic.scene;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import :edit;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace ui = draconic::ui;
    namespace scene = draconic::scene;

    class SceneHierarchyView : public ui::ViewGroup
    {
        DRACONIC_OBJECT(SceneHierarchyView, ui::ViewGroup)
    public:
        /// Cross-page clipboard home (optional - Copy/Paste menu items appear when set).
        void SetEditorContext(EditorContext* context) noexcept { m_editor = context; }

        /// Prefab hooks (wired by the scene page - asset creation/picking lives there):
        /// turn an entity's subtree into a prefab asset + instance, and spawn an instance
        /// under `parent` (nil = scene root).
        Function<void(const Guid&)> OnCreatePrefab;
        Function<void(const Guid&)> OnSpawnPrefab;
        Function<void(const Guid&)> OnApplyPrefab;  // instance root -> write back to the asset
        Function<void(const Guid&)> OnRevertPrefab; // instance root -> discard deltas

        explicit SceneHierarchyView(SceneEditContext& edit) : m_edit(&edit)
        {
            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;

            // Header: [+] create root entity | filter box.
            auto header = MakeRef<ui::FlexLayout>(DefaultAllocator());
            header->Direction = ui::Orientation::Horizontal;
            header->Spacing = 4.0f;
            header->Padding = ui::Thickness{4, 3};
            auto addButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"+"));
            {
                SceneEditContext* editPtr = m_edit;
                addButton->OnClick.Add([editPtr](ui::ButtonBase*)
                                       { (void)editPtr->CreateEntity(u8"Entity"); });
                header->AddView(addButton.Get());
            }
            m_filterEdit = MakeRef<ui::EditText>(DefaultAllocator());
            m_filterEdit->SetPlaceholder(u8"Filter...");
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                header->AddView(m_filterEdit.Get(), grow);
            }
            column->AddView(header.Get());

            m_adapter = MakeUnique<Adapter>(DefaultAllocator(), *this);
            m_tree = MakeRef<ui::toolkit::DraggableTreeView>(DefaultAllocator());
            m_tree->SetItemHeight(22.0f);
            m_tree->SetAdapter(m_adapter.Get());
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                column->AddView(m_tree.Get(), grow);
            }

            AddView(column.Get());

            WireEvents();
        }

        /// Per-frame: rebuild the snapshot when the scene changed, keep selection in sync.
        void Refresh();

        /// Begin the in-place rename of an entity (F2 / context menu; double-click and
        /// slow-click on the row do the same via the EditableLabel itself).
        void BeginRename(const Guid& entity);

        [[nodiscard]] ui::toolkit::DraggableTreeView* Tree() const noexcept { return m_tree.Get(); }
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

        // A row IS an EditableLabel (depth-indented via TextOffsetX): double-click / slow-click
        // edits in place, single clicks deliberately pass through to the list's selection, and
        // Enter/Escape commit/cancel. The adapter created it, so static_cast recovery is safe.
        class Row final : public ui::EditableLabel
        {
        public:
            void Bind(const Guid& entity, StringView name, f32 textInset, bool prefabMember)
            {
                m_entity = entity;
                SetText(name);
                // Indent past the expander-chevron column; textInset comes from
                // TreeView::ContentInset(depth) so it tracks IndentWidth (never a drifting literal).
                TextOffsetX.SetValue(textInset);
                // Every prefab-instance member reads distinctly (the Unity-blue convention);
                // the text itself stays clean so in-place renames never absorb a marker.
                if (prefabMember)
                {
                    TextColor.SetValue(Color{0.45f, 0.72f, 1.0f, 1.0f});
                }
                else
                {
                    TextColor.SetValue(Optional<Color>{});
                }
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
                auto row = MakeRef<Row>(DefaultAllocator());
                row->FontSize.SetValue(
                    Optional<f32>{12.0f}); // match the inspector's dense 12px text
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
                static_cast<Row*>(view)->Bind(node.id, node.name.AsView(),
                                              m_owner->m_tree->ContentInset(depth), prefabMember);
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

        SceneEditContext* m_edit;          // borrowed (the page owns it)
        EditorContext* m_editor = nullptr; // borrowed; clipboard home (optional)
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

    DRACONIC_DEFINE_OBJECT(SceneHierarchyView, "draconic::editor")
}
