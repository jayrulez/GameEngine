// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :settings_extra_sources partition.
//
// Rows of a scene settings block that reflection does not show, each a source under its block's
// section while the scene is selected (Documentation/Specs/editor-gui.md, 5.3, layer 4, part 4),
// registered by the block's type as inspector extras:
//   - the physics block's collision groups: a list of groups, each its name and the groups it
//     collides with (flags named by the groups). A pair collides both ways or neither, so a toggle
//     writes both; a new group collides with every group; only the last group is removed, as a
//     body keeps its group by index (removing another renumbers the ones after it, a remap of
//     every body this does not make). A scene without groups shows one, Default.
//   - the scene script block's level properties: its Level class's properties by their kinds, as
//     a behaviour's are (ScriptPropertyRows), each an override on the block while it differs.
// Every edit is the edit context's MutateSceneSettings: one undo step.

module;
#include "Core/Prelude.h"

export module editor.scene:settings_extra_sources;

import foundation.core;
import foundation.scene;
import foundation.script.resource;
import foundation.gui;
import foundation.gui.toolkit;
import engine.physics;
import engine.script;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    class CollisionGroupsSource final : public foundation::gui::IPropertySource
    {
    public:
        explicit CollisionGroupsSource(SceneEditContext& edit);

        /// Tells the grid when the groups changed outside it (an undo).
        void Refresh();

        // === IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override { return 1; }
        void GetProperties(StringView parent, Array<foundation::gui::PropertyInfo>& out) const override;
        [[nodiscard]] foundation::gui::Variant GetValue(StringView path, usize object) const override;
        void BeginEdit(StringView) override {}
        void PreviewEdit(StringView, const foundation::gui::PropertyWrite&) override {}
        void CommitEdit(StringView path, const foundation::gui::PropertyWrite& write) override;
        void CancelEdit(StringView) override {}
        bool EditList(StringView path, const foundation::gui::ListEdit& edit) override;
        [[nodiscard]] foundation::gui::Variant GetDefault(StringView, usize) const override { return foundation::gui::Variant{}; }
        bool Revert(StringView) override { return false; }
        [[nodiscard]] String GetRevertText(StringView) const override { return String{}; }
        [[nodiscard]] String CopyValue(StringView) const override { return String{}; }
        bool PasteValue(StringView, StringView, String& reason) override
        {
            reason = String(u8"A collision group does not paste");
            return false;
        }

    private:
        // A path's group (-1: none) and row ("name", "collides"; empty: the group itself).
        struct Where
        {
            bool list = false;
            i64 group = -1;
            String row;
        };
        [[nodiscard]] static Where Parse(StringView path);
        [[nodiscard]] const engine::physics::PhysicsSceneSettings* Settings() const;
        [[nodiscard]] usize Count() const;
        [[nodiscard]] String NameOf(usize group) const;
        [[nodiscard]] u32 MaskOf(usize group) const;
        [[nodiscard]] u64 Signature() const;
        void Mutate(const Function<void(engine::physics::PhysicsSceneSettings&)>& mutate);

        SceneEditContext* m_edit;
        u64 m_signature = 0;
    };

    class LevelScriptSource final : public foundation::gui::IPropertySource
    {
    public:
        LevelScriptSource(SceneEditContext& edit, EditorContext* context);

        /// Tells the grid when the level's class or values changed outside it (an undo, a cook).
        void Refresh();

        // === IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override { return 1; }
        void GetProperties(StringView parent, Array<foundation::gui::PropertyInfo>& out) const override;
        [[nodiscard]] foundation::gui::Variant GetValue(StringView path, usize object) const override;
        void BeginEdit(StringView) override {}
        void PreviewEdit(StringView, const foundation::gui::PropertyWrite&) override {}
        void CommitEdit(StringView path, const foundation::gui::PropertyWrite& write) override;
        void CancelEdit(StringView) override {}
        bool EditList(StringView, const foundation::gui::ListEdit&) override { return false; }
        [[nodiscard]] foundation::gui::Variant GetDefault(StringView path, usize object) const override;
        bool Revert(StringView path) override;
        [[nodiscard]] String GetRevertText(StringView) const override { return String(u8"Revert to the script's default"); }
        [[nodiscard]] String CopyValue(StringView) const override { return String{}; }
        bool PasteValue(StringView, StringView, String& reason) override
        {
            reason = String(u8"A level property does not paste");
            return false;
        }

    private:
        [[nodiscard]] const engine::script::SceneScriptSettings* Settings() const;
        [[nodiscard]] const foundation::script::ScriptClass* Class() const;
        [[nodiscard]] u64 Signature() const;
        void Mutate(const Function<void(engine::script::SceneScriptSettings&)>& mutate);

        SceneEditContext* m_edit;
        EditorContext* m_context;
        u64 m_signature = 0;
    };
}
