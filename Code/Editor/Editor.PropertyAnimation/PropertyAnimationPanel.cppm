// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:panel` partition.
//
// PropertyAnimationPanel: property animation authored in the scene, on the GUI from its markup
// (Data/Editor/PropertyAnimationPanel.xml; Documentation/Specs/editor-gui.md, 5.4, layer 9, part 2).
// The scene page holds it under its viewport; the panel edits a clip asset of its own, never the
// scene: its own history, Undo, Redo and Save on its bar, its own unsaved mark (user 2026-10-27).
// While it holds the focus the keys are its own: the actions' commands it binds (Undo, Redo, Save)
// are handled on its root, so Ctrl+Z, Ctrl+Y and Ctrl+S act on the clip, and a key it does not
// bind goes on to the scene page and the editor (the GUI's commands bubble along the focus path).
//
// The ruler, the dopesheet and the curve view share one time axis over the clip (:clip_source); the
// curve view shows the chosen tracks' curves (the tracks of the keys chosen in the dopesheet), every
// track's with none chosen. Scrubbing or playing poses the bound entity (:clip_preview), which is
// put back as it was when the preview stops, and stands down while the scene simulates. The bound
// entity is the panel's own choice (Use Selected binds the scene's selection), not the drifting
// selection, or Pick's choice from the scene's entities. While its preview poses the entity, the
// page marks it in the view. Opening, making a clip, picking an entity and asking about an unsaved
// one are the page's (its dialogs), through the seams below.

module;
#include "Core/Prelude.h"

export module editor.propertyanimation:panel;

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :clip_document;
import :clip_source;
import :clip_preview;

using namespace foundation::core;

export namespace editor
{
    class PropertyAnimationPanel
    {
    public:
        static constexpr StringView kPath = u8"Editor/PropertyAnimationPanel.xml";

        /// Over `scene` (the page's, which outlives it), its markup `content` loaded.
        PropertyAnimationPanel(EditorContext& context, foundation::scene::Scene& scene, RefPtr<foundation::gui::Node> content);
        ~PropertyAnimationPanel();
        PropertyAnimationPanel(const PropertyAnimationPanel&) = delete;
        PropertyAnimationPanel& operator=(const PropertyAnimationPanel&) = delete;

        [[nodiscard]] RefPtr<foundation::gui::Node> Content() const { return m_content; }

        // === The page's seams ===
        /// The scene's selected entity (Use Selected binds it); unset: none.
        Function<Guid()> SelectedEntity;
        /// New and Open: the page's dialogs, which end by LoadClip. Unset: the buttons do nothing.
        Function<void()> RequestNewClip;
        Function<void()> RequestOpenClip;
        /// Runs `proceed` now, or once the user has saved or discarded the unsaved clip (the page's
        /// prompt); unset: at once.
        Function<void(Function<void()>)> GuardUnsaved;
        /// The bar's Close: the page hides the panel (the clip stays open in it). Unset: nothing.
        Function<void()> RequestClose;
        /// The bar's Pick: the page's entity picker over the scene, `current` lit; `chosen`
        /// answers its pick. Unset: the button does nothing.
        Function<void(const Guid& current, Function<void(const Guid&)> chosen)> RequestPickEntity;

        // === The clip ===
        /// Edits the clip asset `clipId` (false: no such clip); the transport stopped, at the start.
        bool LoadClip(const Guid& clipId);
        /// Asks about an unsaved clip, then opens `clipId` and binds `entity` (nil: keeps the bound one).
        void RequestEditClip(const Guid& clipId, const Guid& entity);
        Status Save();
        void Undo();
        void Redo();
        void SetLength(f32 seconds);

        // === The entity it animates ===
        void BindEntity(const Guid& entity);
        void BindSelected();
        /// Binds the entity picked in the page's entity picker (the bar's Pick).
        void PickEntity();
        [[nodiscard]] const Guid& BoundEntity() const noexcept { return m_entity; }

