// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :script_behaviors_source partition.
//
// A script component's behaviours as a property source, under its header in the inspector.
// Reflection does not show them (a behaviour's properties belong to its script, from the cooked
// class), so this source does. It is a list of behaviours, each with its script (an asset slot
// taking script classes), Enabled, its update interval and its script's properties by their kinds
// (a vector as its three parts, an entity as a slot over the scene, an asset as a slot of its
// type). A property differs from its script's default while the behaviour overrides it, and a
// revert drops the override. Every edit is the edit context's MutateComponent per entity, grouped
// into one undo step for the selection. A behaviour whose script has no cooked class says so in its
// name; one with no script shows only its own rows.

module;
#include "Core/Prelude.h"

export module editor.scene:script_behaviors_source;

import foundation.core;
import foundation.resource;
import foundation.scene;
import foundation.script.resource;
import engine.script;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    /// A script class's properties as a grid's rows (for a behaviour or a level): each row is named
    /// `p<hash>` and takes its kind from the property's, a vector shows as its three parts, and
    /// values convert to and from the grid's.
    struct ScriptPropertyRows
    {
        /// The rows of `type`'s properties.
        static void Describe(const foundation::script::ScriptClass& type, Array<foundation::gui::PropertyInfo>& out);
        /// A vector property's parts (`x`, `y`, `z`), under its row; none for any other.
        static void Parts(const foundation::script::ScriptPropertyDesc& property, Array<foundation::gui::PropertyInfo>& out);
        /// The property a row names (`p<hash>`); null when none.
        [[nodiscard]] static const foundation::script::ScriptPropertyDesc* Find(const foundation::script::ScriptClass& type, StringView row);
        /// A row's value: `value` as the property's kind (`part` is the vector part, or -1).
        [[nodiscard]] static foundation::gui::Variant ToGrid(const foundation::script::ScriptPropertyValue& value, foundation::script::ScriptPropertyType type, i32 part);
        /// `write` applied over `old`, as the property's kind.
        [[nodiscard]] static foundation::script::ScriptPropertyValue FromGrid(foundation::script::ScriptPropertyValue old, foundation::script::ScriptPropertyType type, i32 part,
                                                                              const foundation::gui::Variant& write);
        /// A row's name (`p<hash>`) and a part's index (`x`: 0) parsed from what follows a source's
        /// own path.
        static void ParseRow(StringView rest, String& row, i32& part);
    };

    class ScriptBehaviorsSource final : public foundation::gui::IPropertySource
    {
    public:
        ScriptBehaviorsSource(SceneEditContext& edit, EditorContext* context, Span<const Guid> entities);

        /// Tells the grid when the behaviours changed outside it (an undo, a pick elsewhere).
        void Refresh();

        // === IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override { return m_entities.Size(); }
        void GetProperties(StringView parent, Array<foundation::gui::PropertyInfo>& out) const override;
        [[nodiscard]] foundation::gui::Variant GetValue(StringView path, usize object) const override;
        void BeginEdit(StringView) override {}
        void PreviewEdit(StringView, const foundation::gui::PropertyWrite&) override {}
        void CommitEdit(StringView path, const foundation::gui::PropertyWrite& write) override;
        void CancelEdit(StringView) override {}
        bool EditList(StringView path, const foundation::gui::ListEdit& edit) override;
        [[nodiscard]] foundation::gui::Variant GetDefault(StringView path, usize object) const override;
        bool Revert(StringView path) override;
        [[nodiscard]] String GetRevertText(StringView) const override { return String(u8"Revert to the script's default"); }
        [[nodiscard]] String CopyValue(StringView) const override { return String{}; }
        bool PasteValue(StringView, StringView, String& reason) override
        {
            reason = String(u8"A behaviour's value does not paste");
            return false;
        }

    private:
        // What a path names: the list, a behaviour, one of its rows, a property's part.
        struct Where
        {
            bool list = false;
            i64 behavior = -1;
            String row; // "script", "enabled", "interval", "p<hash>"
            i32 part = -1; // a vector property's x, y or z
        };
        [[nodiscard]] static Where Parse(StringView path);
        [[nodiscard]] engine::script::ScriptComponent* Component(usize object) const;
        [[nodiscard]] usize BehaviorCount() const;
        [[nodiscard]] const foundation::script::ScriptClass* ClassOf(const engine::script::ScriptBehavior& behavior) const;
        [[nodiscard]] const foundation::script::ScriptPropertyDesc* PropertyOf(usize behavior, StringView row) const;
        [[nodiscard]] u64 Signature() const;
        // Every selected entity's component mutated as one undo step.
        void Mutate(StringView words, const Function<void(engine::script::ScriptComponent&)>& mutate);

        SceneEditContext* m_edit;
        EditorContext* m_context;
        Array<Guid> m_entities;
        u64 m_signature = 0;
        u64 m_groups = 0;
    };
}
