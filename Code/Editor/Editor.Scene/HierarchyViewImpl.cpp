// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :hierarchy partition, its body (Documentation/Specs/editor-gui.md, 5.3,
// layer 3). See HierarchyView.cppm.

module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.scene.resource;
import foundation.gui;
import editor.core;
import editor.app;
import :edit;
import :actions;
import :scene_tree;
import :hierarchy;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        constexpr gui::ItemId kSceneRowId = SceneTreeModel::kSceneRowId;

        template <typename T>
        T* FindIn(gui::Node& node, StringView id)
        {
            if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(&node);
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (T* found = FindIn<T>(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }
    }

    SceneHierarchyView::SceneHierarchyView(IAllocator& allocator, SceneEditContext& edit, Services services, gui::Node& pane)
        : m_allocator(allocator), m_edit(&edit), m_services(Move(services)), m_model(*this, allocator), m_known(allocator)
    {
        SceneHierarchyView* self = this;
        m_tree = FindIn<gui::TreeView>(pane, u8"hierarchy-tree");
        m_search = FindIn<gui::TextField>(pane, u8"hierarchy-search");
        m_clear = FindIn<gui::Button>(pane, u8"hierarchy-clear");
        if (m_services.context != nullptr && m_services.page != nullptr)
        {
            m_bar = MakeUnique<app::PageActionBar>(m_allocator, m_allocator, m_services.context->Actions(), *m_services.page);
            m_bar->Bind(FindIn<gui::ButtonBase>(pane, u8"hierarchy-add"), SceneActionIds::kEntityCreate);
        }
        if (m_search != nullptr)
            m_search->SetOnTextChanging([self](StringView text) { self->Search(text); });
        if (m_clear != nullptr)
        {
            m_clear->SetVisible(false);
            m_clear->SetOnClick([self]()
                                {
                                    if (self->m_search != nullptr)
                                        self->m_search->SetText(StringView{});
                                    self->Search(StringView{});
                                });
        }
        if (m_tree != nullptr)
        {
            m_tree->SetSelectionMode(gui::SelectionMode::Multi);
            m_tree->SetModel(&m_model);
            m_tree->SetOnSelectionChanged([self](gui::ModelIndex) { self->SyncSelectionFromTree(); });
            m_tree->SetOnActivated([self](gui::ModelIndex index)
                                   {
                                       const Guid entity = self->m_model.EntityOfNode(index.InternalId);
                                       if (!entity.IsNil() && self->m_services.frame)
                                           self->m_services.frame(entity);
                                   });
            m_tree->SetDragType(String(EntityDrag::kType));
            m_tree->SetOnDragPayload([self](gui::DragPayload& payload, Span<const gui::ItemId> ids)
                                     {
                                         Array<Guid> entities(self->m_allocator);
                                         for (const gui::ItemId id : ids)
                                             if (const Guid entity = self->m_model.EntityOfId(id); !entity.IsNil())
                                                 entities.PushBack(entity);
                                         if (!entities.IsEmpty())
                                             payload = self->DragOf(Span<const Guid>(entities.Data(), entities.Size()));
                                     });
            (void)m_tree->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                           {
                                               const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                               self->ShowMenu(self->m_tree->ItemAt(at), at);
                                               e.Handled = true;
                                           });
            (void)m_tree->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                           {
                                               const auto& key = static_cast<const gui::KeyEvent&>(e);
                                               if (static_cast<gui::KeyCode>(key.KeyCode) == gui::KeyCode::Delete && key.Modifiers == 0 && !self->m_tree->IsEditing())
                                               {
                                                   self->RunAction(SceneActionIds::kEntityDelete);
                                                   e.Handled = true;
                                               }
                                           });
        }
        // The page's selection into the tree (a viewport pick, a marquee, an undo).
        m_edit->EntitySelection().OnChanged = [self]()
        {
            if (!self->m_syncing)
                self->SyncSelectionToTree();
        };
        Refresh();
    }

    SceneHierarchyView::~SceneHierarchyView()
    {
        m_edit->EntitySelection().OnChanged = {};
        if (m_tree != nullptr)
            m_tree->SetModel(nullptr);
    }

    void SceneHierarchyView::Refresh()
    {
        if (!m_built || m_edit->Scene().Revision() != m_revision)
        {
            m_revision = m_edit->Scene().Revision();
            Build();
        }
        if (m_bar)
            m_bar->Sync();
    }

    void SceneHierarchyView::Search(StringView text)
    {
        if (m_filter.AsView() == text && m_built)
            return;
        m_filter = String(text);
        if (m_clear != nullptr)
            m_clear->SetVisible(!m_filter.IsEmpty());
        // A whole guid asks for that entity: shown with its way open, selected and scrolled to.
        Guid wanted;
        const bool whole = FilterAsGuid(m_filter.AsView(), wanted) && m_edit->Scene().IsValid(m_edit->Scene().FindEntity(wanted));
        Build();
        if (whole)
        {
            OpenWayTo(wanted);
            m_edit->EntitySelection().Set(wanted);
        }
    }

    void SceneHierarchyView::Build()
    {
        m_built = true;
        scene::Scene& scene = m_edit->Scene();
        const String name = m_services.sceneName ? String(m_services.sceneName()) : String(scene.Name());
        m_model.Build(scene, name.AsView(), m_services.sceneIcon, m_services.icons, m_filter.AsView());
        // The scene's row is always open. The scene opens with its top level shown (a model's
        // skeleton stays closed); after that an entity seen with children for the first time
        // opens (what an edit makes shows), and one the user closed stays closed.
        if (m_tree != nullptr)
        {
            m_tree->SetIdExpanded(kSceneRowId, true);
            const bool first = !m_opened;
            m_opened = true;
            for (const SceneTreeModel::Row& row : m_model.Rows())
            {
                if (row.entity.IsNil() || row.children.IsEmpty())
                    continue;
                const gui::ItemId id = SceneTreeModel::IdOfEntity(row.entity);
                if (!m_known.Contains(id))
                {
                    if (!first)
                        m_tree->SetIdExpanded(id, true);
                    (void)m_known.Insert(id);
                }
            }
            // A search opens every way to its matches.
            if (!m_filter.IsEmpty())
                for (const SceneTreeModel::Row& row : m_model.Rows())
                    if (!row.entity.IsNil() && !row.children.IsEmpty())
                        m_tree->SetIdExpanded(SceneTreeModel::IdOfEntity(row.entity), true);
        }
        m_model.Reset();
        SyncSelectionToTree();
    }

    void SceneHierarchyView::SyncSelectionToTree()
    {
        if (m_tree == nullptr)
            return;
        Array<gui::ItemId> ids(m_allocator);
        for (const Guid& entity : m_edit->EntitySelection().Items())
            ids.PushBack(SceneTreeModel::IdOfEntity(entity));
        m_syncing = true;
        m_tree->SetSelectedIds(Span<const gui::ItemId>(ids.Data(), ids.Size()));
        m_syncing = false;
    }

    void SceneHierarchyView::SyncSelectionFromTree()
    {
        if (m_syncing || m_tree == nullptr)
            return;
        Array<gui::ItemId> ids(m_allocator);
        m_tree->GetSelectedIds(ids);
        Array<Guid> entities(m_allocator);
        for (const gui::ItemId id : ids)
            if (const Guid entity = m_model.EntityOfId(id); !entity.IsNil())
                entities.PushBack(entity);
        m_syncing = true;
        if (entities.IsEmpty())
            m_edit->EntitySelection().Clear(); // the scene's row, or nothing: no entity
        else
            m_edit->EntitySelection().Set(Span<const Guid>(entities.Data(), entities.Size()));
        m_syncing = false;
    }

    bool SceneHierarchyView::SceneSelected() const
    {
        return m_tree != nullptr && m_tree->IsIdSelected(kSceneRowId);
    }

    void SceneHierarchyView::OpenWayTo(const Guid& entity)
    {
        if (m_tree == nullptr)
            return;
        scene::Scene& scene = m_edit->Scene();
        for (scene::EntityHandle e = scene.GetParent(scene.FindEntity(entity)); scene.IsValid(e); e = scene.GetParent(e))
            m_tree->SetIdExpanded(SceneTreeModel::IdOfEntity(scene.GetEntityId(e)), true);
    }

    void SceneHierarchyView::BeginRename(const Guid& entity)
    {
        if (m_tree == nullptr)
            return;
        OpenWayTo(entity);
        if (const i32 item = ItemOf(entity); item >= 0)
            (void)m_tree->BeginEdit(item);
    }

    void SceneHierarchyView::ShowMenu(i32 item, Float2 at)
    {
        if (m_tree == nullptr || m_services.context == nullptr)
            return;
        EditorActionRegistry& actions = m_services.context->Actions();
        EditorPage* page = m_services.page;
        SceneHierarchyView* self = this;
        m_menu = MakeRef<gui::Menu>(m_allocator);
        const Guid entity = EntityAt(item);
        if (entity.IsNil())
        {
            // The scene's row or the background: what makes a root.
            const StringView ids[] = {SceneActionIds::kEntityCreate, SceneActionIds::kEntitySpawnPrefab, SceneActionIds::kEntityPaste};
            (void)AppendActionItems(*m_menu, actions, page, Span<const StringView>(ids, 3));
        }
        else
        {
            // A press on a row not selected selects it alone first, so the actions act on it.
            if (!m_edit->EntitySelection().Contains(entity))
                m_edit->EntitySelection().Set(entity);
            const StringView head[] = {SceneActionIds::kEntityCreateChild};
            (void)AppendActionItems(*m_menu, actions, page, Span<const StringView>(head, 1));
            (void)m_menu->AddItem(u8"Rename", [self, entity]() { self->BeginRename(entity); })->SetShortcut(u8"F2");
            (void)m_menu->AddItem(u8"Copy ID", [self, entity]()
                                  {
                                      gui::EventDispatcher* dispatcher = self->m_tree->GetEventDispatcher();
                                      if (self->m_services.context != nullptr)
                                          (void)self->m_services.context->CopyText(dispatcher != nullptr ? dispatcher->GetClipboard() : nullptr,
                                                                                   Format(u8"{}", entity).AsView(), u8"entity ID");
                                  });
            m_menu->AddSeparator();
            const StringView entityIds[] = {SceneActionIds::kEntityDuplicate, SceneActionIds::kEntityCreatePrefab, SceneActionIds::kEntitySpawnPrefabAsChild,
                                            SceneActionIds::kEntitySpawnPrefab};
            (void)AppendActionItems(*m_menu, actions, page, Span<const StringView>(entityIds, 4));
            if (actions.IsEnabled(SceneActionIds::kPrefabApply, page))
            {
                m_menu->AddSeparator();
                const StringView prefab[] = {SceneActionIds::kPrefabApply, SceneActionIds::kPrefabRevert};
                (void)AppendActionItems(*m_menu, actions, page, Span<const StringView>(prefab, 2));
            }
            m_menu->AddSeparator();
            const StringView clipboard[] = {SceneActionIds::kEntityCopy, SceneActionIds::kEntityPasteAsChild};
            (void)AppendActionItems(*m_menu, actions, page, Span<const StringView>(clipboard, 2));
            m_menu->AddSeparator();
            const StringView tail[] = {SceneActionIds::kEntityDelete};
            (void)AppendActionItems(*m_menu, actions, page, Span<const StringView>(tail, 1));
        }
        m_menu->Open(*m_tree, at);
    }

    void SceneHierarchyView::RunAction(StringView id)
    {
        if (m_services.context == nullptr)
            return;
        const Status ran = m_services.context->Actions().Execute(id, m_services.page);
        // Refused (while simulating, say): said, not swallowed.
        if (!ran.IsOk() && ran.Code() == ErrorCode::NotSupported)
            m_services.context->SetStatus(u8"Not while the scene simulates: stop it first.");
    }

    String SceneHierarchyView::Rows() const
    {
        String out;
        if (m_tree == nullptr)
            return out;
        for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
        {
            if (!out.IsEmpty())
                out += u8" | ";
            for (i32 depth = m_tree->ItemDepth(item); depth > 0; --depth)
                out += u8"  ";
            out += m_tree->GetItemText(item);
        }
        return out;
    }

    Guid SceneHierarchyView::EntityAt(i32 item) const
    {
        if (m_tree == nullptr || item < 0 || item >= static_cast<i32>(m_tree->GetItemCount()))
            return Guid{};
        return m_model.EntityOfNode(m_tree->GetItemIndex(item).InternalId);
    }

    i32 SceneHierarchyView::ItemOf(const Guid& entity) const
    {
        if (m_tree == nullptr)
            return -1;
        for (i32 item = 0; item < static_cast<i32>(m_tree->GetItemCount()); ++item)
            if (EntityAt(item) == entity && !entity.IsNil())
                return item;
        return -1;
    }

    gui::ModelIndex SceneHierarchyView::IndexOf(const Guid& entity) const { return m_model.IndexOf(entity); }

    gui::DragPayload SceneHierarchyView::DragOf(Span<const Guid> entities) const
    {
        const scene::EntityHandle first = entities.IsEmpty() ? scene::EntityHandle{} : m_edit->Scene().FindEntity(entities[0]);
        return EntityDrag::Payload(entities, first.IsAssigned() ? m_edit->Scene().GetEntityName(first) : StringView{});
    }

    bool SceneHierarchyView::DropTarget(const gui::ModelDrop& drop, Guid& parent, Guid& before) const
    {
        parent = Guid{};
        before = Guid{};
        if (!drop.Index.IsValid())
            return true; // below the rows: the end of the roots
        const Guid target = m_model.EntityOfNode(drop.Index.InternalId);
        if (target.IsNil() || drop.Place == gui::DropPlace::On)
        {
            parent = target; // onto an entity: under it; onto the scene's row: a root
            return true;
        }
        scene::Scene& scene = m_edit->Scene();
        const scene::EntityHandle e = scene.FindEntity(target);
        if (!scene.IsValid(e))
            return false;
        const scene::EntityHandle up = scene.GetParent(e);
        parent = scene.IsValid(up) ? scene.GetEntityId(up) : Guid{};
        if (drop.Place == gui::DropPlace::Before)
            before = target;
        else
        {
            const scene::EntityHandle next = scene.GetNextSibling(e);
            before = scene.IsValid(next) ? scene.GetEntityId(next) : Guid{};
        }
        return true;
    }

    bool SceneHierarchyView::CanTake(const gui::ModelDrop& drop) const
    {
        String reason;
        return m_model.CanDrop(drop, reason) != gui::DropEffect::None;
    }
    bool SceneHierarchyView::Take(const gui::ModelDrop& drop)
    {
        return CanTake(drop) && m_model.Drop(drop);
    }

    // === The model ===

    bool SceneHierarchyView::Model::IsEditable(const gui::ModelIndex& index) const
    {
        return !EntityOfNode(index.InternalId).IsNil();
    }
    bool SceneHierarchyView::Model::SetData(const gui::ModelIndex& index, const gui::Variant& value)
    {
        const Guid entity = EntityOfNode(index.InternalId);
        const String name = value.ToString();
        if (entity.IsNil() || name.IsEmpty())
            return false;
        m_view.m_edit->RenameEntity(entity, name.AsView());
        return true;
    }
    gui::DropEffect SceneHierarchyView::Model::CanDrop(const gui::ModelDrop& drop, String& reason) const
    {
        Guid parent;
        Guid before;
        if (!m_view.DropTarget(drop, parent, before))
            return gui::DropEffect::None;
        // The hierarchy's own rows move: never inside themselves.
        if (drop.Source == this && !drop.Ids.IsEmpty())
        {
            for (const gui::ItemId id : drop.Ids)
            {
                const Guid entity = EntityOfId(id);
                if (entity.IsNil())
                {
                    reason = String(u8"The scene stays where it is");
                    return gui::DropEffect::None;
                }
                if (!parent.IsNil() && m_view.m_edit->IsSelfOrAncestor(parent, entity))
                {
                    reason = String(u8"An entity cannot go inside itself");
                    return gui::DropEffect::None;
                }
            }
            return gui::DropEffect::Move;
        }
        // Prefabs from the assets: an instance of each, there.
        Array<app::AssetDrag> assets;
        if (drop.Payload != nullptr && app::AssetDrag::ReadAll(*drop.Payload, assets) && m_view.m_services.spawnPrefab)
        {
            for (const app::AssetDrag& asset : assets)
                if (asset.TypeName.AsView() != StringView(u8"PrefabDocument"))
                {
                    reason = String(u8"Only a prefab goes into the scene");
                    return gui::DropEffect::None;
                }
            return gui::DropEffect::Copy;
        }
        return gui::DropEffect::None;
    }
    bool SceneHierarchyView::Model::Drop(const gui::ModelDrop& drop)
    {
        String reason;
        if (CanDrop(drop, reason) == gui::DropEffect::None)
            return false;
        Guid parent;
        Guid before;
        (void)m_view.DropTarget(drop, parent, before);
        SceneEditContext& edit = *m_view.m_edit;
        if (drop.Source == this && !drop.Ids.IsEmpty())
        {
            for (const gui::ItemId id : drop.Ids)
            {
                const Guid entity = EntityOfId(id);
                if (entity == before || entity == parent)
                    continue;
                if (!before.IsNil() || parent.IsNil())
                    edit.MoveEntityBefore(entity, before); // nil before a root: the end of the roots
                else
                    edit.ReparentEntity(entity, parent); // the end of the parent's children
            }
            return true;
        }
        Array<app::AssetDrag> assets;
        (void)app::AssetDrag::ReadAll(*drop.Payload, assets);
        bool spawned = false;
        for (const app::AssetDrag& asset : assets)
            spawned = m_view.m_services.spawnPrefab(asset.Id, parent) || spawned;
        return spawned;
    }
}
