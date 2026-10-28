// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The animation clip page over its markup (Data/Editor/AnimationClipPage.gml) and a scratch project,
// without a host (no preview): the clip's rows its own (Looping, the root motion, the events a list
// named by each event's name and summed up by its time), its facts read only; each edit one undo
// step; a new event at the playhead; the events as the ruler's markers, a marker's move one step
// and a cancelled one none; the preview starting on the skeleton its import names; Travel and the
// clock the page's own, never the clip's.

#include <doctest/doctest.h>
#include <initializer_list>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.animation;

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

    gui::ListEdit ListEditOf(gui::ListEdit::Op kind, usize index)
    {
        gui::ListEdit edit;
        edit.Kind = kind;
        edit.Index = index;
        return edit;
    }

    Guid SkeletonId()
    {
        Guid id;
        (void)Guid::TryParse(u8"5d2b1c7e-9a41-4f0e-8a3b-0c6d7e8f9a10", id);
        return id;
    }

    template <typename T>
    void Fill(Array<T>& out, std::initializer_list<T> values)
    {
        out.Clear();
        for (const T& value : values)
            out.PushBack(value);
    }

    /// A scratch project holding one clip, "Walk": 1.2 s, two tracks, its events a footstep each side.
    struct Bed
    {
        String dir = String(u8"scratch_clip_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<AnimationClipEditorPage> page;
        foundation::content::Instance* instance = nullptr;

        Bed()
        {
            pipeline::RegisterAnimationAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            instance = project->SourceDb().RootGroup()->CreateInstance(u8"Walk", pipeline::AnimationClipAsset::StaticType());
            REQUIRE(instance != nullptr);
            pipeline::AnimationClipAsset asset;
            asset.skeleton = SkeletonId();
            auto& source = asset.source;
            source.name = String(u8"walk");
            source.duration = 1.2f;
            Fill<i32>(source.trackBone, {0, 1});
            Fill<u8>(source.trackKind, {0, 1});
            Fill<u8>(source.trackInterp, {0, 0});
            Fill<u32>(source.trackStart, {0, 2});
            Fill<u32>(source.trackCount, {2, 2});
            Fill<f32>(source.keyTimes, {0.0f, 1.2f, 0.0f, 1.2f});
            Fill<Float4>(source.keyValues, {Float4{}, Float4{}, Float4{0, 0, 0, 1}, Float4{0, 0, 0, 1}});
            Fill<f32>(source.eventTimes, {0.3f, 0.9f});
            source.eventNames.PushBack(String(u8"FootLeft"));
            source.eventNames.PushBack(String(u8"FootRight"));
            REQUIRE(instance->WriteObject(asset).IsOk());
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            page = MakeUnique<AnimationClipEditorPage>(DefaultAllocator(), context, Move(services), *instance);
            REQUIRE_FALSE(page->LoadFailed());
        }
        ~Bed()
        {
            page.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }

        foundation::animation::AnimationClipSource& Source() { return page->Clip()->source; }
        static void Edit(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
        {
            rows.BeginEdit(path);
            rows.CommitEdit(path, gui::PropertyWrite{value});
        }
        static Optional<gui::PropertyInfo> Info(gui::PropertyBag& rows, StringView parent, StringView name)
        {
            Array<gui::PropertyInfo> found;
            rows.GetProperties(parent, found);
            for (gui::PropertyInfo& info : found)
                if (info.Name == name)
                    return Optional<gui::PropertyInfo>(Move(info));
            return {};
        }
    };
}

TEST_CASE("clip page: the clip's rows its own, its facts read only, its events named by their names")
{
    Bed bed;
    CHECK(bed.page->Fact(u8"Name") == String(u8"walk"));
    CHECK(bed.page->Fact(u8"Duration") == String(u8"1.20 s"));
    CHECK(bed.page->Fact(u8"Tracks") == String(u8"2"));
    CHECK(bed.page->Fact(u8"Keys") == String(u8"4"));
    CHECK(bed.page->ClipRows().GetValue(u8"looping", 0).AsBool() == false);
    CHECK(bed.page->RootRows().GetValue(u8"yaw", 0).AsBool() == false);

    // The events as a list of groups, each named by its name, closed showing when it fires.
    const Optional<gui::PropertyInfo> first = Bed::Info(bed.page->EventRows(), u8"events", u8"0");
    REQUIRE(first.HasValue());
    CHECK(first->DisplayName == String(u8"FootLeft"));
    CHECK(first->Summary == String(u8"at 0.30 s"));
    CHECK(bed.page->EventRows().GetValue(u8"events.1.name", 0).AsString() == StringView(u8"FootRight"));
    CHECK(bed.page->EventRows().GetValue(u8"events.1.time", 0).AsFloat() == doctest::Approx(0.9));
}

TEST_CASE("clip page: each edit one undo step, a new event at the playhead")
{
    Bed bed;
    Bed::Edit(bed.page->ClipRows(), u8"looping", gui::Variant(true));
    CHECK(bed.Source().isLooping);
    CHECK(bed.page->IsDirty());
    Bed::Edit(bed.page->RootRows(), u8"yaw", gui::Variant(true));
    CHECK(bed.Source().rootMotion.yaw);
    Bed::Edit(bed.page->RootRows(), u8"bone", gui::Variant(StringView(u8" Hips ")));
    CHECK(bed.Source().rootMotion.rootBone == String(u8"Hips"));
    Bed::Edit(bed.page->EventRows(), u8"events.1.name", gui::Variant(StringView(u8"Land")));
    CHECK(bed.Source().eventNames[1] == String(u8"Land"));
    Bed::Edit(bed.page->EventRows(), u8"events.0.time", gui::Variant(5.0)); // kept within the clip
    CHECK(bed.Source().eventTimes[0] == doctest::Approx(1.2f));

    bed.page->Commands().Undo();
    CHECK(bed.Source().eventTimes[0] == doctest::Approx(0.3f));
    CHECK(bed.page->EventRows().GetValue(u8"events.0.time", 0).AsFloat() == doctest::Approx(0.3)); // the row follows
    bed.page->Commands().Undo();
    CHECK(bed.Source().eventNames[1] == String(u8"FootRight"));
    bed.page->Commands().Undo();
    bed.page->Commands().Undo();
    CHECK_FALSE(bed.Source().rootMotion.yaw);
    bed.page->Commands().Undo();
    CHECK_FALSE(bed.Source().isLooping);

    // Added where the playhead is, named to be renamed; removed by its place.
    bed.page->SetTime(0.6f);
    REQUIRE(bed.page->EventRows().EditList(u8"events", ListEditOf(gui::ListEdit::Op::Insert, 2)));
    REQUIRE(bed.Source().eventTimes.Size() == 3u);
    CHECK(bed.Source().eventTimes[2] == doctest::Approx(0.6f));
    CHECK(bed.Source().eventNames[2] == String(u8"Event"));
    REQUIRE(bed.page->EventRows().EditList(u8"events", ListEditOf(gui::ListEdit::Op::Remove, 0)));
    CHECK(bed.Source().eventTimes.Size() == 2u);
    CHECK(bed.Source().eventNames[0] == String(u8"FootRight"));
    bed.page->Commands().Undo();
    bed.page->Commands().Undo();
    CHECK(bed.Source().eventTimes.Size() == 2u);
    CHECK(bed.Source().eventNames[0] == String(u8"FootLeft"));
}

TEST_CASE("clip page: the events are the ruler's markers; a marker's move is one step, a cancelled one none")
{
    Bed bed;
    REQUIRE(bed.page->Ruler() != nullptr);
    CHECK(bed.page->Ruler()->GetMarkerSource() == bed.page.Get());
    REQUIRE(bed.page->MarkerCount() == 2u);
    CHECK(bed.page->GetMarker(0).Label == String(u8"FootLeft"));
    CHECK(bed.page->GetMarker(1).Time == doctest::Approx(0.9f));

    bed.page->BeginMarkerMove(1);
    bed.page->PreviewMarkerMove(1, 0.7f);
    CHECK(bed.Source().eventTimes[1] == doctest::Approx(0.7f));
    CHECK(bed.page->EventRows().GetValue(u8"events.1.time", 0).AsFloat() == doctest::Approx(0.7)); // live in its row
    bed.page->PreviewMarkerMove(1, 0.75f);
    bed.page->CommitMarkerMove();
    CHECK(bed.Source().eventTimes[1] == doctest::Approx(0.75f));
    CHECK(bed.page->IsDirty());
    bed.page->Commands().Undo();
    CHECK(bed.Source().eventTimes[1] == doctest::Approx(0.9f));
    bed.page->Commands().Redo();
    CHECK(bed.Source().eventTimes[1] == doctest::Approx(0.75f));
    bed.page->Commands().Undo();

    bed.page->BeginMarkerMove(0);
    bed.page->PreviewMarkerMove(0, 1.0f);
    bed.page->CancelMarkerMove();
    CHECK(bed.Source().eventTimes[0] == doctest::Approx(0.3f));
    CHECK(bed.page->EventRows().GetValue(u8"events.0.time", 0).AsFloat() == doctest::Approx(0.3));
    CHECK_FALSE(bed.page->Commands().CanUndo());

    // Chosen on the ruler: the same event chosen on the page.
    bed.page->ChooseEvent(1);
    CHECK(bed.page->Ruler()->GetChosenMarker() == 1);
}

TEST_CASE("clip page: the preview starts on the skeleton its import names; Travel and the clock are the page's own")
{
    Bed bed;
    REQUIRE(bed.page->Rig() != nullptr);
    CHECK(bed.page->Rig()->SkeletonId() == SkeletonId());
    CHECK(bed.page->Rig()->MeshId().IsNil());
    REQUIRE(bed.page->Rig()->SkeletonSlot() != nullptr);
    REQUIRE(bed.page->Rig()->MeshSlot() != nullptr);

    // Not cooked here: nothing to play, the clock still the page's to move.
    CHECK_FALSE(bed.page->CanPlay());
    bed.page->SetTime(0.5f);
    CHECK(bed.page->Time() == doctest::Approx(0.5f));
    CHECK(bed.page->Ruler()->GetTimeAxis()->Playhead() == doctest::Approx(0.5f));
    bed.page->Stop();
    CHECK(bed.page->Time() == 0.0f);
    CHECK_FALSE(bed.page->IsPlaying());

    // A scrub of the ruler pauses the clock where it is put.
    bed.page->Restart();
    CHECK(bed.page->IsPlaying());
    bed.page->Ruler()->GetTimeAxis()->SetPlayhead(0.4f);
    CHECK(bed.page->Time() == doctest::Approx(0.4f));
    CHECK_FALSE(bed.page->IsPlaying());

    bed.page->SetTravel(true);
    CHECK(bed.page->Travels());
    CHECK_FALSE(bed.page->IsDirty()); // never the clip's
}
