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

module draconic.editor.scene;

import draconic.foundation;
import draconic.fonts;
import draconic.scene;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import :edit;

using namespace draconic::foundation;

namespace draconic::editor
{
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
            auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
            SceneEditContext* edit = m_edit;
            SceneHierarchyView* self = this;
            menu->AddItem(u8"Create Entity", [edit]() { (void)edit->CreateEntity(u8"Entity"); });
            menu->AddItem(u8"Spawn Prefab...",
                          [self]()
                          {
                              if (self->OnSpawnPrefab)
                              {
                                  self->OnSpawnPrefab(Guid{});
                              }
                          });
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

    void SceneHierarchyView::WireEvents()
    {
        ui::TreeView* tree = m_tree->InternalTreeView();
        SceneHierarchyView* self = this;

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

                SceneEditContext* edit = self->m_edit;
                auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
                menu->AddItem(u8"Create Child",
                              [edit, id]() { (void)edit->CreateEntity(u8"Entity", id); });
                menu->AddItem(u8"Rename", [self, id]() { self->BeginRename(id); });
                menu->AddSeparator();
                menu->AddItem(u8"Duplicate", [edit, id]() { (void)edit->DuplicateEntity(id); });
                menu->AddItem(u8"Create Prefab from Selection",
                              [self, id]()
                              {
                                  if (self->OnCreatePrefab)
                                  {
                                      self->OnCreatePrefab(id);
                                  }
                              });
                menu->AddItem(u8"Spawn Prefab as Child",
                              [self, id]()
                              {
                                  if (self->OnSpawnPrefab)
                                  {
                                      self->OnSpawnPrefab(id);
                                  }
                              });
                menu->AddItem(u8"Spawn Prefab at Root",
                              [self]()
                              {
                                  if (self->OnSpawnPrefab)
                                  {
                                      self->OnSpawnPrefab(Guid{});
                                  }
                              });
                scene::PrefabMemberInfo member;
                if (scene::FindPrefabMember(edit->Scene(), id, member))
                {
                    // Reachable from ANY member, acting on the whole owning instance.
                    const Guid rootId = member.state->rootEntityId;
                    menu->AddSeparator();
                    menu->AddItem(u8"Apply to Prefab",
                                  [self, rootId]()
                                  {
                                      if (self->OnApplyPrefab)
                                      {
                                          self->OnApplyPrefab(rootId);
                                      }
                                  });
                    menu->AddItem(u8"Revert Instance",
                                  [self, rootId]()
                                  {
                                      if (self->OnRevertPrefab)
                                      {
                                          self->OnRevertPrefab(rootId);
                                      }
                                  });
                }
                if (EditorContext* editor = self->m_editor)
                {
                    menu->AddItem(u8"Copy",
                                  [edit, editor, id]()
                                  {
                                      Array<byte> blob = edit->CopyEntity(id);
                                      if (!blob.IsEmpty())
                                      {
                                          editor->SetClipboard(u8"entities", Move(blob));
                                      }
                                  });
                    const Span<const byte> clip = editor->ClipboardData(u8"entities");
                    menu->AddItem(
                        u8"Paste as Child", [edit, editor, id]()
                        { (void)edit->PasteEntities(editor->ClipboardData(u8"entities"), id); },
                        !clip.IsEmpty());
                }
                menu->AddSeparator();
                menu->AddItem(u8"Delete", [edit, id]() { edit->DestroyEntity(id); });
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
                SceneEditContext* edit = self->m_edit;
                auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
                menu->AddItem(u8"Create Entity",
                              [edit]() { (void)edit->CreateEntity(u8"Entity"); });
                menu->AddItem(u8"Spawn Prefab...",
                              [self]()
                              {
                                  if (self->OnSpawnPrefab)
                                  {
                                      self->OnSpawnPrefab(Guid{});
                                  }
                              });
                if (EditorContext* editor = self->m_editor)
                {
                    const Span<const byte> clip = editor->ClipboardData(u8"entities");
                    menu->AddItem(
                        u8"Paste", [edit, editor]()
                        { (void)edit->PasteEntities(editor->ClipboardData(u8"entities")); },
                        !clip.IsEmpty());
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
