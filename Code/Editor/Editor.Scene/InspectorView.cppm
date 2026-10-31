// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :inspector partition.
//
// SceneInspectorView: the inspector inside a scene page, one per page, on the GUI's property grid.
// It shows the page's selection: a head (the type icon, the name and the kind, or a count when
// several are selected) and one grid of sections (the editor's PropertySections). The sections are
// the entity's Active flag, its Transform (a reflected Transform with its rotation in Euler
// degrees), then each component every selected entity has, with its reflected rows and the editors
// chosen by hint: an asset reference is a slot that takes what the builders make, and an entity
// reference is a slot over the scene's entities (EntityRef is taken as a reference by its id).
// Several entities are edited at once, and a row whose value differs among them shows as mixed.
//   - Every edit is one undo step on the page's stack. A component edit is the reflected source's
//     change (each entity's value before and after as text, restored on undo onto the components
//     found again by entity and type); the transform and Active go through the edit context's
//     commands.
//   - A component's header has its verbs: copy, paste over it, reset to its defaults, revert to
//     its prefab (while it overrides the prefab) and remove. Add Component opens the picker over
//     the component types the selection lacks, by category, each with its icon and description.
//   - A search narrows the rows. While the page simulates, the grid is read-only and says so.
//   - With the scene selected (its hierarchy row), the grid shows the scene's settings: a section
//     per system's block, each property showing and editing where it lands (the block's own
//     values, or its profile's while it uses one), and a block's profile actions under its header
//     (open the profile, make one from these values, copy its values into the scene).
//   - The grid is rebuilt when the selection, the scene's shape or the components change;
//     otherwise only the values are shown again, so a gizmo's drag or an undo shows as it happens.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:inspector;

import foundation.core;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :edit;
import :entity_ref_editor;
import :entity_picker_window;
import :inspector_extras;
import :scene_settings_source;

using namespace foundation::core;

