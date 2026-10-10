// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :settings_profile_page partition.
//
// SettingsProfilePage: edits a profile asset (an Environment Profile, a Post Process Profile;
// any pipeline::SettingsProfileAsset) - a preview on the left rendered with the profile applied,
// the profile's values on the right in the same rows a scene's settings section shows
// (SettingsRows). The preview is a built-in scene (a ground, spheres of a few materials, a cube
// and a sun) or one of the project's scenes, picked in the Preview row (user 2026-10-09: shared
// profiles are tuned by how they look in the levels): it loads as the scene page opens it, seen
// from where that page's camera was left, the profile's values over the scene's own. The choice
// is remembered per profile (ProfilePreviewSettings). Save writes the asset and cooks it, so every
// scene using the profile picks the change up.
//
// Edits are blob-snapshot commands like the material page's: the whole asset serialized before
// and after (it is small), consecutive scrubs of one field merged, each apply re-applying the
// values to the preview.
//
// The interface names no render type (the asset is held as the ISerializable it was read as):
// GCC 15's module merger has crashed on page interfaces importing both editor.preview and the
// render modules, so they stay in the implementation unit.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:settings_profile_page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.resource; // ResourceManager (the preview binds the profile's references)
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import editor.core;
import editor.app;
import editor.preview; // PreviewViewport (viewport + preview scene + camera + render loop)

using namespace foundation::core;

export namespace editor
{
    /// Gives `scene`'s settings block the values of `profileAsset` (a SettingsProfileAsset; the
    /// block is the one whose settings type is the profile's values type) and binds its
    /// references through `resources` when given. False when either is missing.
    bool ApplySettingsProfileToScene(ISerializable& profileAsset, foundation::scene::Scene& scene,
                                     foundation::resource::ResourceManager* resources);

    /// As ApplySettingsProfileToScene, on a scene loaded to preview the profile on: the block is
    /// first turned to its own values, so a scene that takes this profile (or another) by
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

    class SettingsProfilePage final : public app::UIEditorPage
    {
    public:
        SettingsProfilePage(EditorContext& context, foundation::runtime::IApplicationHost& host,
                            foundation::ui::runtime::UIHost& uiHost,
                            foundation::content::Instance& instance);
        ~SettingsProfilePage();

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        void OnUpdate(foundation::runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;
        [[nodiscard]] Status Save() override;
        void OnClose() override;
        void OnAssetExternallyModified() override;

        // The command stack's apply path (Execute and Undo both land here): the asset read from
        // `blob`, the preview given its values.
        void ApplyAssetBlob(const Array<byte>& blob);
        [[nodiscard]] Array<byte> SnapshotAsset() const;

        /// The scene the profile is previewed on (nil: the built-in one), and picking another:
        /// the preview reloads and the choice is remembered for this profile.
        [[nodiscard]] const Guid& PreviewSceneId() const noexcept { return m_previewSceneId; }
        void SetPreviewScene(const Guid& scene);

    private:
        class EditProfileCommand final : public IEditorCommand
        {
        public:
            EditProfileCommand(SettingsProfilePage& page, StringView mergeKey, Array<byte> before,
                               Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }
            [[nodiscard]] bool Execute() override
            {
                m_page->ApplyAssetBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplyAssetBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_settings_profile"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditProfileCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            SettingsProfilePage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        // One undoable edit of the profile's values: snapshot, mutate, snapshot, push.
        void ApplyEdit(StringView mergeKey, Function<void(void* values)> mutate);
        void BuildPreviewScene();
        /// The preview's content: the picked scene, loaded and seen from where its page's camera
        /// was left (else framed whole), or the built-in scene when none is picked or it is gone.
        void LoadPreviewContent();
        void LoadPreviewPref();
        void SavePreviewPref();
        void ApplyPreview();
        void RebuildGrid();

        EditorContext* m_context;
        String m_title;
        RefPtr<ISerializable> m_asset; // a pipeline::SettingsProfileAsset (null: failed to read)

        UniquePtr<PreviewViewport> m_preview;
        Guid m_previewSceneId; // the project scene previewed on; nil = the built-in one
        Array<RefPtr<RefCounted>> m_previewContent; // the preview's runtime meshes and materials

        RefPtr<foundation::ui::toolkit::PropertyGrid> m_grid;
        RefPtr<foundation::ui::View> m_content;
        RefPtr<app::PageToolbar> m_toolbar;
        Array<Function<void()>> m_refreshers;
    };

    class SettingsProfilePageFactory final : public IEditorPageFactory
    {
    public:
        SettingsProfilePageFactory(foundation::runtime::IApplicationHost& host,
                                   foundation::ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }
        /// Every profile asset (the registry matches along the asset type's base chain).
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        foundation::runtime::IApplicationHost* m_host;
        foundation::ui::runtime::UIHost* m_uiHost;
    };

    /// The profile page's registration (from the editor's assembly, beside the material page's).
    void RegisterSettingsProfileEditor(EditorContext& context,
                                       foundation::runtime::IApplicationHost& host,
                                       foundation::ui::runtime::UIHost& uiHost);
}
