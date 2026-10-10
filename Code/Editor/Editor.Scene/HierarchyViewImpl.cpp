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

module editor.scene;

import foundation.core;
import foundation.fonts;
import foundation.scene;
import foundation.ui;
import foundation.ui.toolkit;
import editor.app; // AssetPickerSlot::EntityItemKind() (a dragged row names its entity)
import editor.core;
import :edit;
import :actions;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace ui = foundation::ui;

namespace editor
{
    bool SceneHierarchyView::CopyEntityId(const Guid& id)
    {
        ui::IClipboard* clipboard = Context != nullptr ? Context->Clipboard() : nullptr;
        if (clipboard == nullptr || id == Guid{})
        {
            return false;
        }
        const String text = Format(u8"{}", id);
        if (m_editor != nullptr)
        {
            return m_editor->CopyText(clipboard, text.AsView(), u8"entity ID"); // with its toast
        }
        return clipboard->SetText(text.AsView()).IsOk();
    }

    void SceneHierarchyView::Refresh()
    {
        if (m_edit->Scene().Revision() != m_revision)
        {
            m_revision = m_edit->Scene().Revision();
            RebuildSnapshot();
        }
    }

    void SceneHierarchyView::BeginRename(const Guid& entity)
    {
        ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter();
        if (flat == nullptr)
        {
            return;
        }
        for (i32 pos = 0; pos < flat->ItemCount(); ++pos)
        {
            if (GuidOfNode(flat->GetNodeId(pos)) == entity)
            {
                m_tree->InternalTreeView()->InternalListView()->ScrollToPosition(pos);
                if (auto* row = static_cast<Row*>(
                        m_tree->InternalTreeView()->InternalListView()->GetActiveView(pos)))
                {
                    row->BeginEdit();
                }
                return;
            }
        }
    }

    void SceneHierarchyView::OnMouseDown(ui::MouseEventArgs& e)
    {
        if (e.Button == ui::MouseButton::Right && Context != nullptr)
        {
            auto menu = MakeRef<ui::ContextMenu>(MemoryAllocator());
            if (m_actions != nullptr)
            {
                const StringView ids[] = {SceneActionIds::kEntityCreate,
                                          SceneActionIds::kEntitySpawnPrefab,
                                          SceneActionIds::kEntityPaste};
                (void)AppendActionItems(*menu, *m_actions, m_subject, Span<const StringView>(ids, 3));
            }
            const Float2 screenPos = LocalToScreen(Float2{e.X, e.Y});
            menu->Show(Context, screenPos.x, screenPos.y);
            e.Handled = true;
        }
    }

    void SceneHierarchyView::OnMeasure(ui::BoxConstraints constraints)
    {
        for (usize i = 0; i < ChildCount(); ++i)
        {
            GetChildAt(i)->Measure(constraints);
        }
        MeasuredSize = Float2{constraints.MaxWidth, constraints.MaxHeight};
    }

    void SceneHierarchyView::OnLayout(f32, f32, f32 width, f32 height)
    {
        for (usize i = 0; i < ChildCount(); ++i)
        {
            GetChildAt(i)->Layout(0, 0, width, height);
        }
    }

    void SceneHierarchyView::DecorateDrag(ui::toolkit::TreeDragData& data)
    {
        ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter();
        if (flat == nullptr || data.SourcePosition < 0 || data.SourcePosition >= flat->ItemCount())
        {
            return;
        }
        const Guid id = GuidOfNode(flat->GetNodeId(data.SourcePosition));
        if (id.IsNil())
        {
            return;
        }
        data.ItemKind = String(editor::app::AssetPickerSlot::EntityItemKind());
        data.ItemId = id;
        const scene::EntityHandle h = m_edit->Scene().FindEntity(id);
        if (h.IsAssigned())
        {
            data.ItemName = String(m_edit->Scene().GetEntityName(h));
        }
    }

