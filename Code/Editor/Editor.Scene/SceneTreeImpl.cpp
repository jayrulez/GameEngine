// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_tree partition, its body. See SceneTree.cppm.

module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.scene.resource;
import foundation.gui;
import editor.core;
import :scene_tree;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    SceneTreeModel::SceneTreeModel(IAllocator& allocator) : m_rows(allocator), m_entityOfId(allocator) {}

    void SceneTreeModel::Build(scene::Scene& scene, StringView sceneName, StringView sceneIcon, EditorIcons* icons, StringView filter)
    {
        m_rows.Clear();
        m_entityOfId.Clear();
        m_filter = String(filter);
        m_icons = icons;
        Row top;
        top.name = String(sceneName);
        top.icon = String(sceneIcon);
        m_rows.PushBack(Move(top));
        // Roots in list order (the order reordering keeps and saving writes), then each one's
        // children depth first; with a search, an entity whose subtree holds a match.
        for (scene::EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
            if (SubtreeMatches(scene, r))
                AddEntity(scene, r, 0);
    }

    void SceneTreeModel::AddEntity(scene::Scene& scene, scene::EntityHandle e, i32 parent)
    {
        const i32 node = static_cast<i32>(m_rows.Size());
        Row row;
        row.entity = scene.GetEntityId(e);
        row.parent = parent;
        row.rowInParent = static_cast<i32>(m_rows[static_cast<usize>(parent)].children.Size());
        row.name = String(scene.GetEntityName(e));
        if (row.name.IsEmpty())
            row.name = String(u8"(unnamed)");
        // A prefab's members read apart; an entity that is not active, with what is under it, dims.
        scene::PrefabMemberInfo member;
        const bool prefabMember = scene::FindPrefabMember(scene, row.entity, member);
        const bool active = scene.IsEffectivelyActive(e);
        if (prefabMember)
            row.classes = String(u8"member");
        if (!active)
            row.classes = row.classes.IsEmpty() ? String(u8"inactive") : Format(u8"{} inactive", row.classes);
        IconOf(scene, e, row);
        m_entityOfId.InsertOrAssign(IdOfEntity(row.entity), row.entity);
        m_rows[static_cast<usize>(parent)].children.PushBack(node);
        m_rows.PushBack(Move(row));
        for (scene::EntityHandle c = scene.GetFirstChild(e); c.IsAssigned(); c = scene.GetNextSibling(c))
            if (SubtreeMatches(scene, c))
                AddEntity(scene, c, node);
    }

    // An entity by its first component that has an icon of its own; a plain entity by the generic one.
    void SceneTreeModel::IconOf(scene::Scene& scene, scene::EntityHandle e, Row& row) const
    {
        if (m_icons == nullptr)
            return;
        scene.ForEachManager([&](scene::ComponentManagerBase& manager)
                             {
                                 if (!row.icon.IsEmpty() || manager.ComponentType() == nullptr || !manager.HasComponent(e))
                                     return;
                                 const TypeIcon icon = m_icons->For(*manager.ComponentType(), StringView{});
                                 row.icon = icon.Source;
                                 row.iconClass = icon.Class;
                             });
        if (row.icon.IsEmpty())
            row.icon = String(u8"Editor/Icons/cube.svg");
    }

    bool SceneTreeModel::SubtreeMatches(scene::Scene& scene, scene::EntityHandle e) const
    {
        if (m_filter.IsEmpty() || NameOrGuidMatches(scene.GetEntityName(e), scene.GetEntityId(e), m_filter.AsView()))
            return true;
        for (scene::EntityHandle c = scene.GetFirstChild(e); c.IsAssigned(); c = scene.GetNextSibling(c))
            if (SubtreeMatches(scene, c))
                return true;
        return false;
    }

    Guid SceneTreeModel::EntityOfNode(i64 node) const
    {
        return node >= 0 && static_cast<usize>(node) < m_rows.Size() ? m_rows[static_cast<usize>(node)].entity : Guid{};
    }
    Guid SceneTreeModel::EntityOfId(gui::ItemId id) const
    {
        const Guid* entity = m_entityOfId.Find(id);
        return entity != nullptr ? *entity : Guid{};
    }
    gui::ItemId SceneTreeModel::IdOfEntity(const Guid& entity)
    {
        return entity.IsNil() ? kSceneRowId : static_cast<gui::ItemId>(Hash<Guid>{}(entity) & ~kSceneRowId);
    }
    gui::ModelIndex SceneTreeModel::IndexOf(const Guid& entity) const
    {
        for (usize node = 0; node < m_rows.Size(); ++node)
            if (m_rows[node].entity == entity)
                return gui::MakeModelIndex(m_rows[node].rowInParent, 0, static_cast<i64>(node));
        return gui::ModelIndex{};
    }

    // === IModel ===

    usize SceneTreeModel::RowCount(const gui::ModelIndex& parent) const
    {
        if (!parent.IsValid())
            return m_rows.IsEmpty() ? 0u : 1u;
        const i64 node = parent.InternalId;
        return node >= 0 && static_cast<usize>(node) < m_rows.Size() ? m_rows[static_cast<usize>(node)].children.Size() : 0u;
    }
    gui::ModelIndex SceneTreeModel::Index(i32 row, i32 column, const gui::ModelIndex& parent) const
    {
        if (!parent.IsValid())
            return row == 0 && !m_rows.IsEmpty() ? gui::MakeModelIndex(0, column, 0) : gui::ModelIndex{};
        const i64 node = parent.InternalId;
        if (node < 0 || static_cast<usize>(node) >= m_rows.Size())
            return gui::ModelIndex{};
        const Array<i32>& children = m_rows[static_cast<usize>(node)].children;
        return row >= 0 && static_cast<usize>(row) < children.Size() ? gui::MakeModelIndex(row, column, children[static_cast<usize>(row)]) : gui::ModelIndex{};
    }
    gui::ModelIndex SceneTreeModel::ParentIndex(const gui::ModelIndex& child) const
    {
        const i64 node = child.InternalId;
        if (node <= 0 || static_cast<usize>(node) >= m_rows.Size())
            return gui::ModelIndex{};
        const i32 parent = m_rows[static_cast<usize>(node)].parent;
        return parent >= 0 ? gui::MakeModelIndex(m_rows[static_cast<usize>(parent)].rowInParent, 0, parent) : gui::ModelIndex{};
    }
    gui::ItemId SceneTreeModel::IdOf(const gui::ModelIndex& index) const
    {
        if (!index.IsValid())
            return gui::kNoItem;
        return IdOfEntity(EntityOfNode(index.InternalId));
    }
    gui::Variant SceneTreeModel::Data(const gui::ModelIndex& index, gui::ModelRole role) const
    {
        const i64 node = index.InternalId;
        if (node < 0 || static_cast<usize>(node) >= m_rows.Size())
            return gui::Variant{};
        const Row& row = m_rows[static_cast<usize>(node)];
        switch (role)
        {
        case gui::ModelRole::Display:
        case gui::ModelRole::Sort:
            return gui::Variant(row.name.AsView());
        case gui::ModelRole::Icon:
            return row.icon.IsEmpty() ? gui::Variant{} : gui::Variant(row.icon.AsView());
        case gui::ModelRole::IconClass:
            return row.iconClass.IsEmpty() ? gui::Variant{} : gui::Variant(row.iconClass.AsView());
        case gui::ModelRole::RowClass:
            return row.classes.IsEmpty() ? gui::Variant{} : gui::Variant(row.classes.AsView());
        default:
            return gui::Variant{};
        }
    }
}
