// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :inspector partition.
//
// SceneInspectorView: the inspector inside a scene page, one per page, on the GUI's property grid
// (Documentation/Specs/editor-gui.md, 5.3, layer 4). It shows the page's selection: its head (the
// type icon, the name, the kind; several selected say how many), and one grid of sections (the
// editor's PropertySections): the entity's Active, its Transform (a reflected Transform, its
// rotation in Euler degrees), then each component every selected entity has, its reflected rows
// with the editors by hint (an asset reference a slot taking what the builders make, an entity
// reference a slot over the scene's entities: EntityRef taken as a reference by its id). Several
// entities are edited at once, a row differing among them shown mixed.
//   - Every edit is one undo step on the page's stack: a component's as the reflected source's
//     change (each entity's value before and after as text, restored on undo, the components found
//     again by entity and type), the transform's and Active's through the edit context's commands.
//   - A component's header has its verbs: copy, paste over it, reset to its defaults, revert to
//     its prefab (while it overrides it), remove; Add Component is the picker over the component
//     types the selection lacks, by category, each with its icon and its description.
//   - A search narrows the rows. While the page simulates the grid is read only, and says so.
//   - Built again when the selection, the scene's shape or the components change; otherwise only
//     the values are shown again, so a gizmo's drag or an undo shows as it happens.

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

using namespace foundation::core;

export namespace editor
{
    /// The engine's XmlSerializer as a reflected source's text format: what an edit's before and
    /// after are written in, and an undo reads back.
    class XmlPropertyTextFormat final : public foundation::gui::IPropertyTextFormat
    {
    public:
        explicit XmlPropertyTextFormat(IAllocator& allocator) : m_allocator(allocator) {}
        String Write(Function<void(ISerializer&)> write) override;
        bool Read(StringView text, Function<void(ISerializer&)> read) override;

    private:
        IAllocator& m_allocator;
    };

    class SceneInspectorView
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            EditorPage* page = nullptr; // the page whose selection it shows (its actions, its simulation)
            EditorIcons* icons = nullptr;
            const app::ResourceRefServices* refs = nullptr; // the asset slots' (null: none)
            /// Opens the picker for `request` below `anchor` (Add Component).
            Function<void(app::PickerRequest request, foundation::gui::Node* anchor)> pick;
            /// Whether the page simulates (the grid read only meanwhile).
            Function<bool()> simulating;
        };

        /// The inspector over `edit`'s selection in `pane` (the markup's: `inspector-icon`,
        /// `inspector-name`, `inspector-kind`, `inspector-search`, `inspector-add`,
        /// `inspector-grid`, `inspector-state`, `inspector-note`, by id).
        SceneInspectorView(IAllocator& allocator, SceneEditContext& edit, Services services, foundation::gui::Node& pane);
        SceneInspectorView(const SceneInspectorView&) = delete;
        SceneInspectorView& operator=(const SceneInspectorView&) = delete;
        ~SceneInspectorView();

        /// Once a frame: built again when what it shows changed; else its values shown again.
        void Refresh();
        /// The search over the rows, as typing it does.
        void Search(StringView text);
        /// Add Component's picker, below `anchor`.
        void OpenAddComponent(foundation::gui::Node* anchor);
        /// What Add Component offers the selection now, by type name.
        [[nodiscard]] Array<String> AddableComponents() const;
        /// Adds a component of `type` (a type name) to every selected entity, one undo step each.
        bool AddComponent(StringView type);

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

    private:
        struct ComponentSection
        {
            String key;
            String title;
            const TypeInfo* type = nullptr;
            UniquePtr<foundation::gui::ReflectedPropertySource> source;
        };

        void Build();
        void Clear();
        [[nodiscard]] u64 Signature() const;
        void BuildHead();
        void BuildTransform();
        void BuildComponents();
        void PullTransforms();
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
        foundation::gui::PropertyGrid* m_grid = nullptr;
        app::PropertySections m_sections;
        EntityRefEditor::Services m_entityRefs; // what the entity slots ask (the grid's registry holds it)
        XmlPropertyTextFormat m_format;
        // The selection it shows, in order.
        Array<Guid> m_entities;
        // Active: a value per entity.
        UniquePtr<foundation::gui::PropertyBag> m_active;
        // Transform: a copy of each entity's, the reflected source's objects; an edit sets it back.
        Array<Transform> m_transforms;
        UniquePtr<foundation::gui::ReflectedPropertySource> m_transform;
        Array<UniquePtr<ComponentSection>> m_components;
        RefPtr<foundation::gui::Menu> m_menu;
        String m_filter;
        u64 m_signature = ~0ull;
        bool m_readOnly = false;
        u64 m_edits = 0; // transform edits made, each a group of its own
    };
}
