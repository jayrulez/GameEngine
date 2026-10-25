// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_settings_source partition.
//
// A scene system's settings block as a property source, for the inspector while the scene is
// selected (ruling 12; Documentation/Specs/editor-gui.md, 5.3, layer 4, part 4). A block's values
// are its own, or a profile's while it uses one (shared by every scene that does): each property
// shows and edits where the edit context says it lands (SettingsEditProfile: the profile's values,
// or the block's own for its scene-only fields, its source and profile reference). Two reflected
// sources of the block's type stand for the two, and each call goes to the one its property's
// top-level name lands in. Every edit is one undo step, restored by its text on the values found
// again (scene blocks and profiles may move); an edit that lands in a profile is noted, so the
// page's save writes it to the profile's asset.

module;
#include "Core/Prelude.h"

export module editor.scene:scene_settings_source;

import foundation.core;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    class SceneSettingsSource final : public foundation::gui::IPropertySource
    {
    public:
        SceneSettingsSource(IAllocator& allocator, SceneEditContext& edit, const TypeInfo& settingsType, foundation::gui::IPropertyTextFormat& format);
        ~SceneSettingsSource() override;
        SceneSettingsSource(const SceneSettingsSource&) = delete;
        SceneSettingsSource& operator=(const SceneSettingsSource&) = delete;

        [[nodiscard]] const TypeInfo& Type() const noexcept { return *m_type; }
        /// The values found again (a block or a profile may have moved), the grid told.
        void Refresh();

        // === IPropertySource ===
        [[nodiscard]] usize ObjectCount() const override { return 1; }
        void GetProperties(StringView parent, Array<foundation::gui::PropertyInfo>& out) const override;
        [[nodiscard]] foundation::gui::Variant GetValue(StringView path, usize object) const override;
        void BeginEdit(StringView path) override;
        void PreviewEdit(StringView path, const foundation::gui::PropertyWrite& write) override;
        void CommitEdit(StringView path, const foundation::gui::PropertyWrite& write) override;
        void CancelEdit(StringView path) override;
        bool EditList(StringView path, const foundation::gui::ListEdit& edit) override;
        [[nodiscard]] foundation::gui::Variant GetDefault(StringView path, usize object) const override;
        bool Revert(StringView path) override;
        [[nodiscard]] String GetRevertText(StringView path) const override;
        [[nodiscard]] String CopyValue(StringView path) const override;
        bool PasteValue(StringView path, StringView text, String& reason) override;

    private:
        class Forwarder;
        // The profile `path`'s top-level property lands in (nil: the block's own values).
        [[nodiscard]] Guid ProfileOf(StringView path) const;
        [[nodiscard]] foundation::gui::ReflectedPropertySource* SourceOf(StringView path) const;
        void Bind();
        void Changed(const Guid& profile, const foundation::gui::ReflectedChange& change);

        IAllocator& m_allocator;
        SceneEditContext* m_edit;
        const TypeInfo* m_type;
        foundation::gui::IPropertyTextFormat* m_format;
        UniquePtr<foundation::gui::ReflectedPropertySource> m_own;     // the block's own values
        UniquePtr<foundation::gui::ReflectedPropertySource> m_profile; // a profile's, while it uses one
        UniquePtr<Forwarder> m_forwarder;
        void* m_ownValues = nullptr;
        void* m_profileValues = nullptr;
        Guid m_profileId;
    };
}
