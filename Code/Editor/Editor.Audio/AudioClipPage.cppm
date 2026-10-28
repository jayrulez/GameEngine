// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the `editor.audio` module.
//
// AudioClipEditorPage: the audition page of an AudioClipAsset, on the GUI from its markup
// (Data/Editor/AudioClipPage.xml; Documentation/Specs/editor-gui.md, 5.4, layer 5). The clip's
// peak waveform (decoded from its source file, the bytes the cook writes through) with the kit's
// transport over it: Play, Stop and Restart are the playback actions over the page, auditioned
// through the RUNTIME context's AudioSubsystem (the game's engine, buses and voices, so what is
// heard is what the game plays), honouring the clip's loop, a playhead following the voice. Beside
// it the clip's options on the grid, each edit one undo step, the audition following them (a loop
// turned on loops the next play), and its source's facts read only (channels, rate, length). The
// old page edited nothing itself; its options were the inspector's.
//
// The sound cue page is its :sound_cue_page partition, the bus layout page its :bus_layout_page
// (the rules it edits by its :bus_layout_model).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.audio;

export import :waveform_view;
export import :audition;
export import :sound_cue_page;
export import :bus_layout_model;
export import :bus_layout_page;
export import :thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.audio;
import audio.pipeline;
import engine.audio;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace audio = foundation::audio;

    class AudioClipEditorPage final : public app::AssetObjectPage, public IPlaybackPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/AudioClipPage.xml";
        static constexpr usize kPeakBuckets = 256;

        AudioClipEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~AudioClipEditorPage() override;

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnClose() override { StopAudition(); }

        // === IPlaybackPage ===
        [[nodiscard]] bool CanPlay() const override { return m_clip.Get() != nullptr && m_voice && m_voice->HasEngine(); }
        [[nodiscard]] bool IsPlaying() const override { return m_voice && m_voice->IsPlaying(); }
        void Play() override;
        void Pause() override;
        void Stop() override { StopAudition(); }
        void Restart() override { Audition(); }

        /// The audition's volume (this page's voice only, not saved), 0 to 1.
        void SetAuditionVolume(f32 volume);
        [[nodiscard]] f32 AuditionVolume() const noexcept { return m_voice ? m_voice->Volume() : 1.0f; }

        // === For tests ===
        [[nodiscard]] const audio::AudioClip* Clip() const noexcept { return m_clip.Get(); }
        [[nodiscard]] WaveformView* Waveform() const noexcept { return m_waveform.Get(); }
        [[nodiscard]] StringView State() const;

    protected:
        void OnObjectChanged() override;

    private:
        /// Reads the source file named by the asset: the clip to audition, its peaks and its facts;
        /// the waveform's place says why when there is none.
        void LoadClip();
        /// The clip's options as the asset has them now (an edit, an undo): the next play takes them.
        void ApplyOptions();
        void Audition();
        void StopAudition();
        void ShowState(StringView state, StringView text);
        void ShowEmpty(StringView why);

        UniquePtr<AuditionVoice> m_voice; // through the runtime context's audio (none in a test)
        RefPtr<audio::AudioClip> m_clip;
        String m_loadedFile; // the source the clip was read from (a new one reads it again)
        RefPtr<WaveformView> m_waveform;
        foundation::gui::Node* m_stage = nullptr;
        foundation::gui::Node* m_empty = nullptr;
        foundation::gui::Label* m_emptyText = nullptr;
        foundation::gui::Label* m_state = nullptr;
        foundation::gui::Slider* m_volume = nullptr;
    };

    class AudioClipPageFactory final : public IEditorPageFactory
    {
    public:
        explicit AudioClipPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The audio pages' registration (from the editor's assembly), each page asking for its
    /// services when it is made; the clip thumbnails with them.
    inline void RegisterAudioClipEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<AudioClipPageFactory>(services), editor::EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<SoundCuePageFactory>(services), editor::EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<AudioBusLayoutPageFactory>(services), editor::EditorRootAllocator()));
        if (context.Thumbnails() != nullptr)
            RegisterAudioThumbnailGenerator(*context.Thumbnails());
    }
}