export namespace editor
{
    class SceneInspectorView
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            EditorPage* page = nullptr; // the page whose selection it shows (its actions, its simulation)
            EditorIcons* icons = nullptr;
            const app::ResourceRefServices* refs = nullptr; // services for the asset slots (null: none)
            /// Opens the picker for `request` (Add Component).
            Function<void(app::PickerRequest request)> pick;
            /// Opens the entity picker for `request` (an entity slot's pick).
            Function<void(EntityPickRequest request)> pickEntity;
            /// Whether the page simulates (the grid is read-only meanwhile).
            Function<bool()> simulating;
            /// Whether the scene's own row is selected (the scene's settings then show).
            Function<bool()> sceneSelected;
            /// The scene's icon (a prefab's or a scene's document) and name, for its head.
            StringView sceneIcon;
            Function<StringView()> sceneName;
        };

        /// The inspector over `edit`'s selection in `pane` (found in the markup by id:
        /// `inspector-icon`, `inspector-name`, `inspector-kind`, `inspector-search`,
        /// `inspector-add`, `inspector-grid`, `inspector-state`, `inspector-note`).
        SceneInspectorView(IAllocator& allocator, SceneEditContext& edit, Services services, foundation::gui::Node& pane);
        SceneInspectorView(const SceneInspectorView&) = delete;
        SceneInspectorView& operator=(const SceneInspectorView&) = delete;
        ~SceneInspectorView();

        /// Once a frame: rebuilds when what it shows has changed, otherwise shows its values again.
        void Refresh();
        /// Searches the rows for `text`, as typing it does.
        void Search(StringView text);
        /// Opens Add Component's picker.
        void OpenAddComponent();
        /// What Add Component offers the selection now, by type name.
        [[nodiscard]] Array<String> AddableComponents() const;
        /// The same, as types (for the picker's display names and categories).
        [[nodiscard]] Array<const TypeInfo*> AddableTypes() const;
        /// Adds a component of `type` (a type name) to every selected entity, one undo step each.
        bool AddComponent(StringView type);
        /// Whether a copied component on the clipboard can be pasted onto the selected entities
        /// (Paste Component's button shows while it can).
        [[nodiscard]] bool CanPasteComponent() const;
        /// Paste Component: pastes the copied component onto every selected entity as one undo
        /// step. When one already has that component, it asks first, as the component is
        /// overwritten. False when there is nothing to do.
        bool PasteComponent();
        /// The same without asking (what the confirmation's Yes runs).
        bool PasteComponentNow();
        /// The overwrite question, while it is open.
        [[nodiscard]] foundation::gui::MessageBox* PasteConfirm() const;

        // === For tests ===
        [[nodiscard]] foundation::gui::PropertyGrid* Grid() const noexcept { return m_grid; }
        [[nodiscard]] app::PropertySections& Sections() noexcept { return m_sections; }
        /// The sections' titles in order, by " | ".
        [[nodiscard]] String SectionTitles() const;
        /// The grid's path of `property` in the section titled `title` (empty: no such section).
        [[nodiscard]] String PathOf(StringView title, StringView property) const;
        /// The component a section's header path ("#Light") stands for; null: none.
        [[nodiscard]] const TypeInfo* ComponentOf(StringView headerPath) const;
        /// A header's menu as the grid builds it, at `headerPath`.
        bool OpenHeaderMenu(StringView headerPath);
        [[nodiscard]] StringView Kind() const;
        /// A settings block's section by its title; null: none.
        [[nodiscard]] SceneSettingsSource* SettingsOf(StringView title) const;
        /// A settings block's profile action `name` ("Open Profile", "Make Profile", "Copy Into
        /// Scene"): whether it shows now. RunProfileAction runs it.
        [[nodiscard]] bool ShowsProfileAction(StringView title, StringView name) const;
        bool RunProfileAction(StringView title, StringView name);
        /// A section's extras' row at `name` (a notice's text, an action's label); empty when
        /// there is none or it is hidden.
        [[nodiscard]] String ExtraText(StringView title, StringView name) const;
        /// Runs a section's action `name`, as its button does.
        bool RunExtra(StringView title, StringView name);
        /// A section's extras' source `index` (a script component's behaviours, a block's
        /// collision groups); null: none.
        [[nodiscard]] foundation::gui::IPropertySource* ExtraSource(StringView title, usize index) const;

    private:
        // A component's extras: a bag under its header, whose notices are asked again as values
        // show and whose actions run on their rows' commits.
        class Extras;
        struct ComponentSection
        {
            String key;
            String title;
            const TypeInfo* type = nullptr;
            UniquePtr<foundation::gui::ReflectedPropertySource> source;
            UniquePtr<Extras> extras; // null: none
        };

        void Build();
        void Clear();
        void ShowPaste();
        [[nodiscard]] u64 Signature() const;
        void BuildHead();
        void BuildTransform();
        void BuildComponents();
        void PullTransforms();
        // A type's extras (for a component or a settings block) under its section; null when it has
        // none.
        [[nodiscard]] UniquePtr<Extras> BuildExtras(const TypeInfo& type, StringView key, StringView title, usize objects);
        void BuildScene();
        [[nodiscard]] bool ShowsScene() const;
        void RefreshExtras();
        void HeaderMenu(foundation::gui::Menu& menu, const foundation::gui::PropertyRow& row);
        void ComponentChanged(const TypeInfo* type, const foundation::gui::ReflectedChange& change);
        [[nodiscard]] Array<foundation::core::Instance> InstancesOf(const TypeInfo& type) const;
        [[nodiscard]] static String TitleOf(const TypeInfo& type);
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const;

        IAllocator& m_allocator;
        SceneEditContext* m_edit;
        Services m_services;
        foundation::gui::Node* m_pane = nullptr;
        RefPtr<foundation::gui::MessageBox> m_pasteConfirm; // Paste Component's overwrite question
        foundation::gui::PropertyGrid* m_grid = nullptr;
        app::PropertySections m_sections;
        EntityRefEditor::Services m_entityRefs; // the entity slots' services (the grid's registry holds them)
        app::XmlPropertyTextFormat m_format;
        // The selection it shows, in order.
        Array<Guid> m_entities;
        // Active: a value per entity.
        UniquePtr<foundation::gui::PropertyBag> m_active;
        // Transform: a copy of each entity's, as the reflected source's objects; an edit writes
        // them back.
        Array<Transform> m_transforms;
        UniquePtr<foundation::gui::ReflectedPropertySource> m_transform;
        Array<UniquePtr<ComponentSection>> m_components;
        // The scene's settings while it is selected: a source per block, and a block's profile
        // actions.
        struct SettingsSection
        {
            String key;
            String title;
            UniquePtr<SceneSettingsSource> source;
            UniquePtr<Extras> profile; // null: the block has no profile
            UniquePtr<Extras> extras;  // null: none
        };
        Array<UniquePtr<SettingsSection>> m_settings;
        RefPtr<foundation::gui::Menu> m_menu;
        String m_filter;
        u64 m_signature = ~0ull;
        bool m_readOnly = false;
        u64 m_edits = 0; // transform edits made, each a group of its own
    };
}
