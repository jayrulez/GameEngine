// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - :sound_file_preview partition, its body. See SoundFilePreview.cppm.

module;
#include "Core/Prelude.h"

module editor.audio;

import foundation.core;
import foundation.audio;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.runtime.client; // the host, for its audio
import engine.audio;
import pipeline.importer; // FileNameOf, ImportOptions
import audio.pipeline;    // AudioImportOptions, AudioEdits (the cook's own trim and peak)
import editor.core;
import editor.app;
import :waveform_view;
import :audition;
import :sound_file_preview;

using namespace foundation::core;

namespace editor
{
    namespace audio = foundation::audio;
    namespace gui = foundation::gui;

    namespace
    {
        constexpr u32 kPreviewPeakBuckets = 400; // the pane is a few hundred pixels wide

        [[nodiscard]] RefPtr<gui::ToolButton> Tool(IAllocator& allocator, StringView icon, StringView tooltip)
        {
            RefPtr<gui::ToolButton> button = MakeRef<gui::ToolButton>(allocator);
            button->SetIconSource(String(icon));
            button->SetTooltip(tooltip);
            return button;
        }

        // "Mono, 44100 Hz".
        [[nodiscard]] String ChannelsText(const audio::AudioClipMetadata& metadata)
        {
            const String channels = metadata.channels == 1 ? String(u8"Mono") : metadata.channels == 2 ? String(u8"Stereo")
                                                                                                         : Format(u8"{} channels", metadata.channels);
            return Format(u8"{}, {} Hz", channels, metadata.sampleRate);
        }
    }

    String PeakText(f32 peak)
    {
        if (peak <= 0.0f)
            return String(u8"Silent");
        constexpr f32 kDecibelsPerLn = 8.68588964f; // 20 / ln(10): 20 log10(x) through the natural log
        return Format(u8"Peak {} dB", FormatFixed(kDecibelsPerLn * Log(peak), 1));
    }