    void SceneHierarchyView::WireEvents()
    {
        ui::TreeView* tree = m_tree->InternalTreeView();
        SceneHierarchyView* self = this;

        // A dragged row names its entity, so an inspector's entity slot can take it.
        m_tree->OnDecorateDragData = [self](ui::toolkit::TreeDragData& data)
        { self->DecorateDrag(data); };

        tree->OnItemClick.Add(
            [self](ui::TreeView::ItemClickInfo info)
            {
                if (self->m_syncing)
                {
                    return;
                }
                const Guid id = self->GuidOfNode(info.NodeId);
                if (id != Guid{})
                {
                    self->m_syncing = true;
                    self->m_edit->EntitySelection().Set(id);
                    self->m_syncing = false;
                    if (info.ClickCount >= 2 && self->OnFrameEntity)
                    {
                        self->OnFrameEntity(id);
                    }
                }
            });

        // Sync the inspector on ANY selection change - crucially keyboard nav (arrow keys move the
        // ListView selection without firing OnItemClick). Guarded by m_syncing so the scene->tree sync
        // (SyncSelectionToTree) does not loop back. Covers click too (harmless double-set, idempotent).
        m_tree->Selection().OnSelectionChanged.Add(
            [self]()
            {
                if (self->m_syncing)
                {
                    return;
                }
                const i32 pos = self->m_tree->Selection().FirstSelected();
                const Guid id = (pos >= 0) ? self->GuidAtFlat(pos) : Guid{};
                if (id != Guid{})
                {
                    self->m_syncing = true;
                    self->m_edit->EntitySelection().Set(id);
                    self->m_syncing = false;
                }
            });

        tree->OnItemRightClick.Add(
            [self](i32 nodeId, f32 x, f32 y)
            {
                const Guid id = self->GuidOfNode(nodeId);
                if (id == Guid{} || self->Context == nullptr)
                {
                    return;
                }
                self->m_edit->EntitySelection().Set(id);

                auto menu = MakeRef<ui::ContextMenu>(self->MemoryAllocator());
                // The scene editor's actions over the page (the right-click selected this
                // entity, so they act on it), with the view's own items between.
                if (self->m_actions != nullptr)
                {
                    const StringView head[] = {SceneActionIds::kEntityCreateChild};
                    (void)AppendActionItems(*menu, *self->m_actions, self->m_subject,
                                            Span<const StringView>(head, 1));
                }
                menu->AddItem(u8"Rename", [self, id]() { self->BeginRename(id); });
                menu->AddItem(u8"Copy ID", [self, id]() { (void)self->CopyEntityId(id); });
                if (self->m_actions != nullptr)
                {
                    menu->AddSeparator();
                    const StringView entity[] = {SceneActionIds::kEntityDuplicate,
                                                 SceneActionIds::kEntityCreatePrefab,
                                                 SceneActionIds::kEntitySpawnPrefabAsChild,
                                                 SceneActionIds::kEntitySpawnPrefab};
                    (void)AppendActionItems(*menu, *self->m_actions, self->m_subject,
                                            Span<const StringView>(entity, 4));
                    if (self->m_actions->IsEnabled(SceneActionIds::kPrefabApply, self->m_subject))
                    {
                        menu->AddSeparator();
                        const StringView prefab[] = {SceneActionIds::kPrefabApply,
                                                     SceneActionIds::kPrefabRevert};
                        (void)AppendActionItems(*menu, *self->m_actions, self->m_subject,
                                                Span<const StringView>(prefab, 2));
                    }
                    menu->AddSeparator();
                    const StringView clipboard[] = {SceneActionIds::kEntityCopy,
                                                    SceneActionIds::kEntityPasteAsChild};
                    (void)AppendActionItems(*menu, *self->m_actions, self->m_subject,
                                            Span<const StringView>(clipboard, 2));
                    menu->AddSeparator();
                    const StringView tail[] = {SceneActionIds::kEntityDelete};
                    (void)AppendActionItems(*menu, *self->m_actions, self->m_subject,
                                            Span<const StringView>(tail, 1));
                }
                const Float2 screenPos =
                    self->m_tree->InternalTreeView()->LocalToScreen(Float2{x, y});
                menu->Show(self->Context, screenPos.x, screenPos.y);
            });

        // Right-click on empty space below the rows: the ListView consumes ALL right-clicks
        // (its contract) and routes background ones here - the OnMouseDown fallback on this
        // view never fires while the tree fills the pane.
        tree->InternalListView()->OnBackgroundRightClicked.Add(
            [self](f32 x, f32 y)
            {
                if (self->Context == nullptr)
                {
                    return;
                }
                auto menu = MakeRef<ui::ContextMenu>(self->MemoryAllocator());
                if (self->m_actions != nullptr)
                {
                    const StringView ids[] = {SceneActionIds::kEntityCreate,
                                              SceneActionIds::kEntitySpawnPrefab,
                                              SceneActionIds::kEntityPaste};
                    (void)AppendActionItems(*menu, *self->m_actions, self->m_subject,
                                            Span<const StringView>(ids, 3));
                }
                const Float2 screenPos =
                    self->m_tree->InternalTreeView()->InternalListView()->LocalToScreen(
                        Float2{x, y});
                menu->Show(self->Context, screenPos.x, screenPos.y);
            });

        tree->OnItemKeyDown.Add(
            [self](i32 nodeId, ui::KeyEventArgs& e)
            {
                const Guid id = self->GuidOfNode(nodeId);
                if (id == Guid{})
                {
                    return;
                }
                if (e.Key == ui::KeyCode::F2)
                {
                    self->BeginRename(id);
                    e.Handled = true;
                }
                else if (e.Key == ui::KeyCode::Delete)
                {
                    self->m_edit->DestroyEntity(id);
                    e.Handled = true;
                }
            });

        m_filterEdit->OnTextChanged.Add(
            [self](ui::EditText* edit)
            {
                self->m_filter = String(edit->Text());
                self->RebuildSnapshot(); // filter changes rebuild regardless of revision
            });

        // Tree selection -> context selection is on click above; context -> tree here.
        m_edit->EntitySelection().OnChanged = [self]()
        {
            if (self->m_syncing)
            {
                return;
            }
            self->SyncSelectionToTree();
        };
    }

