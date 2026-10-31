// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the audio clip page's implementation (see AudioClipPage.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.audio;

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
namespace audio = foundation::audio;
namespace runtime = foundation::runtime;
namespace gui = foundation::gui;

namespace editor
{
    RTTI_DEFINE_OBJECT(WaveformView, "rtti::editor::audio")

    AudioClipEditorPage::AudioClipEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::AudioClipAsset::StaticType(), kPath)
    {
        Provide<IPlaybackPage>(*this);
        m_voice = MakeUnique<AuditionVoice>(Allocator(), Services().host != nullptr ? Services().host->Subsystems().GetSubsystem<engine::audio::AudioSubsystem>() : nullptr);
        m_stage = Find<gui::Node>(u8"audio-stage");
        m_empty = Find<gui::Node>(u8"audio-empty");
        m_emptyText = Find<gui::Label>(u8"audio-empty-text");
        m_state = Find<gui::Label>(u8"audio-state");
        m_volume = Find<gui::Slider>(u8"audio-volume");
        m_waveform = MakeRef<WaveformView>(Allocator());
        if (m_stage != nullptr)
            m_stage->AddChild(m_waveform.Get());
        // The kit's transport runs the playback actions for this page, as on every page.
        BindAction(Find<gui::ButtonBase>(u8"audio-restart"), u8"playback.restart");
        BindAction(Find<gui::ButtonBase>(u8"audio-play"), u8"playback.play");
        BindAction(Find<gui::ButtonBase>(u8"audio-stop"), u8"playback.stop");
        AudioClipEditorPage* self = this;
        if (m_volume != nullptr)
            m_volume->SetOnValueChanging([self](f64 volume) { self->SetAuditionVolume(static_cast<f32>(volume)); });
        LoadClip();
        ShowState(u8"stopped", u8"Stopped");
    }

    AudioClipEditorPage::~AudioClipEditorPage()
    {
        StopAudition();
    }

    StringView AudioClipEditorPage::State() const
    {
        return m_state != nullptr ? m_state->GetText() : StringView{};
    }

    void AudioClipEditorPage::ShowState(StringView state, StringView text)
    {
        ShowTransportState(m_state, state, text);
    }

    void AudioClipEditorPage::ShowEmpty(StringView why)
    {
        m_clip = nullptr;
        if (m_waveform)
            m_waveform->SetPeaks(Array<f32>{});
        if (m_stage != nullptr)
            m_stage->SetDisplayed(false);
        if (m_empty != nullptr)
            m_empty->SetDisplayed(true);
        if (m_emptyText != nullptr)
            m_emptyText->SetText(why);
    }

    void AudioClipEditorPage::LoadClip()
    {
        StopAudition();
        const auto* asset = ObjectAs<pipeline::AudioClipAsset>();
        if (asset == nullptr)
            return;
        m_loadedFile = String(asset->fileName.View());
        if (asset->fileName.IsEmpty() || Context().Project() == nullptr)
        {
            ShowEmpty(u8"This clip names no source file.");
            return;
        }
        audio::AudioClipMetadata metadata;
        m_clip = ReadAuditionClip(Allocator(), *Context().Project(), *asset, &metadata);
        if (m_clip.Get() == nullptr)
        {
            LOG_WARNING(u8"Editor", u8"audio source missing or undecodable: {}", asset->fileName.View());
            ShowEmpty(Format(u8"{} is missing or did not decode.", asset->fileName.View()).AsView());
            return;
        }
        Array<f32> peaks;
        if (audio::BuildWaveformPeaks(Span<const byte>(m_clip->encodedData.Data(), m_clip->encodedData.Size()), kPeakBuckets, peaks))
            m_waveform->SetPeaks(Move(peaks));
        if (m_stage != nullptr)
            m_stage->SetDisplayed(true);
        if (m_empty != nullptr)
            m_empty->SetDisplayed(false);
        SetFact(u8"Channels", metadata.channels == 1 ? StringView(u8"Mono") : metadata.channels == 2 ? StringView(u8"Stereo")
                                                                                                       : Format(u8"{}", metadata.channels).AsView());
        SetFact(u8"Sample Rate", Format(u8"{} Hz", metadata.sampleRate).AsView());
        SetFact(u8"Length", Format(u8"{} s ({} frames)", FormatFixed(metadata.durationSeconds, 2), metadata.frameCount).AsView());
    }

    void AudioClipEditorPage::ApplyOptions()
    {
        const auto* asset = ObjectAs<pipeline::AudioClipAsset>();
        if (asset == nullptr || m_clip.Get() == nullptr)
            return;
        m_clip->gain = asset->gain;
        m_clip->loop = asset->loop;
        m_clip->loopStartFrame = asset->loopStartFrame;
        m_clip->loopEndFrame = asset->loopEndFrame;
    }

    void AudioClipEditorPage::OnObjectChanged()
    {
        const auto* asset = ObjectAs<pipeline::AudioClipAsset>();
        if (asset != nullptr && asset->fileName.View() != m_loadedFile.AsView())
            LoadClip(); // a different source: read it again
        else
            ApplyOptions(); // the next play uses the new options
    }

    void AudioClipEditorPage::Audition()
    {
        StopAudition();
        if (!CanPlay())
            return;
        audio::AudioPlayParams params;
        params.loop = m_clip->loop;
        if (!m_voice->Start(m_clip, params))
        {
            ShowState(u8"faulted", u8"No voice: the audio engine did not start one");
            return;
        }
        ShowState(u8"playing", u8"Playing");
    }

    void AudioClipEditorPage::Play()
    {
        if (!m_voice->IsActive())
        {
            Audition();
            return;
        }
        m_voice->SetPaused(false);
        ShowState(u8"playing", u8"Playing");
    }

    void AudioClipEditorPage::Pause()
    {
        if (!m_voice->IsPlaying())
            return;
        m_voice->SetPaused(true);
        ShowState(u8"paused", u8"Paused");
    }

    void AudioClipEditorPage::SetAuditionVolume(f32 volume)
    {
        m_voice->SetVolume(volume); // heard at once, as the slider moves
        if (m_volume != nullptr && m_volume->GetValue() != static_cast<f64>(m_voice->Volume()))
            m_volume->SetValue(m_voice->Volume());
    }

    void AudioClipEditorPage::StopAudition()
    {
        if (m_voice)
            m_voice->Stop();
        if (m_waveform)
            m_waveform->SetPlayhead(-1.0f);
        ShowState(u8"stopped", u8"Stopped");
    }

    void AudioClipEditorPage::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt); // updates the bar, and the transport with it, each frame
        if (!m_voice->IsActive())
            return;
        f32 cursor = 0.0f;
        if (!m_voice->Poll(cursor))
        {
            StopAudition(); // finished, or the engine took its voice
            return;
        }
        const f32 duration = m_clip.Get() != nullptr ? m_clip->durationSeconds : 0.0f;
        if (m_voice->IsPaused() || duration <= 0.0f)
            return; // a paused audition holds its playhead
        f32 fraction = cursor / duration;
        if (m_clip->loop)
            fraction -= static_cast<f32>(static_cast<i64>(fraction));
        m_waveform->SetPlayhead(Min(fraction, 1.0f));
        ShowState(u8"playing", Format(u8"Playing {} s", FormatFixed(cursor, 1)).AsView());
    }

    const TypeInfo* AudioClipPageFactory::PrimaryType() const
    {
        return &pipeline::AudioClipAsset::StaticType();
    }

    UniquePtr<EditorPage> AudioClipPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<AudioClipEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }
}