    SoundFilePreview::SoundFilePreview(StringView path, const pipeline::ImportOptions* options, TaskPool& tasks, IAllocator& allocator,
                                       engine::audio::AudioSubsystem* audio)
        : m_allocator(&allocator), m_options(options), m_facts(allocator)
    {
        m_voice = MakeUnique<AuditionVoice>(allocator, audio);
        RefPtr<gui::Panel> root = MakeRef<gui::Panel>(allocator);
        root->AddClass(u8"import-sound");
        RefPtr<gui::Panel> bar = MakeRef<gui::Panel>(allocator);
        bar->AddClass(u8"panel-toolbar");
        bar->AddClass(u8"audio-bar");
        SoundFilePreview* self = this;
        RefPtr<gui::ToolButton> play = Tool(allocator, u8"Editor/Icons/play.svg", u8"Play it from the start");
        play->SetOnClick([self]() { (void)self->Play(); });
        RefPtr<gui::ToolButton> stop = Tool(allocator, u8"Editor/Icons/stop.svg", u8"Stop");
        stop->SetOnClick([self]() { self->Stop(); });
        RefPtr<gui::ToolButton> asImported = Tool(allocator, u8"Editor/Icons/speaker.svg", u8"As imported: play it as its settings will make it (normalized, trimmed)");
        asImported->SetToggle(true);
        asImported->SetText(u8"As imported");
        asImported->SetShowsText(true);
        asImported->SetOnToggled([self](bool on) { self->SetAsImported(on); });
        m_asImportedButton = asImported.Get();
        RefPtr<gui::Label> state = MakeRef<gui::Label>(allocator);
        state->AddClass(u8"transport-state");
        m_state = state.Get();
        bar->AddChild(play.Get());
        bar->AddChild(stop.Get());
        bar->AddChild(asImported.Get());
        bar->AddChild(state.Get());
        RefPtr<gui::Panel> stage = MakeRef<gui::Panel>(allocator);
        stage->AddClass(u8"audio-stage");
        m_waveform = MakeRef<WaveformView>(allocator);
        stage->AddChild(m_waveform.Get());
        RefPtr<gui::Label> message = MakeRef<gui::Label>(allocator);
        message->AddClass(u8"state-text");
        message->AddClass(u8"import-sound-message");
        m_message = message.Get();
        root->AddChild(bar.Get());
        root->AddChild(stage.Get());
        root->AddChild(message.Get());
        m_root = root;
        ShowTransportState(m_state, u8"stopped", u8"Stopped");
        m_message->SetText(Format(u8"Reading {}...", pipeline::FileNameOf(path)).AsView());

        RefPtr<Decoded> decoded = MakeRef<Decoded>(allocator);
        // On the worker: only the file and `decoded`, which nothing else touches until it is back.
        m_read = tasks.Submit(Format(u8"Reading {}", pipeline::FileNameOf(path)).AsView(),
                              [decoded, file = String(path)](TaskContext&)
                             {
                                 Result<Array<byte>> bytes = ReadFile(file.AsView());
                                 if (!bytes.HasValue())
                                     return Status{}; // Show says it did not read
                                 const Span<const byte> encoded(bytes.Value().Data(), bytes.Value().Size());
                                 if (!audio::ProbeAudioClipMetadata(encoded, decoded->metadata))
                                     return Status{};
                                 (void)audio::BuildWaveformPeaks(encoded, kPreviewPeakBuckets, decoded->peaks);
                                 // Where the trim cuts and how loud its peak is, as the cook counts them.
                                 Array<i16> samples;
                                 audio::AudioClipMetadata pcm;
                                 if (audio::DecodeAudioClipToPcm16(encoded, 0u, samples, pcm))
                                 {
                                     decoded->audibleFrames = pipeline::AudioEdits::AudibleFrames(Span<const i16>(samples.Data(), samples.Size()), pcm.channels, pcm.sampleRate);
                                     decoded->peak = pipeline::AudioEdits::Peak(Span<const i16>(samples.Data(), samples.Size()));
                                 }
                                 decoded->bytes = Move(bytes.Value());
                                 decoded->ok = true;
                                 return Status{};
                             },
                             // On the main thread, and only while this preview lives (it cancels the read as it goes).
                             [self, decoded](Status) { self->Show(*decoded); });
    }

    SoundFilePreview::~SoundFilePreview()
    {
        if (m_read)
            m_read->Cancel();
        if (m_follow)
            m_follow->Stop();
        m_voice->Stop();
    }

    void SoundFilePreview::Show(Decoded& decoded)
    {
        if (!decoded.ok)
        {
            m_message->SetText(u8"This file did not decode as a sound.");
        }
        else
        {
            const audio::AudioClipMetadata& metadata = decoded.metadata;
            f32 peak = 0.0f;
            for (const f32 bucket : decoded.peaks)
                peak = Max(peak, bucket);
            m_facts.PushBack(Format(u8"{} s", FormatFixed(metadata.durationSeconds, 2)));
            m_facts.PushBack(ChannelsText(metadata));
            if (!decoded.peaks.IsEmpty())
                m_facts.PushBack(PeakText(peak));
            m_waveform->SetPeaks(Move(decoded.peaks));
            m_clip = MakeRef<audio::AudioClip>(*m_allocator);
            m_clip->channels = metadata.channels;
            m_clip->sampleRate = metadata.sampleRate;
            m_clip->frameCount = metadata.frameCount;
            m_clip->durationSeconds = metadata.durationSeconds;
            m_clip->encodedData = Move(decoded.bytes);
            m_audibleFraction = metadata.frameCount > 0 && decoded.audibleFrames > 0
                                    ? Min(static_cast<f32>(static_cast<f64>(decoded.audibleFrames) / static_cast<f64>(metadata.frameCount)), 1.0f)
                                    : 1.0f;
            m_normalizeGain = decoded.peak > 0 ? pipeline::AudioEdits::kNormalizeTarget / static_cast<f32>(decoded.peak) : 1.0f;
            m_message->SetText(StringView{});
            m_message->SetDisplayed(false);
            ShowSettings();
        }
        if (onChanged)
            onChanged();
    }

