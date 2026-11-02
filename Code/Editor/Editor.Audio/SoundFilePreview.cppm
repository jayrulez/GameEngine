// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :sound_file_preview partition.
//
// The import window's preview of a sound file (Specs/import-window.md W3): its waveform, with
// Play and Stop over it and a playhead while it plays, and the facts the decoder gives: its
// length, its channels and rate, and its peak. The file is read, probed and reduced to its peaks
// on the ImportPreviewReader's worker; it plays through the runtime context's audio, as the clip
// page auditions (nothing plays without an engine, as in a test). The audio domain registers it
// for audio clips.

module;
#include "Core/Prelude.h"

export module editor.audio:sound_file_preview;

import foundation.core;
import foundation.audio;
import foundation.gui;
import engine.audio;
import editor.core;
import editor.app;
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
        SoundFilePreview(StringView path, ImportPreviewReader& reader, IAllocator& allocator, engine::audio::AudioSubsystem* audio);
        SoundFilePreview(const SoundFilePreview&) = delete;
        SoundFilePreview& operator=(const SoundFilePreview&) = delete;
        ~SoundFilePreview() override;

        [[nodiscard]] foundation::gui::Node& View() override { return *m_root; }
        [[nodiscard]] Span<const String> Facts() const override { return m_facts.AsSpan(); }

        /// Plays the sound from its start, the playhead following it; false when nothing plays.
        bool Play();
        void Stop();

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
            bool ok = false;
        };
        void Show(Decoded& decoded);
        // Called on the GUI's clock while it plays: the playhead and the state follow the voice.
        bool Follow();

        IAllocator* m_allocator;
        RefPtr<foundation::gui::Node> m_root;
        RefPtr<WaveformView> m_waveform;
        foundation::gui::Label* m_state = nullptr;
        foundation::gui::Label* m_message = nullptr;
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
        [[nodiscard]] UniquePtr<IImportPreview> Create(StringView path, ImportPreviewReader& reader, IAllocator& allocator) override;

    private:
        const TypeInfo* m_type;
        const app::AssetPageServicesSource* m_services;
    };

    /// A sound's peak (0 to 1) as the facts say it: "Peak -3.1 dB"; "Silent" for none.
    [[nodiscard]] String PeakText(f32 peak);
}
