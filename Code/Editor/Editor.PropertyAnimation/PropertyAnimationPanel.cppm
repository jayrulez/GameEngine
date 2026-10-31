// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:panel` partition.
//
// PropertyAnimationPanel authors property animation in the scene, built on the GUI from its markup
// (Data/Editor/PropertyAnimationPanel.xml). The scene page holds it under its viewport. The panel
// edits a clip asset of its own, never the scene: it has its own history, Undo, Redo and Save on
// its bar, and its own unsaved mark. While it holds the focus it handles its own keys: the action
// commands it binds (Undo, Redo, Save) are handled on its root, so Ctrl+Z, Ctrl+Y and Ctrl+S act
// on the clip, and a key it does not bind goes on to the scene page and the editor (the GUI's
// commands bubble along the focus path).
//
// The ruler, the dopesheet and the curve view share one time axis over the clip (:clip_source).
// The curve view shows the curves of the chosen tracks (the tracks of the keys chosen in the
// dopesheet), or every track's when none is chosen. Scrubbing or playing poses the bound entity
// (:clip_preview); the entity is restored when the preview stops, and the preview stands down
// while the scene simulates. The bound entity is the panel's own choice (Use Selected binds the
// scene's selection, and Pick chooses from the scene's entities), not whatever is currently
// selected. While the preview poses the entity, the page marks it in the view. Opening a clip,
// making one, picking an entity and asking about an unsaved clip belong to the page (its dialogs),
// through the hooks below.

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
    /// The panel's markup, at namespace scope for another library to read (MSVC does not export a
    /// class's static constexpr member from a shared library).
    inline constexpr StringView kPropertyAnimationPanelPath = u8"Editor/PropertyAnimationPanel.xml";

    class PropertyAnimationPanel
    {
    public:
        static constexpr StringView kPath = kPropertyAnimationPanelPath;

        /// Builds the panel over `scene` (the page's, which outlives it) from its loaded markup
        /// `content`.
        PropertyAnimationPanel(EditorContext& context, foundation::scene::Scene& scene, RefPtr<foundation::gui::Node> content);
        ~PropertyAnimationPanel();
        PropertyAnimationPanel(const PropertyAnimationPanel&) = delete;
        PropertyAnimationPanel& operator=(const PropertyAnimationPanel&) = delete;

        [[nodiscard]] RefPtr<foundation::gui::Node> Content() const { return m_content; }

        // === Hooks the page sets ===
        /// The scene's selected entity (Use Selected binds it); unset means none.
        Function<Guid()> SelectedEntity;
        /// New and Open open the page's dialogs, which end by calling LoadClip. When unset, the
        /// buttons do nothing.
        Function<void()> RequestNewClip;
        Function<void()> RequestOpenClip;
        /// Runs `proceed` now, or once the user has saved or discarded the unsaved clip (the page's
        /// prompt); when unset, runs it at once.
        Function<void(Function<void()>)> GuardUnsaved;
        /// The bar's Close: the page hides the panel (the clip stays open in it). When unset,
        /// nothing happens.
        Function<void()> RequestClose;
        /// The bar's Pick: opens the page's entity picker over the scene with `current` highlighted;
        /// `chosen` receives the pick. When unset, the button does nothing.
        Function<void(const Guid& current, Function<void(const Guid&)> chosen)> RequestPickEntity;

        // === The clip ===
        /// Edits the clip asset `clipId` (false when there is no such clip), with the transport
        /// stopped at the start.
        bool LoadClip(const Guid& clipId);
        /// Asks about an unsaved clip, then opens `clipId` and binds `entity` (nil keeps the bound
        /// entity).
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
        /// Adds a track for `seed`, keyed at the playhead with the entity's current value (when it
        /// can be read).
        void AddTrack(const AnimatablePropertyInfo& seed);
        /// What Add Track offers: the bound entity's properties without a track yet.
        [[nodiscard]] Array<AnimatablePropertyInfo> OfferedTracks() const;
        /// The tracks of the keys chosen in the dopesheet.
        [[nodiscard]] Array<usize> ChosenTracks() const;
        /// Keys the chosen tracks (every track when none is chosen) at the playhead with the
        /// entity's current values, as one undo step; returns how many it keyed.
        usize KeyChosen();
        /// Removes the chosen tracks as one undo step; false when none is chosen.
        bool RemoveChosen();

        // === The transport ===
        void Play();
        void Pause();
        void Stop();
        void Restart();
        void SetLooping(bool looping);
        /// Moves the playhead to `time`, as scrubbing the ruler does.
        void Scrub(f32 time);
        /// Frames the shown curves in time and value (the bar's Frame).
        void FrameCurves();

        /// Called once a frame by the page: updates the transport, the preview (stood down while
        /// `simulating`) and the bar's state.
        void Update(f32 dt, bool simulating);
        /// Restores what the preview posed (when the page saves the scene or starts a simulation);
        /// Repose poses it again at the playhead.
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
        // Fits the curve view's values to the curves it shows. It never fits time, because the
        // dopesheet shares that axis and a choice made there must not rezoom it under the mouse.
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
        bool m_moving = false;        // set while the transport moves the playhead, so it is not a scrub
        Array<usize> m_shownTracks;   // the tracks the curve view shows, as last chosen
        bool m_fitValues = true;      // what the curve view shows changed, so fit its values
        usize m_trackCount = 0;       // the clip's track count, as last shown
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
