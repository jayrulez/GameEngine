// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :sound_file_preview partition.
//
// The import window's preview of a sound file (Specs/import-window.md W3, W6): its waveform, with
// Play and Stop over it and a playhead while it plays, and the facts the decoder gives: its
// length, its channels and rate, and its peak. The file's settings show on the waveform as the
// cook will apply them (the trimmed tail shaded, the bars normalized), and with "As imported" on,
// Play plays it so (louder or softer by Normalize, stopping where the trim cuts). The file is read,
// probed and reduced to its peaks on the ImportPreviewReader's worker; it plays through the
// runtime context's audio, as the clip page auditions (nothing plays without an engine, as in a
// test). The audio domain registers it for audio clips.

module;
#include "Core/Prelude.h"

export module editor.audio:sound_file_preview;

import foundation.core;
import foundation.audio;
import foundation.gui;
import foundation.gui.toolkit; // ToolButton
import engine.audio;
import editor.core;
import editor.app;
import pipeline.importer;
import :waveform_view;
import :audition;

using namespace foundation::core;

export namespace editor
{
    class SoundFilePreview final : public IImportPreview
    {
    public:
        /// Shows that it is reading `path` and starts the read on `reader`; `audio` is what Play
        /// plays through (null: Play does nothing).
        SoundFilePreview(StringView path, const pipeline::ImportOptions* options, ImportPreviewReader& reader, IAllocator& allocator,
                         engine::audio::AudioSubsystem* audio);
        SoundFilePreview(const SoundFilePreview&) = delete;
        SoundFilePreview& operator=(const SoundFilePreview&) = delete;
        ~SoundFilePreview() override;

        [[nodiscard]] foundation::gui::Node& View() override { return *m_root; }
        [[nodiscard]] Span<const String> Facts() const override { return m_facts.AsSpan(); }
        void OnSettingsChanged() override { ShowSettings(); }

        /// Plays the sound from its start, the playhead following it; false when nothing plays.
        bool Play();
        void Stop();
        /// Whether Play plays the sound as the settings will make it.
        void SetAsImported(bool asImported);
        [[nodiscard]] bool AsImported() const noexcept { return m_asImported; }

        // === For tests ===
        [[nodiscard]] WaveformView& Waveform() noexcept { return *m_waveform; }
        [[nodiscard]] bool HasSound() const noexcept { return m_clip.Get() != nullptr; }
        [[nodiscard]] StringView State() const;

    private:
        // What the worker reads, for the main thread to show.
        struct Decoded : RefCounted
        {
            Array<byte> bytes;
            foundation::audio::AudioClipMetadata metadata;
            Array<f32> peaks;
            usize audibleFrames = 0; // what the trim keeps
            i32 peak = 0;            // the loudest sample, of 32767
            bool ok = false;
        };
        void Show(Decoded& decoded);
        // The settings on the waveform: the trimmed tail shaded, the bars normalized.
        void ShowSettings();
        // The gain Normalize gives it, and where the trim cuts (a fraction of its length); 1 and 1
        // when the setting is off or the file has none.
        [[nodiscard]] f32 ImportedGain() const;
        [[nodiscard]] f32 ImportedEnd() const;
        // Called on the GUI's clock while it plays: the playhead and the state follow the voice.
        bool Follow();

        IAllocator* m_allocator;
        RefPtr<foundation::gui::Node> m_root;
        RefPtr<WaveformView> m_waveform;
        foundation::gui::Label* m_state = nullptr;
        foundation::gui::Label* m_message = nullptr;
        foundation::gui::ToolButton* m_asImportedButton = nullptr;
        const pipeline::ImportOptions* m_options = nullptr; // the file's, the window's to keep alive
        bool m_asImported = false;
        f32 m_audibleFraction = 1.0f;
        f32 m_normalizeGain = 1.0f;
        Array<String> m_facts;
        RefPtr<foundation::audio::AudioClip> m_clip;
        UniquePtr<AuditionVoice> m_voice;
        RefPtr<foundation::gui::TickAction> m_follow;
        RefPtr<ImportPreviewRead> m_read;
    };

    /// Previews the sound files an importer lists by one asset type; it plays through the audio of
    /// the host its page services name when it makes a preview.
    class SoundFilePreviewFactory final : public IImportPreviewFactory
    {
    public:
        SoundFilePreviewFactory(const TypeInfo& type, const app::AssetPageServicesSource& services) : m_type(&type), m_services(&services) {}
        [[nodiscard]] const TypeInfo* AssetType() const override { return m_type; }
        [[nodiscard]] UniquePtr<IImportPreview> Create(StringView path, const pipeline::ImportOptions* options, ImportPreviewReader& reader,
                                                       IAllocator& allocator) override;

    private:
        const TypeInfo* m_type;
        const app::AssetPageServicesSource* m_services;
    };

    /// A sound's peak (0 to 1) as the facts say it: "Peak -3.1 dB"; "Silent" for none.
    [[nodiscard]] String PeakText(f32 peak);
}
