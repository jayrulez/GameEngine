// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The sound cue page over its markup (Data/Editor/SoundCuePage.gml) and a scratch project, without
// a host (no audio engine: nothing plays): the cue's rows and a variant per clip with one more to
// fill, a clip picked or cleared one undo step that adds or takes out a variant, the cue's options,
// and a cue with no clip said so.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
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

    String Text(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return String(StringView(text, 36));
    }

    // A project with a cue ("Steps") whose first two slots hold clips, and one with none ("Hush").
    struct Bed
    {
        String dir = String(u8"scratch_sound_cue_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* steps = nullptr;
        foundation::content::Instance* hush = nullptr;
        const Guid left{0x51, 0x1};
        const Guid right{0x51, 0x2};
        const Guid third{0x51, 0x3};

        Bed()
        {
            pipeline::RegisterAudioAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            steps = top->CreateInstance(u8"Steps", pipeline::SoundCueAsset::StaticType());
            hush = top->CreateInstance(u8"Hush", pipeline::SoundCueAsset::StaticType());
            REQUIRE(steps != nullptr);
            REQUIRE(hush != nullptr);
            pipeline::SoundCueAsset cue;
            REQUIRE(hush->WriteObject(cue).IsOk());
            cue.clipIds[0] = left;
            cue.clipIds[1] = right;
            cue.weights[1] = 3.0f;
            REQUIRE(steps->WriteObject(cue).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<SoundCueEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<SoundCueEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };

    // The variant rows' names, in order ("Variant 1 | Variant 2 | Add a Variant").
    String Groups(gui::PropertyBag& rows)
    {
        Array<gui::PropertyInfo> top;
        rows.GetProperties(StringView{}, top);
        String out;
        for (const gui::PropertyInfo& info : top)
        {
            if (!out.IsEmpty())
                out += u8" | ";
            out += info.ShownName();
        }
        return out;
    }
}

TEST_CASE("sound cue page: the cue's rows, a variant per clip and one more to fill; no clip said so")
{
    Bed bed;
    UniquePtr<SoundCueEditorPage> page = bed.Open(*bed.steps);
    REQUIRE_FALSE(page->LoadFailed());
    CHECK(page->VariantCount() == 2u);
    CHECK(Groups(page->VariantRows()) == String(u8"Variant 1 | Variant 2 | Add a Variant"));
    CHECK(page->VariantRows().GetValue(u8"slot0.clip", 0).AsString() == Text(bed.left).AsView());
    CHECK(page->VariantRows().GetValue(u8"slot1.weight", 0).AsFloat() == doctest::Approx(3.0));
    REQUIRE(page->VariantRows().FindInfo(u8"slot0.clip") != nullptr);
    CHECK(page->VariantRows().FindInfo(u8"slot0.clip")->ReferenceType == &foundation::audio::AudioClip::StaticType());
    CHECK(page->VariantRows().FindInfo(u8"slot2.weight") == nullptr); // the slot to fill has no weight yet
    CHECK(page->CueRows().FindInfo(u8"pitchMax")->Hint == StringView(u8"slider"));
    CHECK(page->Notice(u8"empty").IsEmpty());
    CHECK_FALSE(page->CanPlay()); // no audio engine here
    CHECK(ServiceOf<IPlaybackPage>(page.Get()) == page.Get());

    UniquePtr<SoundCueEditorPage> empty = bed.Open(*bed.hush);
    CHECK(Groups(empty->VariantRows()) == String(u8"Add a Variant"));
    CHECK_FALSE(empty->Notice(u8"empty").IsEmpty());
}

TEST_CASE("sound cue page: a clip picked adds a variant, cleared takes it out, each one undo step")
{
    Bed bed;
    UniquePtr<SoundCueEditorPage> page = bed.Open(*bed.steps);
    page->EditRow(page->VariantRows(), u8"slot2.clip", gui::Variant(Text(bed.third)));
    page->Refresh(); // the rows made again for three
    CHECK(page->VariantCount() == 3u);
    CHECK(Groups(page->VariantRows()) == String(u8"Variant 1 | Variant 2 | Variant 3 | Add a Variant"));
    CHECK(page->IsDirty());

    page->EditRow(page->VariantRows(), u8"slot0.clip", gui::Variant(StringView{}));
    page->Refresh();
    CHECK(page->VariantCount() == 2u);
    // The first slot is now the one to fill; the others keep their order.
    CHECK(Groups(page->VariantRows()) == String(u8"Add a Variant | Variant 1 | Variant 2"));

    page->Commands().Undo();
    page->Refresh();
    CHECK(page->VariantCount() == 3u);
    page->Commands().Undo();
    page->Refresh();
    CHECK(page->VariantCount() == 2u);
    CHECK(page->VariantRows().GetValue(u8"slot0.clip", 0).AsString() == Text(bed.left).AsView());
}

TEST_CASE("sound cue page: how it picks and its ranges, each one undo step")
{
    Bed bed;
    UniquePtr<SoundCueEditorPage> page = bed.Open(*bed.steps);
    page->EditRow(page->CueRows(), u8"mode", gui::Variant(static_cast<i64>(2)));
    page->EditRow(page->CueRows(), u8"pitchMax", gui::Variant(1.25));
    auto* cue = Cast<pipeline::SoundCueAsset>(page->Object());
    REQUIRE(cue != nullptr);
    CHECK(cue->mode == 2u);
    CHECK(cue->pitchMax == doctest::Approx(1.25f));
    page->Commands().Undo();
    CHECK(cue->pitchMax == doctest::Approx(1.0f));
    CHECK(cue->mode == 2u);
    page->Commands().Undo();
    CHECK(cue->mode == 0u);
}
