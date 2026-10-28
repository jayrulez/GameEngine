// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:clip_page` partition.
//
// AnimationClipEditorPage: an animation clip on the GUI from its markup
// (Data/Editor/AnimationClipPage.gml; Documentation/Specs/editor-gui.md, 5.4, layer 10, part 2). The
// preview plays the cooked clip on a skeleton (the bones, and a skinned mesh over them when one is
// picked), both chosen on the preview's bar and kept per clip with the project; the skeleton the
// importer named is the one it starts on. Travel, the preview's own choice (it had been among the
// clip's rows), walks it by the clip's root motion; off, it plays in place, as it is cooked, the
// path drawn on the ground either way. The kit's transport plays it; under the preview, the ruler on
// the clip's time scrubs it, and the clip's events are its markers: one chosen opens its rows, one
// dragged moves its time, one step.
//
// The clip's tracks and keys come from its import and nothing here edits them, so the rows are the
// page's own, not its reflection's: under Clip whether it loops; under Root motion the bone it is
// read from and what it extracts (travel, height, turning); under Events a list, each a time and a
// name (a new one at the playhead), named by its name and summed up by its time while closed. Its
// facts (its name, duration, tracks and keys) are read-only rows. Each edit is one undo step (the
// whole asset before and after). Clips come from import: there is no creator.

module;
#include "Core/Prelude.h"

export module editor.animation:clip_page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :preview;

using namespace foundation::core;

export namespace editor
{
    class AnimationClipEditorPage final : public app::AssetObjectPage, public IPlaybackPage, public foundation::gui::ITimeMarkerSource
    {
    public:
        static constexpr StringView kPath = u8"Editor/AnimationClipPage.gml";

        AnimationClipEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~AnimationClipEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // === IPlaybackPage: the preview's clock, never the clip's ===
        [[nodiscard]] bool CanPlay() const override;
        [[nodiscard]] bool IsPlaying() const override { return m_playing; }
        void Play() override;
        void Pause() override { m_playing = false; }
        void Stop() override;
        void Restart() override;

        // === ITimeMarkerSource: the clip's events on the ruler ===
        [[nodiscard]] usize MarkerCount() const override;
        [[nodiscard]] foundation::gui::TimeMarker GetMarker(usize marker) const override;
        void BeginMarkerMove(usize marker) override;
        void PreviewMarkerMove(usize marker, f32 time) override;
        void CommitMarkerMove() override;
        void CancelMarkerMove() override;

        /// Where the preview is in the clip (seconds); a scrub of the ruler moves it.
        [[nodiscard]] f32 Time() const noexcept { return m_time; }
        void SetTime(f32 seconds);
        /// The preview walking by the clip's root motion (the bar's Travel).
        void SetTravel(bool travel);
        [[nodiscard]] bool Travels() const noexcept { return m_travel; }
        /// The event `index` chosen: its rows opened in the grid, its marker the ruler's chosen one.
        void ChooseEvent(i32 index);

        // === For tests ===
        [[nodiscard]] pipeline::AnimationClipAsset* Clip() const;
        [[nodiscard]] foundation::gui::PropertyBag& ClipRows() noexcept { return *m_clipRows; }
        [[nodiscard]] foundation::gui::PropertyBag& RootRows() noexcept { return *m_rootRows; }
        [[nodiscard]] foundation::gui::PropertyBag& EventRows() noexcept { return *m_eventRows; }
        [[nodiscard]] AnimationPreviewRig* Rig() const noexcept { return m_rig.Get(); }
        [[nodiscard]] foundation::gui::TimeRuler* Ruler() const noexcept { return m_ruler; }
        /// The clip's length as the ruler and the clock take it: the cooked clip's, else the source's.
        [[nodiscard]] f32 Length() const;

    protected:
        void OnObjectChanged() override;

    private:
        void BuildRows();
        void ShowRows();
        void ShowFacts();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void EditEvents(const foundation::gui::ListEdit& edit);
        void ShowTransport();
        void FollowAxis();
        void Pose();
        void FramePreview();
        void LoadPreviewPref();
        void SavePreviewPref();

        UniquePtr<foundation::gui::PropertyBag> m_clipRows;  // Clip: looping
        UniquePtr<foundation::gui::PropertyBag> m_rootRows;  // Root motion
        UniquePtr<foundation::gui::PropertyBag> m_eventRows; // Events
        UniquePtr<app::PreviewPane> m_pane;
        UniquePtr<AnimationPreviewRig> m_rig;
        foundation::resource::Proxy<foundation::animation::AnimationClip> m_clip; // the cooked product
        RefPtr<foundation::gui::TimeAxis> m_axis;
        foundation::gui::TimeRuler* m_ruler = nullptr;
        foundation::gui::Label* m_state = nullptr;
        foundation::gui::ButtonBase* m_travelButton = nullptr;
        Array<foundation::animation::BoneTransform> m_pose;
        f32 m_time = 0.0f;
        bool m_playing = true;
        bool m_travel = false;
        bool m_moving = false;   // the clock moving the playhead: not a scrub
        f32 m_rulerWidth = 0.0f; // the ruler's, as last fitted
        f32 m_fittedLength = 0.0f;
        i32 m_markerMoving = -1; // the event a marker drag moves
        f32 m_markerFrom = 0.0f; // its time when the drag began
    };

    class AnimationClipPageFactory final : public IEditorPageFactory
    {
    public:
        explicit AnimationClipPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The clip page (asking for its services when it is made) and its preview prefs' section.
    void RegisterAnimationClipEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
