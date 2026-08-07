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

export module draconic.editor.audio;

export import :sound_cue_page;
export import :bus_layout_page;

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

    /// The peak waveform strip: symmetric per-bucket bars around the midline plus an
    /// optional playhead (fraction of the clip; < 0 hides it).
    class WaveformView final : public ui::View
    {
    public:
        void SetPeaks(Array<f32> peaks);
        void SetPlayheadFraction(f32 fraction);

        void OnDraw(ui::UIDrawContext& ctx) override;

    private:
        Array<f32> m_peaks;
        f32 m_playhead = -1.0f;
    };

    class AudioClipEditorPage final : public app::UIEditorPage
    {
    public:
        AudioClipEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                            draconic::content::Instance& instance)
            : m_context(&context), m_title(instance.Name())
        {
            SetInstanceId(instance.Id());
            m_audio = host.Ctx().GetSubsystem<audio::AudioSubsystem>();
            LoadClip(instance);

            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;
            column->Spacing = 8.0f;

            m_info = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
            m_info->FontSize.SetValue(13.0f);
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(m_info.Get(), lp);
            }

            m_waveform = MakeRef<WaveformView>(DefaultAllocator());
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                lp->Height = ui::SizeSpec::Fixed(ui::Unit::Px(160));
                column->AddView(m_waveform.Get(), lp);
            }

            auto controls = MakeRef<ui::FlexLayout>(DefaultAllocator());
            controls->Direction = ui::Orientation::Horizontal;
            controls->Spacing = 8.0f;
            AudioClipEditorPage* self = this;
            m_playButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Play"));
            m_playButton->OnClick.Add([self](ui::ButtonBase*) { self->Audition(); });
            controls->AddView(m_playButton.Get());
            m_stopButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Stop"));
            m_stopButton->OnClick.Add([self](ui::ButtonBase*) { self->StopAudition(); });
            controls->AddView(m_stopButton.Get());
            m_status = MakeRef<ui::Label>(DefaultAllocator(), StringView(u8""));
            m_status->FontSize.SetValue(12.0f);
            controls->AddView(m_status.Get());
            {
                auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                lp->Width = ui::SizeSpec::Match();
                column->AddView(controls.Get(), lp);
            }

            m_content = column;
            RefreshInfo();
        }

        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] Status Save() override;

        void OnUpdate(runtime::IApplicationHost&, f32) override;

        void OnClose() override { StopAudition(); }

    private:
        void LoadClip(draconic::content::Instance& instance);

        void RefreshInfo();

        void Audition();

        void StopAudition();

        EditorContext* m_context = nullptr;
        audio::AudioSubsystem* m_audio = nullptr; // the RUNTIME context's subsystem
        String m_title;
        bool m_loop = false;
        RefPtr<audio::AudioClip> m_clip;
        audio::VoiceHandle m_voice;
        RefPtr<ui::View> m_content;
        RefPtr<ui::Label> m_info;
        RefPtr<ui::Label> m_status;
        RefPtr<ui::Button> m_playButton;
        RefPtr<ui::Button> m_stopButton;
        RefPtr<WaveformView> m_waveform;
    };

    class AudioClipPageFactory final : public IEditorPageFactory
    {
    public:
        explicit AudioClipPageFactory(runtime::IApplicationHost& host) : m_host(&host) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
    };

    inline void RegisterAudioClipEditor(EditorContext& context, runtime::IApplicationHost& host)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<AudioClipPageFactory>(host), DefaultAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<SoundCuePageFactory>(host), DefaultAllocator()));
    }
}
