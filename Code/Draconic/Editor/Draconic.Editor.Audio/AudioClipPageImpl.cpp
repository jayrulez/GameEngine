// Draconic::EditorAudio - the `draconic.editor.audio` module.
//
// AudioClipPage (audio.md P2): the audition page. Opens an AudioClipAsset with a peak
// waveform (decoded from the copied source file - the same container bytes the cook
// writes through) and Play/Stop that audition through the RUNTIME CONTEXT's
// AudioSubsystem - the GAME's engine, buses, and voice pool, so what you hear IS what
// the game plays (the "cook says fine, ears say nothing" gap this page closes).
// Auditioning honors the asset's loop intent; a playhead tracks the voice.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.audio;

import :sound_cue_page;

import draconic.foundation;
import draconic.content;
import draconic.runtime.client;
import draconic.audio;
import draconic.audio.editor;
import draconic.engine.audio;
import draconic.ui;
import draconic.editor.core;
import draconic.editor.app;

using namespace draconic::foundation;

namespace draconic::editor
{
    Status AudioClipEditorPage::Save()
    {
        return Status{};
    } // audition-only (options edit via inspector)

    void AudioClipEditorPage::OnUpdate(runtime::IApplicationHost&, f32)
    {
        // Playhead: the voice's TRUE cursor (VoiceStatus::cursorSeconds) - honors
        // pitch and loop wraps, unlike the old elapsed-time approximation. The
        // voice handle going invalid (finished/stolen) parks the head.
        if (!m_voice.IsValid() || m_audio == nullptr || m_audio->Engine() == nullptr)
        {
            return;
        }
        if (!m_audio->Engine()->IsPlaying(m_voice))
        {
            StopAudition();
            return;
        }
        audio::VoiceStatus status;
        if (!m_audio->Engine()->GetVoiceStatus(m_voice, status))
        {
            return;
        }
        const f32 duration = m_clip.Get() != nullptr ? m_clip->durationSeconds : 0.0f;
        if (duration <= 0.0f)
        {
            return;
        }
        f32 fraction = status.cursorSeconds / duration;
        if (m_loop)
        {
            fraction = fraction - static_cast<f32>(static_cast<i64>(fraction));
        }
        m_waveform->SetPlayheadFraction(Min(fraction, 1.0f));
        String text = Format(u8"Playing...  {} s", FormatFixed(status.cursorSeconds, 1));
        m_status->SetText(text.AsView());
    }

    void AudioClipEditorPage::LoadClip(draconic::content::Instance& instance)
    {
        RefPtr<ISerializable> object = instance.ReadObject();
        auto* asset = Cast<audio::AudioClipAsset>(object.Get());
        if (asset == nullptr || m_context->Project() == nullptr)
        {
            return;
        }
        m_loop = asset->loop;
        const String path =
            PathJoin(m_context->Project()->SourcesRoot().AsView(), asset->fileName.View());
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        if (!bytes.HasValue())
        {
            DRACONIC_LOG_WARNING(u8"Editor", u8"audio source missing: {}", path);
            return;
        }
        audio::AudioClipMetadata metadata;
        if (!audio::ProbeAudioClipMetadata(
                Span<const byte>(bytes.Value().Data(), bytes.Value().Size()), metadata))
        {
            return;
        }
        m_clip = MakeRef<audio::AudioClip>(DefaultAllocator());
        m_clip->channels = metadata.channels;
        m_clip->sampleRate = metadata.sampleRate;
        m_clip->frameCount = metadata.frameCount;
        m_clip->durationSeconds = metadata.durationSeconds;
        m_clip->gain = asset->gain;
        m_clip->loop = asset->loop;
        m_clip->loopStartFrame = asset->loopStartFrame;
        m_clip->loopEndFrame = asset->loopEndFrame;
        m_clip->encodedData = Move(bytes.Value());
    }

    void AudioClipEditorPage::RefreshInfo()
    {
        if (m_clip.Get() == nullptr)
        {
            m_info->SetText(u8"Source file missing or undecodable.");
            m_playButton->IsEnabled = false;
            m_stopButton->IsEnabled = false;
            return;
        }
        String text = Format(u8"{} ch  |  {} Hz  |  {} s{}", m_clip->channels, m_clip->sampleRate,
                             FormatFixed(m_clip->durationSeconds, 2),
                             m_loop ? StringView(u8"  |  loops") : StringView(u8""));
        m_info->SetText(text.AsView());
        Array<f32> peaks;
        if (audio::BuildWaveformPeaks(
                Span<const byte>(m_clip->encodedData.Data(), m_clip->encodedData.Size()), 256,
                peaks))
        {
            m_waveform->SetPeaks(Move(peaks));
        }
    }

    void AudioClipEditorPage::Audition()
    {
        if (m_clip.Get() == nullptr || m_audio == nullptr || m_audio->Engine() == nullptr)
        {
            return;
        }
        StopAudition();
        audio::AudioPlayParams params;
        params.loop = m_loop;
        params.allowDedupe = false; // rapid re-audition must restart, never merge
        m_voice = m_audio->Engine()->Play(m_clip, params);
        m_status->SetText(m_voice.IsValid() ? StringView(u8"Playing...")
                                            : StringView(u8"No voice (engine headless?)"));
    }

    void AudioClipEditorPage::StopAudition()
    {
        if (m_audio != nullptr && m_audio->Engine() != nullptr && m_voice.IsValid())
        {
            m_audio->Engine()->Stop(m_voice);
        }
        m_voice = audio::VoiceHandle{};
        m_waveform->SetPlayheadFraction(-1.0f);
        m_status->SetText(u8"");
    }
    const TypeInfo* AudioClipPageFactory::PrimaryType() const
    {
        return &audio::AudioClipAsset::StaticType();
    }

    UniquePtr<EditorPage> AudioClipPageFactory::CreatePage(EditorContext& context,
                                                           draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<AudioClipEditorPage>(context, *m_host, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
    void WaveformView::SetPeaks(Array<f32> peaks)
    {
        m_peaks = Move(peaks);
        Invalidate();
    }

    void WaveformView::SetPlayheadFraction(f32 fraction)
    {
        if (m_playhead != fraction)
        {
            m_playhead = fraction;
            Invalidate();
        }
    }

    void WaveformView::OnDraw(ui::UIDrawContext& ctx)
    {
        const Rectangle bounds{0.0f, 0.0f, Width(), Height()};
        ctx.VG().FillRect(bounds, Color{0.10f, 0.11f, 0.13f, 1.0f});
        if (m_peaks.IsEmpty() || bounds.width <= 2.0f || bounds.height <= 2.0f)
        {
            return;
        }
        const f32 mid = bounds.height * 0.5f;
        const f32 barWidth = bounds.width / static_cast<f32>(m_peaks.Size());
        const Color barColor{64.0f / 255.0f, 200.0f / 255.0f, 190.0f / 255.0f, 0.9f};
        for (usize i = 0; i < m_peaks.Size(); ++i)
        {
            const f32 half = Max(1.0f, m_peaks[i] * (mid - 2.0f));
            ctx.VG().FillRect(Rectangle{static_cast<f32>(i) * barWidth, mid - half,
                                        Max(1.0f, barWidth - 1.0f), half * 2.0f},
                              barColor);
        }
        if (m_playhead >= 0.0f && m_playhead <= 1.0f)
        {
            ctx.VG().FillRect(
                Rectangle{m_playhead * bounds.width - 1.0f, 0.0f, 2.0f, bounds.height},
                Color{0.95f, 0.85f, 0.4f, 1.0f});
        }
    }
}
