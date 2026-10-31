// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_tree partition.
//
// SceneTreeModel: the scene as its hierarchy shows it, a GUI tree model built from the live scene,
// shared by the hierarchy and the entity picker (the picker is the hierarchy's tree in a picker's
// frame). The scene is the top row (node 0); under it come the roots in list order, each with its
// children depth first. Each entity shows its first component's type icon (the generic one for a
// plain entity), and a prefab's members and inactive entities are marked as such. With a search, an
// entity shows when its subtree holds a match, so a match keeps its ancestors. A row's id is its
// entity's guid (a fixed one for the scene's row), so what is open and selected survives each
// build. It is read-only; the hierarchy's own model adds rename and drops.

module;
#include "Core/Prelude.h"

export module editor.scene:scene_tree;

import foundation.core;
import foundation.scene;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    class SceneTreeModel : public foundation::gui::IModel
    {
    public:
        /// The scene's row: an id no entity's guid hashes to in practice, kept apart from them.
        static constexpr foundation::gui::ItemId kSceneRowId = 0x5ce4e0000000001ull;

        struct Row
        {
            Guid entity;   // nil: the scene's row
            i32 parent = -1;
            i32 rowInParent = 0;
            Array<i32> children;
            String name;
            String classes; // "member", "inactive"
            String icon;
            String iconClass;
        };

        explicit SceneTreeModel(IAllocator& allocator);

        /// Builds the rows from `scene`: its own row reads `sceneName` with `sceneIcon`, the
        /// entities use `icons`' type icons (null: none), and only what `filter` keeps is shown
        /// (empty: all). It does not notify the view: the owner opens what it wants open, then
        /// calls Reset.
        void Build(foundation::scene::Scene& scene, StringView sceneName, StringView sceneIcon, EditorIcons* icons, StringView filter);
        void Reset() { NotifyReset(); }

        [[nodiscard]] Span<const Row> Rows() const noexcept { return Span<const Row>(m_rows.Data(), m_rows.Size()); }
        /// The entity of a model node (nil: the scene's row, or none).
        [[nodiscard]] Guid EntityOfNode(i64 node) const;
        /// The entity of a row id (nil: the scene's row, or none).
        [[nodiscard]] Guid EntityOfId(foundation::gui::ItemId id) const;
        [[nodiscard]] static foundation::gui::ItemId IdOfEntity(const Guid& entity);
        /// The model index of `entity` (nil: the scene's row); invalid when it is not shown.
        [[nodiscard]] foundation::gui::ModelIndex IndexOf(const Guid& entity) const;

        // === IModel ===
        [[nodiscard]] usize RowCount(const foundation::gui::ModelIndex& parent = {}) const override;
        [[nodiscard]] usize ColumnCount() const override { return 1; }
        [[nodiscard]] foundation::gui::ModelIndex Index(i32 row, i32 column = 0, const foundation::gui::ModelIndex& parent = {}) const override;
        [[nodiscard]] foundation::gui::ModelIndex ParentIndex(const foundation::gui::ModelIndex& child) const override;
        [[nodiscard]] bool HasChildren(const foundation::gui::ModelIndex& parent = {}) const override { return RowCount(parent) > 0; }
        [[nodiscard]] foundation::gui::ItemId IdOf(const foundation::gui::ModelIndex& index) const override;
        [[nodiscard]] foundation::gui::Variant Data(const foundation::gui::ModelIndex& index,
                                                     foundation::gui::ModelRole role = foundation::gui::ModelRole::Display) const override;

    private:
        void AddEntity(foundation::scene::Scene& scene, foundation::scene::EntityHandle e, i32 parent);
        [[nodiscard]] bool SubtreeMatches(foundation::scene::Scene& scene, foundation::scene::EntityHandle e) const;
        void IconOf(foundation::scene::Scene& scene, foundation::scene::EntityHandle e, Row& row) const;

        Array<Row> m_rows; // by node id
        HashMap<foundation::gui::ItemId, Guid> m_entityOfId;
        String m_filter;
        EditorIcons* m_icons = nullptr;
    };
}
