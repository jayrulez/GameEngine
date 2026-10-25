// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :script_behaviors_source partition.
//
// A script component's behaviours as a property source, under its header in the inspector
// (Documentation/Specs/editor-gui.md, 5.3, layer 4, part 3): reflection does not show them (a
// behaviour's properties are its script's, from the cooked class), so this source does. A list of
// behaviours, each its script (an asset slot taking script classes), Enabled, its update interval
// and its script's properties by their kinds (a vector its three parts, an entity a slot over the
// scene, an asset a slot of its type); a property differs from its script's default while the
// behaviour overrides it, and a revert drops the override. Every edit is the edit context's
// MutateComponent per entity, grouped: one undo step for the selection. A behaviour whose script
// has no cooked class says so in its name; one with no script shows only its own rows.

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
