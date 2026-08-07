// Draconic::EditorAudio - the `:sound_cue_page` partition.
//
// SoundCuePage (audio.md P3): the cue editor - eight variant slot rows (clip picker +
// weight), cue-level mode/jitter fields, and AUDITION that resolves through the REAL
// ResolveSoundCue (same weights, no-repeat state, and jitter the game uses) and plays
// through the runtime engine. Save writes the asset and nudges the validating recook.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.audio:sound_cue_page;

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

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace audio = draconic::audio;

    class SoundCueEditorPage final : public app::UIEditorPage
    {
    public:
        SoundCueEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                           draconic::content::Instance& instance)
            : m_context(&context), m_title(instance.Name())
        {
            SetInstanceId(instance.Id());
            m_audio = host.Ctx().GetSubsystem<audio::AudioSubsystem>();
            if (RefPtr<ISerializable> object = instance.ReadObject())
            {
                if (auto* asset = Cast<audio::SoundCueAsset>(object.Get()))
                {
                    // Field-wise copy (the Asset base is non-copyable).
                    m_asset.fileName = asset->fileName; // SourcePath copies
                    for (usize i = 0; i < audio::kSoundCueSlotCount; ++i)
                    {
                        m_asset.clipIds[i] = asset->clipIds[i];
                        m_asset.weights[i] = asset->weights[i];
                    }
                    m_asset.mode = asset->mode;
                    m_asset.pitchMin = asset->pitchMin;
                    m_asset.pitchMax = asset->pitchMax;
                    m_asset.volumeMin = asset->volumeMin;
                    m_asset.volumeMax = asset->volumeMax;
                }
            }

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 6.0f;

            // Slot rows: "<clip name>" [Pick...] [Clear] weight [field]
            for (usize i = 0; i < audio::kSoundCueSlotCount; ++i)
            {
                auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6.0f;

                m_slotLabels[i] = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"(empty)"));
                m_slotLabels[i]->FontSize.SetValue(13.0f);
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Grow = 1.0f;
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_slotLabels[i].Get(), lp);
                }
                SoundCueEditorPage* self = this;
                const usize slot = i;
                auto pick = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pick..."));
                pick->OnClick.Add([self, slot](ui::ButtonBase*) { self->PickClip(slot); });
                row->AddView(pick.Get());
                auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
                clear->OnClick.Add(
                    [self, slot](ui::ButtonBase*)
                    {
                        self->m_asset.clipIds[slot] = Guid{};
                        self->RefreshSlot(slot);
                        self->MarkDirty();
                    });
                row->AddView(clear.Get());

                auto weightLabel = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8"weight"));
                weightLabel->FontSize.SetValue(12.0f);
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(weightLabel.Get(), lp);
                }
                m_weightFields[i] = MakeRef<ui::NumericField>(DefaultAllocator());
                m_weightFields[i]->SetMin(0.0);
                m_weightFields[i]->SetMax(100.0);
                m_weightFields[i]->SetValue(m_asset.weights[i]);
                m_weightFields[i]->OnValueChanged.Add(
                    [self, slot](ui::NumericField*, f64 value)
                    {
                        self->m_asset.weights[slot] = static_cast<f32>(value);
                        self->MarkDirty();
                    });
                row->AddView(m_weightFields[i].Get());

                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Width = ui::SizeSpec::Match();
                    column->AddView(row.Get(), lp);
                }
            }

            // Cue-level: mode + pitch/volume jitter.
            {
                auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6.0f;
                SoundCueEditorPage* self = this;
                m_modeButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8""));
                m_modeButton->OnClick.Add(
                    [self](ui::ButtonBase*)
                    {
                        self->m_asset.mode = static_cast<u8>((self->m_asset.mode + 1) % 3);
                        self->RefreshModeButton();
                        self->MarkDirty();
                    });
                row->AddView(m_modeButton.Get());
                AddJitterField(*row, u8"pitch min", m_asset.pitchMin);
                AddJitterField(*row, u8"pitch max", m_asset.pitchMax);
                AddJitterField(*row, u8"vol min", m_asset.volumeMin);
                AddJitterField(*row, u8"vol max", m_asset.volumeMax);
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Width = ui::SizeSpec::Match();
                    column->AddView(row.Get(), lp);
                }
            }

            // Audition: resolve + play, exactly what the game does per trigger.
            {
                auto row = MakeRef<ui::FlexLayout>(DefaultAllocator());
                row->Direction = ui::Orientation::Horizontal;
                row->Spacing = 6.0f;
                SoundCueEditorPage* self = this;
                auto play = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Audition"));
                play->OnClick.Add([self](ui::ButtonBase*) { self->Audition(); });
                row->AddView(play.Get());
                m_status = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
                m_status->FontSize.SetValue(12.0f);
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->AlignSelf = ui::Align::Center;
                    row->AddView(m_status.Get(), lp);
                }
                {
                    auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                    lp->Width = ui::SizeSpec::Match();
                    column->AddView(row.Get(), lp);
                }
            }

            m_content = column;
            for (usize i = 0; i < audio::kSoundCueSlotCount; ++i)
            {
                RefreshSlot(i);
            }
            RefreshModeButton();
        }

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }

        void OnUpdate(runtime::IApplicationHost&, f32) override;

        [[nodiscard]] Status Save() override;

        void OnClose() override;

    private:
        void AddJitterField(ui::FlexLayout& row, StringView label, f32& target);

        void PickClip(usize slot);

        void RefreshSlot(usize slot);

        void RefreshModeButton();

        // One REAL trigger: build a transient SoundCue from the slots (source files ->
        // in-memory clips, cached per page), resolve with the page's play state, play.
        void Audition();

        [[nodiscard]] RefPtr<audio::AudioClip> LoadSlotClip(usize slot);

        EditorContext* m_context = nullptr;
        audio::AudioSubsystem* m_audio = nullptr;
        String m_title;
        audio::SoundCueAsset m_asset;
        Random m_rng;
        i32 m_lastVariant = -1;
        u32 m_sequentialCursor = 0;
        audio::VoiceHandle m_voice;
        String m_pickText;
        HashMap<Guid, RefPtr<audio::AudioClip>> m_clipCache;
        RefPtr<ui::View> m_content;
        RefPtr<ui::Label> m_slotLabels[audio::kSoundCueSlotCount];
        RefPtr<ui::NumericField> m_weightFields[audio::kSoundCueSlotCount];
        Array<RefPtr<ui::NumericField>> m_jitterFields;
        RefPtr<ui::Button> m_modeButton;
        RefPtr<ui::Label> m_status;
    };

    class SoundCuePageFactory final : public IEditorPageFactory
    {
    public:
        explicit SoundCuePageFactory(runtime::IApplicationHost& host) : m_host(&host) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
    };
}
