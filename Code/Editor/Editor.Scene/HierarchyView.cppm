// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :hierarchy partition.
//
// SceneHierarchyView: the entity tree inside a scene page, one per page (everything scene-scoped is
// the page's), on the GUI's TreeView (Documentation/Specs/editor-gui.md, 5.3, layer 3). Its model
// is built again from the live scene when the scene's revision moves, so every edit, undo and redo
// shows; a row's id is its entity's guid, so the selection and what is open survive each build.
//   - The scene is the top row (ruling 12: its settings are the scene selected); each entity by its
//     first component's type icon, a prefab's members and inactive entities marked as such.
//   - The selection is the page's, shared with the viewport both ways, several at once.
//   - A double-click frames the entity; F2 renames in place; dragging a row onto another
//     reparents it, between two reorders it, onto the scene's row makes it a root.
//   - The row menu, the background menu, the add button and Delete are the scene editor's actions
//     over the page, one path, so they are refused visibly while it simulates.
//   - The search keeps a match's ancestors; a whole guid selects that entity and opens its way.
//   - A dragged row carries its entities, for an entity slot; a prefab dropped in is spawned,
//     under the row it lands on.

module;
#include "Core/Prelude.h"

export module editor.scene:hierarchy;

import foundation.core;
import foundation.scene;
import foundation.scene.resource;
import foundation.gui;
import editor.core;
import editor.app;
import :edit;
import :scene_tree;

using namespace foundation::core;

export namespace editor
{
    /// What a drag of entities carries (a hierarchy's rows), for an entity slot to take.
    struct EntityDrag
    {
        static constexpr StringView kType = u8"scene/entity";

        /// A payload carrying `entities`, labelled by `name` (the first's) or their count.
        [[nodiscard]] static foundation::gui::DragPayload Payload(Span<const Guid> entities, StringView name)
        {
            foundation::gui::DragPayload payload;
            Array<Variant> ids;
            for (const Guid& id : entities)
            {
                utf8char text[37];
                id.ToChars(text);
                ids.PushBack(Variant::From<String>(String(StringView(text, 36))));
            }
            payload.Type = String(kType);
            payload.Value = Variant::From<Array<Variant>>(Move(ids));
            payload.Label = entities.Size() == 1 ? String(name) : Format(u8"{} entities", entities.Size());
            payload.Allowed = foundation::gui::DropEffectsLink | foundation::gui::DropEffectsMove;
            return payload;
        }
        /// The entities a payload carries; false when it carries none.
        static bool Read(const foundation::gui::DragPayload& payload, Array<Guid>& out)
        {
            if (payload.Type.AsView() != kType)
                return false;
            const Array<Variant>* ids = payload.Value.TryGet<Array<Variant>>();
            if (ids == nullptr)
                return false;
            for (const Variant& id : *ids)
            {
                Guid guid;
                if (const String* text = id.TryGet<String>(); text != nullptr && Guid::TryParse(text->AsView(), guid))
                    out.PushBack(guid);
            }
            return !out.IsEmpty();
        }
    };

    class SceneHierarchyView
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            EditorPage* page = nullptr;         // the page its actions run over
            EditorIcons* icons = nullptr;       // the entities' type icons (null: none)
            StringView sceneIcon;               // the top row's
            Function<StringView()> sceneName;   // the top row's text
            Function<void(const Guid&)> frame;  // a double-click frames an entity
            /// A prefab dropped in, under `parent` (nil: the scene's root); false when it was not.
            Function<bool(const Guid& prefab, const Guid& parent)> spawnPrefab;
        };

        /// The hierarchy over `edit`'s scene in `pane` (the markup's: `hierarchy-tree`,
        /// `hierarchy-search`, `hierarchy-clear`, `hierarchy-add`, by id).
        SceneHierarchyView(IAllocator& allocator, SceneEditContext& edit, Services services, foundation::gui::Node& pane);
        SceneHierarchyView(const SceneHierarchyView&) = delete;
        SceneHierarchyView& operator=(const SceneHierarchyView&) = delete;
        ~SceneHierarchyView();

        /// Once a frame: built again when the scene changed; the add button as its action is now.
        void Refresh();
        /// The search, as typing it does.
        void Search(StringView text);
        /// The in-place rename of an entity's row (F2, the menu's Rename).
        void BeginRename(const Guid& entity);
        /// Whether the scene's own row is selected (ruling 12: the scene's settings are the scene
        /// selected; the inspector shows them).
        [[nodiscard]] bool SceneSelected() const;
        /// The menu of the row at item `item` (-1: the background's) at `at`, where the press was.
        void ShowMenu(i32 item, Float2 at);

        // === For tests ===
        [[nodiscard]] foundation::gui::TreeView* Tree() const noexcept { return m_tree; }
        /// The shown rows' texts in order, the scene's first, nested by two spaces a level.
        [[nodiscard]] String Rows() const;
        /// The entity of shown item `item` (nil: the scene's row, or none).
        [[nodiscard]] Guid EntityAt(i32 item) const;
        /// The item showing `entity`; -1 when it is not shown.
        [[nodiscard]] i32 ItemOf(const Guid& entity) const;
        [[nodiscard]] foundation::gui::Menu* OpenMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }
        /// What a drag of `entities` carries.
        [[nodiscard]] foundation::gui::DragPayload DragOf(Span<const Guid> entities) const;
        /// Whether `drop` would be taken, and takes it (what a drag ending on the tree does).
        [[nodiscard]] bool CanTake(const foundation::gui::ModelDrop& drop) const;
        bool Take(const foundation::gui::ModelDrop& drop);
        /// The model index of `entity` (nil: the scene's row).
        [[nodiscard]] foundation::gui::ModelIndex IndexOf(const Guid& entity) const;

    private:
        // The tree's model: the scene as the shared tree shows it, plus rename in place and drops
        // (a reparent, a reorder, a prefab spawned). A build tells the view once.
        class Model final : public SceneTreeModel
        {
        public:
            Model(SceneHierarchyView& view, IAllocator& allocator) : SceneTreeModel(allocator), m_view(view) {}
            [[nodiscard]] bool IsEditable(const foundation::gui::ModelIndex& index) const override;
            bool SetData(const foundation::gui::ModelIndex& index, const foundation::gui::Variant& value) override;
            [[nodiscard]] foundation::gui::DropEffect CanDrop(const foundation::gui::ModelDrop& drop, String& reason) const override;
            bool Drop(const foundation::gui::ModelDrop& drop) override;

        private:
            SceneHierarchyView& m_view;
        };

        void Build();
        // A drop's target: the entity it goes under (nil: a root) and the sibling it goes before (nil: last).
        [[nodiscard]] bool DropTarget(const foundation::gui::ModelDrop& drop, Guid& parent, Guid& before) const;
        void SyncSelectionToTree();
        void SyncSelectionFromTree();
        void OpenWayTo(const Guid& entity);
        void RunAction(StringView id);

        IAllocator& m_allocator;
        SceneEditContext* m_edit;
        Services m_services;
        Model m_model;
        HashSet<foundation::gui::ItemId> m_known; // rows built with children before: one new to children opens
        foundation::gui::TreeView* m_tree = nullptr;
        foundation::gui::TextField* m_search = nullptr;
        foundation::gui::Button* m_clear = nullptr;
        UniquePtr<app::PageActionBar> m_bar; // the add button
        RefPtr<foundation::gui::Menu> m_menu;
        String m_filter;
        u64 m_revision = ~0ull;
        bool m_syncing = false;
        bool m_built = false;
        bool m_opened = false; // the first build is done: what gains children from now on opens
    };
}