    bool SoundFilePreview::Play()
    {
        Stop();
        if (m_clip.Get() == nullptr)
            return false;
        m_clip->gain = m_asImported ? ImportedGain() : 1.0f; // Normalize, heard
        if (!m_voice->Start(m_clip, audio::AudioPlayParams{}))
        {
            ShowTransportState(m_state, u8"faulted", m_voice->HasEngine() ? StringView(u8"No voice: the audio engine did not start one") : StringView(u8"No audio engine"));
            return false;
        }
        ShowTransportState(m_state, u8"playing", u8"Playing");
        SoundFilePreview* self = this;
        m_follow = MakeRef<gui::TickAction>(*m_allocator, [self](Duration) { return self->Follow(); });
        m_root->RunAction(m_follow);
        return true;
    }

    void SoundFilePreview::Stop()
    {
        if (m_follow)
        {
            m_follow->Stop();
            m_follow = {};
        }
        m_voice->Stop();
        m_waveform->SetPlayhead(-1.0f);
        ShowTransportState(m_state, u8"stopped", u8"Stopped");
    }

    bool SoundFilePreview::Follow()
    {
        f32 cursor = 0.0f;
        bool playing = m_voice->Poll(cursor) && m_clip.Get() != nullptr && m_clip->durationSeconds > 0.0f;
        if (playing && m_asImported && cursor >= ImportedEnd() * m_clip->durationSeconds)
        {
            m_voice->Stop(); // the trim cuts it here
            playing = false;
        }
        if (!playing)
        {
            m_waveform->SetPlayhead(-1.0f);
            ShowTransportState(m_state, u8"stopped", u8"Stopped");
            return false;
        }
        m_waveform->SetPlayhead(Min(cursor / m_clip->durationSeconds, 1.0f));
        ShowTransportState(m_state, u8"playing", Format(u8"Playing {} s", FormatFixed(cursor, 1)).AsView());
        return true;
    }

    void SoundFilePreview::SetAsImported(bool asImported)
    {
        m_asImported = asImported;
        if (m_asImportedButton != nullptr && m_asImportedButton->IsChecked() != asImported)
            m_asImportedButton->SetChecked(asImported);
    }

    f32 SoundFilePreview::ImportedGain() const
    {
        const auto* settings = Cast<pipeline::AudioImportOptions>(m_options);
        return settings != nullptr && settings->normalize ? m_normalizeGain : 1.0f;
    }

    f32 SoundFilePreview::ImportedEnd() const
    {
        const auto* settings = Cast<pipeline::AudioImportOptions>(m_options);
        return settings != nullptr && settings->trimTrailingSilence ? m_audibleFraction : 1.0f;
    }

    void SoundFilePreview::ShowSettings()
    {
        m_waveform->SetGain(ImportedGain());
        m_waveform->SetCutFrom(ImportedEnd());
    }

    StringView SoundFilePreview::State() const
    {
        return m_state != nullptr ? m_state->GetText() : StringView{};
    }

    UniquePtr<IImportPreview> SoundFilePreviewFactory::Create(const ImportPreviewSource& file, TaskPool& tasks, IAllocator& allocator)
    {
        const app::AssetPageServices services = m_services->Make();
        engine::audio::AudioSubsystem* audio = services.host != nullptr ? services.host->Subsystems().GetSubsystem<engine::audio::AudioSubsystem>() : nullptr;
        return UniquePtr<IImportPreview>(allocator.New<SoundFilePreview>(file.path, file.options, tasks, allocator, audio), allocator);
    }
}
