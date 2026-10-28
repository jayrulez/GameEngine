// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The audio clip page over its markup (Data/Editor/AudioClipPage.gml) and a scratch project,
// without a host (no audio engine: nothing plays): the source read into its waveform and facts,
// the transport bound to the playback actions, an option an undo step the audition follows, and a
// missing source said in the waveform's place.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.audio;
import audio.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.audio;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;
namespace audio = foundation::audio;

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // A project with half a second of stereo at 8 kHz ("tone.wav") under a clip ("Tone"), and a
    // clip over a file that is not there ("Gone").
    struct Bed
    {
        String dir = String(u8"scratch_audio_clip_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* tone = nullptr;
        foundation::content::Instance* gone = nullptr;

        Bed()
        {
            pipeline::RegisterAudioAssets(); // read back by its type, as the editor registers it
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            Array<i16> samples;
            samples.Resize(8000); // 4000 frames of two channels
            for (usize i = 0; i < samples.Size(); ++i)
                samples[i] = (i / 8) % 2 == 0 ? i16{20000} : i16{-20000};
            Array<byte> wav;
            REQUIRE(audio::EncodeWavFromPcm16(Span<const i16>(samples.Data(), samples.Size()), 2, 8000, wav));
            REQUIRE(WriteFile(PathJoin(project->SourcesRoot().AsView(), u8"tone.wav").AsView(), Span<const byte>(wav.Data(), wav.Size())).IsOk());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            tone = top->CreateInstance(u8"Tone", pipeline::AudioClipAsset::StaticType());
            gone = top->CreateInstance(u8"Gone", pipeline::AudioClipAsset::StaticType());
            REQUIRE(tone != nullptr);
            REQUIRE(gone != nullptr);
            pipeline::AudioClipAsset asset;
            asset.fileName = foundation::vfs::SourcePath(u8"tone.wav");
            REQUIRE(tone->WriteObject(asset).IsOk());
            asset.fileName = foundation::vfs::SourcePath(u8"gone.wav");
            REQUIRE(gone->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<AudioClipEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<AudioClipEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };
}

TEST_CASE("audio clip page: registered for the clip, with the clip thumbnails")
{
    const app::AssetPageServicesSource none; // no services: the registration only, before the context it outlives
    EditorContext context{DefaultAllocator()};
    RegisterAudioClipEditor(context, none);
    IEditorPageFactory* found = context.Pages().FindFactory(pipeline::AudioClipAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::AudioClipAsset::StaticType());
}

TEST_CASE("audio clip page: the source read into its waveform and facts; the transport the playback actions'")
{
    Bed bed;
    UniquePtr<AudioClipEditorPage> page = bed.Open(*bed.tone);
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Clip() != nullptr);
    CHECK(page->Clip()->channels == 2u);
    CHECK(page->Clip()->sampleRate == 8000u);
    CHECK(page->Clip()->frameCount == 4000u);
    REQUIRE(page->Waveform() != nullptr);
    CHECK(page->Waveform()->Peaks().Size() == AudioClipEditorPage::kPeakBuckets);
    CHECK(page->Waveform()->Peaks()[0] > 0.5f); // a loud tone throughout
    CHECK(page->Fact(u8"Channels") == StringView(u8"Stereo"));
    CHECK(page->Fact(u8"Sample Rate") == StringView(u8"8000 Hz"));
    CHECK(page->Fact(u8"Length") == StringView(u8"0.50 s (4000 frames)"));
    CHECK(page->State() == StringView(u8"Stopped"));
    CHECK_FALSE(page->CanPlay()); // no audio engine here
    CHECK_FALSE(page->IsPlaying());
    CHECK(ServiceOf<IPlaybackPage>(page.Get()) == page.Get()); // what the playback actions run over

    page->SetAuditionVolume(2.0f);
    CHECK(page->AuditionVolume() == doctest::Approx(1.0f)); // 0 to 1
    page->SetAuditionVolume(0.25f);
    CHECK(page->AuditionVolume() == doctest::Approx(0.25f));
}

TEST_CASE("audio clip page: an option is one undo step, and the audition follows it")
{
    Bed bed;
    UniquePtr<AudioClipEditorPage> page = bed.Open(*bed.tone);
    REQUIRE(page->Clip() != nullptr);
    CHECK_FALSE(page->Clip()->loop);
    gui::PropertyWrite write;
    write.Value = gui::Variant(true);
    page->Edit(u8"loop", write);
    CHECK(page->IsDirty());
    CHECK(page->Clip()->loop); // the next play loops
    page->Commands().Undo();
    CHECK_FALSE(page->Clip()->loop);
}

TEST_CASE("audio clip page: a missing source said in the waveform's place")
{
    Bed bed;
    UniquePtr<AudioClipEditorPage> page = bed.Open(*bed.gone);
    REQUIRE_FALSE(page->LoadFailed());
    CHECK(page->Clip() == nullptr);
    CHECK_FALSE(page->CanPlay());
    auto* empty = app::FindById<gui::Node>(*page->Content(), u8"audio-empty");
    auto* stage = app::FindById<gui::Node>(*page->Content(), u8"audio-stage");
    REQUIRE(empty != nullptr);
    REQUIRE(stage != nullptr);
    CHECK(empty->IsDisplayed());
    CHECK_FALSE(stage->IsDisplayed());
    auto* why = app::FindById<gui::Label>(*page->Content(), u8"audio-empty-text");
    REQUIRE(why != nullptr);
    CHECK(why->GetText() == StringView(u8"gone.wav is missing or did not decode."));
}
