// Draconic::EditorAudio - the `:sound_cue_page` partition.
//
// SoundCuePage (audio.md P3): the cue editor - eight variant slot rows (clip picker +
// weight), cue-level mode/jitter fields, and AUDITION that resolves through the REAL
// ResolveSoundCue (same weights, no-repeat state, and jitter the game uses) and plays
// through the runtime engine. Save writes the asset and nudges the validating recook.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.audio;

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
    void SoundCueEditorPage::OnUpdate(runtime::IApplicationHost&, f32)
    {
        // Status line: append the auditioning voice's TRUE cursor (item: voice-
        // cursor playhead) while it plays; restore the plain pick line after.
        if (m_audio == nullptr || m_audio->Engine() == nullptr || !m_voice.IsValid())
        {
            return;
        }
        audio::VoiceStatus status;
        if (m_audio->Engine()->GetVoiceStatus(m_voice, status) && status.playing)
        {
            String text = Format(u8"{}  |  {} s", m_pickText, FormatFixed(status.cursorSeconds, 1));
            m_status->SetText(text.AsView());
        }
        else
        {
            m_voice = audio::VoiceHandle{};
            m_status->SetText(m_pickText.AsView());
        }
    }

    Status SoundCueEditorPage::Save()
    {
        draconic::content::Instance* instance =
            (m_context->Project() != nullptr)
                ? m_context->Project()->SourceDb().GetInstance(InstanceId())
                : nullptr;
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status written = instance->WriteObject(m_asset);
        if (written.IsOk())
        {
            ClearDirty();
            if (m_context->OnCookRequested)
            {
                m_context->OnCookRequested(false);
            }
        }
        return written;
    }

    void SoundCueEditorPage::OnClose()
    {
        if (m_audio != nullptr && m_audio->Engine() != nullptr && m_voice.IsValid())
        {
            m_audio->Engine()->Stop(m_voice);
        }
    }

    void SoundCueEditorPage::AddJitterField(ui::FlexLayout& row, StringView label, f32& target)
    {
        auto text = MakeRef<ui::Label>(DefaultAllocator(), label);
        text->FontSize.SetValue(12.0f);
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->AlignSelf = ui::Align::Center;
            row.AddView(text.Get(), lp);
        }
        auto field = MakeRef<ui::NumericField>(DefaultAllocator());
        field->SetMin(0.0);
        field->SetMax(4.0);
        field->SetDecimalPlaces(2);
        field->SetValue(target);
        SoundCueEditorPage* self = this;
        f32* slot = &target; // points into m_asset (stable for the page's lifetime)
        field->OnValueChanged.Add(
            [self, slot](ui::NumericField*, f64 value)
            {
                *slot = static_cast<f32>(value);
                self->MarkDirty();
            });
        row.AddView(field.Get());
        m_jitterFields.PushBack(field);
    }

    void SoundCueEditorPage::PickClip(usize slot)
    {
        ui::UIContext* uiContext = m_content.Get() != nullptr ? m_content->Context : nullptr;
        if (uiContext == nullptr)
        {
            return;
        }
        Array<String> typeNames;
        typeNames.PushBack(String(u8"AudioClipAsset"));
        auto picker =
            MakeRef<app::AssetPickerDialog>(DefaultAllocator(), *m_context, Move(typeNames));
        SoundCueEditorPage* self = this;
        picker->OnPicked = [self, slot](const Guid& id)
        {
            self->m_asset.clipIds[slot] = id;
            self->RefreshSlot(slot);
            self->MarkDirty();
        };
        picker->Show(uiContext);
    }

    void SoundCueEditorPage::RefreshSlot(usize slot)
    {
        const Guid& id = m_asset.clipIds[slot];
        if (id.IsNil() || m_context->Project() == nullptr)
        {
            m_slotLabels[slot]->SetText(u8"(empty)");
            return;
        }
        draconic::content::Instance* clip = m_context->Project()->SourceDb().GetInstance(id);
        m_slotLabels[slot]->SetText(clip != nullptr ? StringView(clip->Path())
                                                    : StringView(u8"(missing)"));
    }

    void SoundCueEditorPage::RefreshModeButton()
    {
        const StringView names[3] = {u8"Mode: Random (no repeat)", u8"Mode: Random",
                                     u8"Mode: Sequential"};
        m_modeButton->SetText(names[m_asset.mode % 3]);
    }

    void SoundCueEditorPage::Audition()
    {
        if (m_audio == nullptr || m_audio->Engine() == nullptr)
        {
            return;
        }
        audio::SoundCue cue;
        cue.mode = static_cast<audio::SoundCueMode>(m_asset.mode);
        cue.pitchMin = m_asset.pitchMin;
        cue.pitchMax = m_asset.pitchMax;
        cue.volumeMin = m_asset.volumeMin;
        cue.volumeMax = m_asset.volumeMax;
        for (usize i = 0; i < audio::kSoundCueSlotCount; ++i)
        {
            audio::SoundCueVariant variant;
            variant.clip = LoadSlotClip(i);
            variant.weight = m_asset.weights[i];
            cue.variants.PushBack(Move(variant));
        }
        const audio::SoundCuePick pick =
            audio::ResolveSoundCue(cue, m_rng, m_lastVariant, m_sequentialCursor);
        if (pick.variantIndex < 0)
        {
            m_status->SetText(u8"No playable variant.");
            return;
        }
        m_lastVariant = pick.variantIndex;
        audio::AudioPlayParams params;
        params.pitch = pick.pitch;
        params.volume = pick.volume;
        params.allowDedupe = false;
        m_voice = m_audio->Engine()->Play(cue.variants[static_cast<usize>(pick.variantIndex)].clip,
                                          params);
        m_pickText = Format(u8"slot {}  pitch {}  vol {}", pick.variantIndex,
                            FormatFixed(pick.pitch, 2), FormatFixed(pick.volume, 2));
        m_status->SetText(m_pickText.AsView());
    }

    RefPtr<audio::AudioClip> SoundCueEditorPage::LoadSlotClip(usize slot)
    {
        const Guid& id = m_asset.clipIds[slot];
        if (id.IsNil() || m_context->Project() == nullptr)
        {
            return {};
        }
        if (RefPtr<audio::AudioClip>* cached = m_clipCache.Find(id))
        {
            return *cached;
        }
        draconic::content::Instance* instance = m_context->Project()->SourceDb().GetInstance(id);
        RefPtr<ISerializable> object =
            instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
        auto* asset = Cast<audio::AudioClipAsset>(object.Get());
        if (asset == nullptr)
        {
            return {};
        }
        const String path =
            PathJoin(m_context->Project()->SourcesRoot().AsView(), asset->fileName.View());
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        if (!bytes.HasValue())
        {
            return {};
        }
        audio::AudioClipMetadata metadata;
        if (!audio::ProbeAudioClipMetadata(
                Span<const byte>(bytes.Value().Data(), bytes.Value().Size()), metadata))
        {
            return {};
        }
        RefPtr<audio::AudioClip> clip = MakeRef<audio::AudioClip>(DefaultAllocator());
        clip->channels = metadata.channels;
        clip->sampleRate = metadata.sampleRate;
        clip->frameCount = metadata.frameCount;
        clip->durationSeconds = metadata.durationSeconds;
        clip->gain = asset->gain;
        clip->encodedData = Move(bytes.Value());
        m_clipCache.InsertOrAssign(id, clip);
        return clip;
    }
    const TypeInfo* SoundCuePageFactory::PrimaryType() const
    {
        return &audio::SoundCueAsset::StaticType();
    }

    UniquePtr<EditorPage> SoundCuePageFactory::CreatePage(EditorContext& context,
                                                          draconic::content::Instance& instance)
    {
        auto* page = DefaultAllocator().New<SoundCueEditorPage>(context, *m_host, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }
}
