// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :entity_ref_editor partition.
//
// The inspector's entity reference (Documentation/Specs/editor-gui.md, 5.3, layer 4, part 2): a
// component's EntityRef as a slot, the grid's row for it (the reflected source takes EntityRef as a
// reference by its id, hinted with its name, so this editor is chosen before the asset slot). It
// shows the entity referenced by its type icon and its name, "None", or "(missing)" when no entity
// of the scene has that id; its pick is the entity picker, the scene as its hierarchy shows it
// (the current one lit, None to clear); an entity dragged from the hierarchy drops on it. A pick, a
// drop and a clear are one write through the grid, so one undo step.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:entity_ref_editor;

import foundation.core;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :edit;
import :hierarchy; // EntityDrag
import :entity_picker_window; // EntityPickRequest

using namespace foundation::core;

export namespace editor
{
    class EntityRefEditor final : public foundation::gui::SingleControlEditor
    {
        RTTI_OBJECT(EntityRefEditor, foundation::gui::SingleControlEditor)
    public:
        static constexpr StringView kHint = u8"EntityRef";
        static constexpr StringView kMissingText = u8"(missing)";

        struct Services
        {
            SceneEditContext* edit = nullptr;
            EditorIcons* icons = nullptr;
            /// Opens the entity picker for `request`.
            Function<void(EntityPickRequest request)> pick;
        };

        /// `services` outlives the editor (the registrar's).
        explicit EntityRefEditor(const Services& services) : m_services(&services)
        {
            AddClass(u8"reference");
            m_slot = MakeRef<app::AssetPickerSlot>(MemoryAllocator());
            Hold(*m_slot);
        }
        EntityRefEditor() : EntityRefEditor(m_none) {} // what RTTI makes (its own empty services); the registry's has its services

        /// The editor for every EntityRef `registry` shows; `services` outlives it.
        static void Register(foundation::gui::PropertyEditorRegistry& registry, const Services& services)
        {
            const Services* table = &services;
            registry.RegisterHint(kHint, [table](IAllocator& allocator) -> RefPtr<foundation::gui::PropertyEditor>
                                  { return MakeRef<EntityRefEditor>(allocator, *table); }, true);
        }

        [[nodiscard]] app::AssetPickerSlot* Slot() const noexcept { return m_slot.Get(); }
        /// What it references: nil for none, or when the objects differ.
        [[nodiscard]] const Guid& Current() const noexcept { return m_current; }
        /// The entity picker's request it opens: its value, and an answer that writes the pick
        /// (while the row still shows this property).
        [[nodiscard]] EntityPickRequest Request();
        /// One write through the grid: a pick, a drop and a clear alike.
        void Assign(const Guid& id);
        /// Takes a dragged entity, as a drop on it does; false when the payload carries none of
        /// this scene's.
        bool Take(const foundation::gui::DragPayload& payload);

    protected:
        void OnBind(const foundation::gui::PropertyInfo& info, const foundation::gui::PropertyValue& value) override;
        void OnDragQuery(foundation::gui::DropQuery& query) override;
        void OnDrop(const foundation::gui::DragEvent& event) override;

    private:
        void Show();
        void Pick();
        Services m_none; // an editor RTTI made has no services: its own, empty (a process-wide one would be state in an interface)
        const Services* m_services;
        RefPtr<app::AssetPickerSlot> m_slot;
        Guid m_current;
        bool m_mixed = false;
    };

    RTTI_DEFINE_OBJECT(EntityRefEditor, "rtti::editor::editor")
}
