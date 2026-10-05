// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :animation_clip_page partition.
//
// AnimationClipEditorPage: preview + light
// authoring for an AnimationClipAsset. A skeleton-wireframe viewport plays the COOKED clip product
// (pick a Skeleton, scrub or play the timeline); the inspector edits the source's loop flag and
// its animation EVENTS (time + name rows, add/remove) with blob-snapshot undo, and reads out the
// stats (duration, tracks, keys). Save writes the asset + recooks so bound proxies hot-swap.
//
// Clips are IMPORTED (model importer), so there is no New-Asset creator here.

module;
#include "Core/Prelude.h"

export module editor.scene:animation_clip_page;

import foundation.core;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.geometry; // StaticMesh (the skinned preview mesh)
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace vg = foundation::vg;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace animation = foundation::animation;

    class AnimationClipEditorPage final : public app::UIEditorPage, public IPlaybackPage
    {
    public:
        /// Event `index`'s section in the grid: "Event N".
        [[nodiscard]] static String EventSection(usize index);

        AnimationClipEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                ui::runtime::UIHost& uiHost, foundation::content::Instance& instance);

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override;

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;

        // ---- IPlaybackPage: the preview's clock ----
        [[nodiscard]] bool CanPlay() const override;
        [[nodiscard]] bool IsPlaying() const override { return m_playing; }
        void Play() override;
        void Pause() override { m_playing = false; }
        void Stop() override;
        void Restart() override;
        void OnClose() override;

        // Record a coalesced undo step for an in-place edit that already happened (merge by key).
        void CommitEdit(StringView mergeKey);

    private:
        class EditClipCommand final : public IEditorCommand
        {
        public:
            EditClipCommand(AnimationClipEditorPage& page, StringView mergeKey, Array<byte> before,
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
            [[nodiscard]] StringView TypeId() const override { return u8"edit_animclip"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditClipCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            AnimationClipEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        void BuildPreviewScene();
        // The preview rig's skeleton and skinned mesh (nil clears); the slots show them.
        void SetPreviewSkeleton(const Guid& id);
        void SetPreviewMesh(const Guid& id);
        void RebuildGrid(); // stats + loop flag + events editor
        void SyncTimeSlider(); // the scrub slider shows m_time, without reading as a scrub
        void UpdatePreview(f32 dt);

        // Persist / restore the preview rig (skeleton + skinned mesh) per clip (project settings).
        void LoadPreviewPref();
        void SavePreviewPref();

        [[nodiscard]] Array<byte> SnapshotAsset() const;
        void ApplyAssetBlob(const Array<byte>& blob);
        void QueueStructural(StringView undoKey, Function<void()> mutate);

        [[nodiscard]] ui::UIContext* Ctx() const;

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        RefPtr<pipeline::AnimationClipAsset> m_asset;
        foundation::resource::Proxy<animation::AnimationClip> m_clip; // cooked product (hot-swaps)

        // preview world (shared substrate: viewport + preview scene + camera + render loop)
        UniquePtr<PreviewViewport> m_preview;

        RefPtr<app::CompactAssetSlot> m_skeletonSlot;
        RefPtr<app::PageToolbar> m_toolbar;
        RefPtr<ui::Slider> m_timeSlider; // normalized [0..1] scrub
        RefPtr<ui::Label> m_timeLabel;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<foundation::ui::View> m_content;

        Guid m_skeletonGuid{};
        foundation::resource::Proxy<animation::Skeleton> m_skeleton;
        Array<animation::BoneTransform> m_poseScratch;

        // Skinned preview mesh (optional): deformed by an AnimationPlayer driven from m_time and
        // fed to a MeshComponent. Null = skeleton wireframe only.
        RefPtr<app::CompactAssetSlot> m_meshSlot;
        Guid m_previewMeshId{};
        foundation::resource::Proxy<foundation::geometry::StaticMesh> m_previewMesh;
        scene::EntityHandle m_meshEntity;
        UniquePtr<animation::AnimationPlayer> m_previewPlayer;
        animation::Skeleton* m_playerSkeleton = nullptr; // the skeleton the player was built for
        animation::AnimationClip* m_playerClip = nullptr; // the clip last handed to the player
        Array<Float4x4> m_worldScratch;
        Array<byte> m_undoBaseline;
        f32 m_time = 0.0f; // seconds into the clip
        bool m_playing = true;
        bool m_scrubbing = false; // slider writes m_time; playback writes the slider
        // The preview plays a clip's stripped pose in place; on, it travels by the clip's root
        // motion instead (page state, not the clip's).
        bool m_showTravel = false;
    };

    class AnimationClipPageFactory final : public IEditorPageFactory
    {
    public:
        AnimationClipPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Registers the AnimationClip page factory (no creator - clips come from import).
    void RegisterAnimationClipEditor(EditorContext& context, runtime::IApplicationHost& host,
                                     ui::runtime::UIHost& uiHost);
}
