// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Render - :settings_profile_page partition.
//
// SettingsProfilePage edits a profile asset (an Environment Profile, a Post Process Profile, or any
// other pipeline::SettingsProfileAsset). It is built from the preview pages' markup
// (Data/Editor/PreviewPage.xml): a preview rendered with the profile applied, beside the profile's
// values. The values show as a scene's settings rows do, from the settings type's reflection,
// without the block's own source and profile fields, which belong to a scene.
//
// The preview is a built-in scene (a ground, spheres of a few materials, a cube and a sun) or one
// of the project's scenes, picked on the preview's bar, since shared profiles are tuned by how they
// look in the levels. A project scene loads as the scene page opens it, seen from where that page's
// camera was left, with the profile's values over the scene's own. The choice is remembered per
// profile (ProfilePreviewSettings). Each edit is one undo step. Save writes the asset and cooks it,
// so every scene using the profile picks up the change.
//
// The interface names no render type (the asset is held as the ISerializable it was read as). GCC
// 15's module merger has crashed on page interfaces that import both editor.preview and the render
// modules, so those imports stay in the implementation unit.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.render:settings_profile_page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.resource; // ResourceManager (the preview binds the profile's references)
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

export namespace editor
{
    /// Gives `scene`'s settings block the values of `profileAsset` (a SettingsProfileAsset; the
    /// block is the one whose settings type is the profile's values type) and binds its
    /// references through `resources` when given. False when either is missing.
    bool ApplySettingsProfileToScene(ISerializable& profileAsset, foundation::scene::Scene& scene,
                                     foundation::resource::ResourceManager* resources);

    /// Like ApplySettingsProfileToScene, for a scene loaded to preview the profile on. The block is
    /// first switched to its own values, so a scene that takes this profile (or another) by
    /// reference shows the values being edited rather than the cooked ones.
    bool PreviewSettingsProfileInScene(ISerializable& profileAsset, foundation::scene::Scene& scene,
                                       foundation::resource::ResourceManager* resources);

    /// The scene a profile is previewed on, per profile (nil: the built-in scene).
    struct ProfilePreviewPref
    {
        Guid profile;
        Guid scene;

        void Serialize(ISerializer& ar)
        {
            ar.Key("profile");
            ar.GuidValue(profile);
            ar.Key("scene");
            ar.GuidValue(scene);
        }
    };

    inline void Serialize(ISerializer& ar, ProfilePreviewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }

    /// The profile pages' preview choices, in the per-project editor store.
    class ProfilePreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(ProfilePreviewSettings, ISerializable)
    public:
        Array<ProfilePreviewPref> prefs;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "prefs", prefs); }
    };

    RTTI_DEFINE_OBJECT_VERSIONED(ProfilePreviewSettings, "rtti::editor::editor.scene", 1)

    class SettingsProfilePage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/PreviewPage.xml";

        SettingsProfilePage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~SettingsProfilePage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;
        void OnAssetExternallyModified() override;

        /// The scene the profile is previewed on (nil for the built-in one). Picking another
        /// reloads the preview and remembers the choice for this profile.
        [[nodiscard]] const Guid& PreviewSceneId() const noexcept { return m_previewSceneId; }
        void SetPreviewScene(const Guid& scene);

        // === For tests ===
        /// The profile's rows: its values, without the fields that belong to a scene.
        [[nodiscard]] foundation::gui::IPropertySource& Rows() noexcept;
        /// Edits the values' property `path` to `write`, as the grid commits an edit.
        void EditValue(StringView path, const foundation::gui::PropertyWrite& write);
        [[nodiscard]] app::PreviewPane* Pane() const noexcept { return m_pane.Get(); }
        /// How often the values were given to the preview scene (a test has no scene to look at).
        [[nodiscard]] usize PreviewsApplied() const noexcept { return m_previewsApplied; }

    protected:
        void OnObjectChanged() override;

    private:
        class ValuesSource;

        [[nodiscard]] PreviewViewport* Preview() const { return m_pane ? m_pane->Viewport() : nullptr; }
        void BuildPreviewScene();
        /// Loads the preview's content: the picked scene, seen from where its page's camera was
        /// left (or framed whole), or the built-in scene when none is picked or the picked one is
        /// gone.
        void LoadPreviewContent();
        void LoadPreviewPref();
        void SavePreviewPref();
        void ApplyPreview();
        void ShowScene();
        void BindValues();

        UniquePtr<app::PreviewPane> m_pane;
        RefPtr<app::AssetPickerSlot> m_sceneSlot; // on the preview's bar
        UniquePtr<ValuesSource> m_values;
        app::XmlPropertyTextFormat m_format;
        Guid m_previewSceneId; // the project scene previewed on; nil = the built-in one
        Array<RefPtr<RefCounted>> m_previewContent; // the preview's runtime meshes and materials
        usize m_previewsApplied = 0;
    };

    class SettingsProfilePageFactory final : public IEditorPageFactory
    {
    public:
        explicit SettingsProfilePageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        /// Every profile asset (the registry matches along the asset type's base chain).
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the profile page (called from the editor's assembly, beside the material page).
    /// Each page asks `services` for its services when it is made.
    void RegisterSettingsProfileEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