    bool SceneHierarchyView::MatchesFilter(StringView name, StringView filter)
    {
        if (filter.IsEmpty())
        {
            return true;
        }
        if (name.Size() < filter.Size())
        {
            return false;
        }
        auto lower = [](utf8char c)
        { return (c >= utf8char('A') && c <= utf8char('Z')) ? static_cast<utf8char>(c + 32) : c; };
        for (usize i = 0; i + filter.Size() <= name.Size(); ++i)
        {
            bool match = true;
            for (usize j = 0; j < filter.Size(); ++j)
            {
                if (lower(name[i + j]) != lower(filter[j]))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return true;
            }
        }
        return false;
    }

    bool SceneHierarchyView::SubtreeMatches(scene::Scene& scene, scene::EntityHandle e) const
    {
        if (MatchesFilter(scene.GetEntityName(e), m_filter.AsView()))
        {
            return true;
        }
        for (scene::EntityHandle c = scene.GetFirstChild(e); c.IsAssigned();
             c = scene.GetNextSibling(c))
        {
            if (SubtreeMatches(scene, c))
            {
                return true;
            }
        }
        return false;
    }

    void SceneHierarchyView::CaptureCollapseState()
    {
        ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter();
        if (flat == nullptr)
        {
            return;
        }
        for (usize i = 0; i < m_nodes.Size(); ++i)
        {
            if (m_nodes[i].children.IsEmpty())
            {
                continue;
            }
            if (flat->IsExpanded(static_cast<i32>(i)))
            {
                m_collapsed.Remove(m_nodes[i].id);
            }
            else
            {
                m_collapsed.Insert(m_nodes[i].id);
            }
        }
    }

    void SceneHierarchyView::RebuildSnapshot()
    {
        CaptureCollapseState();
        m_nodes.Clear();
        m_roots.Clear();
        scene::Scene& scene = m_edit->Scene();

        // Roots in LIST order (the order reorder edits maintain and serialization
        // preserves), then depth-first children. With a filter, keep nodes whose subtree
        // contains a match.
        for (scene::EntityHandle r = scene.GetFirstRoot(); r.IsAssigned();
             r = scene.GetNextSibling(r))
        {
            if (SubtreeMatches(scene, r))
            {
                m_roots.PushBack(AddNode(scene, r, 0));
            }
        }

        // Rebuild the flat view (SetAdapter recreates the flattened tree). New entities
        // default to expanded so structural edits stay visible; entities the user collapsed
        // stay collapsed (state captured above, keyed by Guid).
        m_tree->SetAdapter(m_adapter.Get());
        ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter();
        for (usize i = 0; i < m_nodes.Size(); ++i)
        {
            if (m_nodes[i].children.IsEmpty())
            {
                continue;
            }
            if (!m_collapsed.Contains(m_nodes[i].id))
            {
                flat->Expand(static_cast<i32>(i));
            }
        }
        m_tree->InternalTreeView()->InternalListView()->NotifyDataChanged();
        SyncSelectionToTree();
    }

    i32 SceneHierarchyView::AddNode(scene::Scene& scene, scene::EntityHandle e, i32 depth)
    {
        const i32 nodeId = static_cast<i32>(m_nodes.Size());
        Node node;
        node.id = scene.GetEntityId(e);
        node.name = String(scene.GetEntityName(e));
        if (node.name.IsEmpty())
        {
            node.name = String(u8"(unnamed)");
        }
        node.depth = depth;
        m_nodes.PushBack(Move(node));

        for (scene::EntityHandle c = scene.GetFirstChild(e); c.IsAssigned();
             c = scene.GetNextSibling(c))
        {
            if (!SubtreeMatches(scene, c))
            {
                continue;
            }
            const i32 child = AddNode(scene, c, depth + 1);
            m_nodes[static_cast<usize>(nodeId)].children.PushBack(child);
        }
        return nodeId;
    }

    Guid SceneHierarchyView::GuidOfNode(i32 nodeId) const
    {
        return (nodeId >= 0 && nodeId < static_cast<i32>(m_nodes.Size()))
                   ? m_nodes[static_cast<usize>(nodeId)].id
                   : Guid{};
    }

    Guid SceneHierarchyView::GuidAtFlat(i32 flatPosition) const
    {
        ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter();
        return (flat != nullptr) ? GuidOfNode(flat->GetNodeId(flatPosition)) : Guid{};
    }

    i32 SceneHierarchyView::FlatCount() const
    {
        ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter();
        return (flat != nullptr) ? flat->ItemCount() : 0;
    }

    void SceneHierarchyView::SyncSelectionToTree()
    {
        const Guid* primary = m_edit->EntitySelection().Primary();
        ui::SelectionModel& sel = m_tree->Selection();
        m_syncing = true;
        if (primary == nullptr)
        {
            sel.ClearSelection();
        }
        else if (ui::FlattenedTreeAdapter* flat = m_tree->InternalTreeView()->FlatAdapter())
        {
            for (i32 pos = 0; pos < flat->ItemCount(); ++pos)
            {
                if (GuidOfNode(flat->GetNodeId(pos)) == *primary)
                {
                    sel.Select(pos);
                    break;
                }
            }
        }
        m_syncing = false;
    }
}