        // === The tracks ===
        /// A track for `seed`, keyed at the playhead with what the entity shows (when it can be read).
        void AddTrack(const AnimatablePropertyInfo& seed);
        /// What Add Track offers: the bound entity's properties without a track yet.
        [[nodiscard]] Array<AnimatablePropertyInfo> OfferedTracks() const;
        /// The tracks of the keys chosen in the dopesheet.
        [[nodiscard]] Array<usize> ChosenTracks() const;
        /// Keys the chosen tracks (every one, with none chosen) at the playhead with what the entity
        /// shows, one step; how many it keyed.
        usize KeyChosen();
        /// Takes the chosen tracks away, one step; false with none chosen.
        bool RemoveChosen();

        // === The transport ===
        void Play();
        void Pause();
        void Stop();
        void Restart();
        void SetLooping(bool looping);
        /// The playhead moved to `time`, as a scrub of the ruler moves it.
        void Scrub(f32 time);
        /// The curves shown framed, in time and value (the bar's Frame).
        void FrameCurves();

        /// Once a frame from the page: the transport, the preview (stood down while `simulating`),
        /// the bar's state.
        void Update(f32 dt, bool simulating);
        /// The preview put back (the page saving the scene, starting a simulation); Repose poses it
        /// again where the playhead stands.
        void StopPreview() { m_preview.Stop(); }
        void Repose() { Pose(); }

        // === For tests ===
        [[nodiscard]] ClipDocument& Document() noexcept { return m_document; }
        [[nodiscard]] ClipSource& Source() noexcept { return m_source; }
        [[nodiscard]] ClipPreview& Preview() noexcept { return m_preview; }
        [[nodiscard]] ClipTransport& Transport() noexcept { return m_transport; }
        [[nodiscard]] foundation::gui::Dopesheet* Sheet() const noexcept { return m_sheet; }
        [[nodiscard]] foundation::gui::TimeAxis& Axis() const noexcept { return *m_axis; }

    private:
        void Changed();
        void Pose();
        void ShowBar();
        void ShowCurves();
        // The curve view's values fitted to the curves it shows (never its time: the dopesheet
        // shares that axis, and a choice made there must not rezoom it under the mouse).
        void FitValues();
        void OpenTrackMenu();
        [[nodiscard]] Variant EntityValue(StringView componentType, StringView propertyPath) const;
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const;

        EditorContext* m_context;
        foundation::scene::Scene* m_scene;
        RefPtr<foundation::gui::Node> m_content;
        ClipDocument m_document;
        ClipSource m_source;
        ClipPreview m_preview;
        ClipTransport m_transport;
        RefPtr<foundation::gui::TimeAxis> m_axis;
        Guid m_entity;
        bool m_simulating = false;
        bool m_moving = false;        // the transport moving the playhead: not a scrub
        Array<usize> m_shownTracks;   // the tracks the curve view shows, as last chosen
        bool m_fitValues = true;      // what the curve view shows changed: fit its values
        usize m_trackCount = 0;       // the clip's tracks, as last shown
        Guid m_shownClip;             // the clip, as last shown
        Array<u32> m_commands;        // the root's command handlers
        RefPtr<foundation::gui::Menu> m_trackMenu;

        foundation::gui::Dopesheet* m_sheet = nullptr;
        foundation::gui::CurveView* m_curves = nullptr;
        foundation::gui::Label* m_clipName = nullptr;
        foundation::gui::Node* m_dirty = nullptr;
        foundation::gui::Label* m_entityName = nullptr;
        foundation::gui::NumberField* m_length = nullptr;
        foundation::gui::Node* m_empty = nullptr;
        foundation::gui::Node* m_body = nullptr;
        foundation::gui::ButtonBase* m_save = nullptr;
        foundation::gui::ButtonBase* m_undo = nullptr;
        foundation::gui::ButtonBase* m_redo = nullptr;
        foundation::gui::ButtonBase* m_play = nullptr;
        foundation::gui::ButtonBase* m_addTrack = nullptr;
        foundation::gui::ButtonBase* m_key = nullptr;
        foundation::gui::ButtonBase* m_remove = nullptr;
        foundation::gui::ButtonBase* m_restart = nullptr;
        foundation::gui::ButtonBase* m_stop = nullptr;
    };
}
