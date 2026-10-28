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

    // A list edit by its parts, as the grid's list makes one.
    gui::ListEdit ListEditOf(gui::ListEdit::Op kind, usize index, usize to = 0)
    {
        gui::ListEdit edit;
        edit.Kind = kind;
        edit.Index = index;
        edit.To = to;
        return edit;
    }

    // The variants' clips as the list shows them ("left | right | -" with one to fill).
    String Clips(SoundCueEditorPage& page, const Bed& bed)
    {
        String out;
        const usize count = page.VariantRows().ElementCount(u8"variants", 0);
        for (usize i = 0; i < count; ++i)
        {
            const String text = page.VariantRows().GetValue(Format(u8"variants.{}.clip", i).AsView(), 0).AsString();
            if (!out.IsEmpty())
                out += u8" | ";
            out += text == Text(bed.left) ? StringView(u8"left") : text == Text(bed.right) ? StringView(u8"right") : text == Text(bed.third) ? StringView(u8"third") : StringView(u8"-");
        }
        return out;
    }
}

TEST_CASE("sound cue page: the variants a list as every page's, each a clip and its weight; no clip said so")
{
    Bed bed;
    UniquePtr<SoundCueEditorPage> page = bed.Open(*bed.steps);
    REQUIRE_FALSE(page->LoadFailed());
    CHECK(page->VariantCount() == 2u);
    gui::PropertyInfo* list = page->VariantRows().FindInfo(u8"variants");
    REQUIRE(list != nullptr);
    CHECK(list->Kind == gui::PropertyKind::List);
    CHECK(list->MaxCount == 8u);
    CHECK(Clips(*page, bed) == String(u8"left | right"));
    CHECK(page->VariantRows().GetValue(u8"variants.1.weight", 0).AsFloat() == doctest::Approx(3.0));
    Array<gui::PropertyInfo> fields;
    page->VariantRows().GetProperties(u8"variants.0", fields);
    REQUIRE(fields.Size() == 2u);
    CHECK(fields[0].ReferenceType == &foundation::audio::AudioClip::StaticType());
    CHECK(page->CueRows().FindInfo(u8"pitchMax")->Hint == StringView(u8"slider"));
    CHECK(page->Notice(u8"empty").IsEmpty());
    CHECK_FALSE(page->CanPlay()); // no audio engine here
    CHECK(ServiceOf<IPlaybackPage>(page.Get()) == page.Get());

    UniquePtr<SoundCueEditorPage> empty = bed.Open(*bed.hush);
    CHECK(empty->VariantRows().ElementCount(u8"variants", 0) == 0u);
    CHECK_FALSE(empty->Notice(u8"empty").IsEmpty());
}

TEST_CASE("sound cue page: Add makes a variant to fill, its clip picked joins the cue; removed and moved, each one undo step")
{
    Bed bed;
    UniquePtr<SoundCueEditorPage> page = bed.Open(*bed.steps);
    // Add, as the list's button: a variant with no clip yet, the cue unchanged.
    REQUIRE(page->VariantRows().EditList(u8"variants", ListEditOf(gui::ListEdit::Op::Insert, 2)));
    CHECK(Clips(*page, bed) == String(u8"left | right | -"));
    CHECK(page->VariantCount() == 2u);
    CHECK_FALSE(page->Commands().CanUndo());
    // Its clip picked: it joins the cue, one step.
    page->EditRow(page->VariantRows(), u8"variants.2.clip", gui::Variant(Text(bed.third)));
    CHECK(page->VariantCount() == 3u);
    CHECK(Clips(*page, bed) == String(u8"left | right | third"));
    CHECK(page->IsDirty());

    // Moved, as an element's grip does: the cue's order with it.
    REQUIRE(page->VariantRows().EditList(u8"variants", ListEditOf(gui::ListEdit::Op::Move, 2, 0)));
    CHECK(Clips(*page, bed) == String(u8"third | left | right"));
    auto* cue = Cast<pipeline::SoundCueAsset>(page->Object());
    REQUIRE(cue != nullptr);
    CHECK(cue->clipIds[0] == bed.third);
    CHECK(cue->weights[2] == doctest::Approx(3.0f)); // right's weight went with it

    // Removed, as an element's menu does.
    REQUIRE(page->VariantRows().EditList(u8"variants", ListEditOf(gui::ListEdit::Op::Remove, 1)));
    CHECK(Clips(*page, bed) == String(u8"third | right"));
    CHECK(page->VariantCount() == 2u);

    page->Commands().Undo(); // the removal
    CHECK(Clips(*page, bed) == String(u8"third | left | right"));
    page->Commands().Undo(); // the move
    CHECK(Clips(*page, bed) == String(u8"left | right | third"));
    page->Commands().Undo(); // the pick
    CHECK(page->VariantCount() == 2u);
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
