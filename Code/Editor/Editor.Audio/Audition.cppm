// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :audition partition.
//
// What the audio pages audition with (Documentation/Specs/editor-gui.md, 5.4, layer 5): a clip read
// from its asset's source file, as the cook reads it, and one voice played through the RUNTIME
// context's AudioSubsystem (the game's engine, buses and voice pool, so what is heard is what the
// game plays). A clip page plays its clip; a cue page plays the variant the runtime's resolver
// picks. Either way one voice at a time: playing again stops the last.

module;
#include "Core/Prelude.h"

export module editor.audio:audition;

import foundation.core;
import foundation.audio;
import audio.pipeline;
import engine.audio;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    namespace audio = foundation::audio;

    /// The clip `asset` names, read from its source file under `project` (its options applied:
    /// gain, loop); null when the file is missing or does not decode. `metadata` (when given) gets
    /// what the probe read.
    [[nodiscard]] inline RefPtr<audio::AudioClip> ReadAuditionClip(IAllocator& allocator, const EditorProject& project, const pipeline::AudioClipAsset& asset,
                                                                  audio::AudioClipMetadata* metadata = nullptr)
    {
        if (asset.fileName.IsEmpty())
            return {};
        Result<Array<byte>> bytes = ReadFile(PathJoin(project.SourcesRoot().AsView(), asset.fileName.View()).AsView());
        audio::AudioClipMetadata probed;
        if (!bytes.HasValue() || !audio::ProbeAudioClipMetadata(Span<const byte>(bytes.Value().Data(), bytes.Value().Size()), probed))
            return {};
        RefPtr<audio::AudioClip> clip = MakeRef<audio::AudioClip>(allocator);
        clip->channels = probed.channels;
        clip->sampleRate = probed.sampleRate;
        clip->frameCount = probed.frameCount;
        clip->durationSeconds = probed.durationSeconds;
        clip->gain = asset.gain;
        clip->loop = asset.loop;
        clip->loopStartFrame = asset.loopStartFrame;
        clip->loopEndFrame = asset.loopEndFrame;
        clip->encodedData = Move(bytes.Value());
        if (metadata != nullptr)
            *metadata = probed;
        return clip;
    }

    /// A transport's state beside it (`label`, the kit's transport-state): its class (stopped,
    /// playing, paused, faulted) the only one of them it has, and `text`.
    inline void ShowTransportState(foundation::gui::Label* label, StringView state, StringView text)
    {
        if (label == nullptr)
            return;
        constexpr StringView kStates[] = {u8"stopped", u8"playing", u8"paused", u8"faulted"};
        for (const StringView name : kStates)
            label->RemoveClass(name);
        label->AddClass(state);
        label->SetText(text);
    }

    /// One audition voice through the runtime's audio engine (none without an engine: a test, a
    /// headless host).
    class AuditionVoice
    {
    public:
        explicit AuditionVoice(engine::audio::AudioSubsystem* audio) : m_audio(audio) {}
        AuditionVoice(const AuditionVoice&) = delete;
        AuditionVoice& operator=(const AuditionVoice&) = delete;
        ~AuditionVoice() { Stop(); }

        /// Whether there is an engine to play through.
        [[nodiscard]] bool HasEngine() const { return m_audio != nullptr && m_audio->Engine() != nullptr; }
        [[nodiscard]] bool IsActive() const noexcept { return m_voice.IsValid(); }
        [[nodiscard]] bool IsPaused() const noexcept { return m_voice.IsValid() && m_paused; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_voice.IsValid() && !m_paused; }

        /// Plays `clip` from its start (the last voice stopped first); false when no voice started.
        bool Start(const RefPtr<audio::AudioClip>& clip, audio::AudioPlayParams params)
        {
            Stop();
            if (!HasEngine() || clip.Get() == nullptr)
                return false;
            params.allowDedupe = false; // a quick play again starts again, never merges
            m_voice = m_audio->Engine()->Play(clip, params);
            if (m_voice.IsValid())
                m_audio->Engine()->SetVoiceVolume(m_voice, m_volume);
            m_paused = false;
            return m_voice.IsValid();
        }
        void SetPaused(bool paused)
        {
            if (!m_voice.IsValid() || paused == m_paused || !HasEngine())
                return;
            m_paused = paused;
            m_audio->Engine()->SetPaused(m_voice, paused);
        }
        void Stop()
        {
            if (m_voice.IsValid() && HasEngine())
                m_audio->Engine()->Stop(m_voice);
            m_voice = audio::VoiceHandle{};
            m_paused = false;
        }
        /// The voice's volume, 0 to 1, this voice only; heard at once while it plays.
        void SetVolume(f32 volume)
        {
            m_volume = Clamp(volume, 0.0f, 1.0f);
            if (m_voice.IsValid() && HasEngine())
                m_audio->Engine()->SetVoiceVolume(m_voice, m_volume);
        }
        [[nodiscard]] f32 Volume() const noexcept { return m_volume; }
        /// Once a frame: where the voice is in its clip (seconds, the voice's own cursor: pitch and
        /// loop wraps honoured); false when it finished or was taken (it is let go).
        bool Poll(f32& cursorSeconds)
        {
            if (!m_voice.IsValid() || !HasEngine())
                return false;
            audio::VoiceStatus status;
            if (!m_audio->Engine()->GetVoiceStatus(m_voice, status) || !status.active)
            {
                m_voice = audio::VoiceHandle{};
                m_paused = false;
                return false;
            }
            cursorSeconds = status.cursorSeconds;
            return true;
        }

    private:
        engine::audio::AudioSubsystem* m_audio = nullptr;
        audio::VoiceHandle m_voice;
        bool m_paused = false;
        f32 m_volume = 1.0f;
    };
}
