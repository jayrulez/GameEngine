// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Profiler.GUI - the focus every view on a page reads. Covers the chosen frames, name and span,
// with each change reported to the views; a new source clearing the choices; a name's time in a
// frame, with a recursive scope counted once; and the longest and the median frame.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.profiler;
import foundation.profiler.gui;

#include "KnownSource.h"

using namespace foundation::core;
using namespace foundation::profiler;
using namespace foundation::profiler::gui;
using profiler_tests::KnownSource;

namespace
{
    struct Counting final : IProfileFocusClient
    {
        int sources = 0;
        int frames = 0;
        int choices = 0;
        void OnProfileSourceChanged() override { ++sources; }
        void OnProfileFramesChanged() override { ++frames; }
        void OnProfileChoiceChanged() override { ++choices; }
    };
}

TEST_CASE("ProfileFocus: the frames, the name and the span chosen, each change told once")
{
    KnownSource source;
    source.AddFrame(10);
    source.AddFrame(20);
    RefPtr<ProfileFocus> focus = MakeRef<ProfileFocus>(DefaultAllocator(), DefaultAllocator());
    Counting views;
    focus->Join(&views);
    focus->SetSource(&source);
    CHECK(views.sources == 1);
    CHECK(focus->Frames().empty);

    focus->SetFrame(1);
    CHECK(views.frames == 2); // once for the new source, then once for the choice
    CHECK(focus->Frames().first == 1u);
    focus->SetFrame(1); // the same frame, so the views are not told
    CHECK(views.frames == 2);

    focus->ChooseName(4, false);
    CHECK(focus->Name().Is(4, false));
    CHECK_FALSE(focus->Name().Is(4, true));
    focus->SetSpan(1030, 1010);
    CHECK(focus->Span().beginTick == 1010u);
    CHECK(focus->Span().endTick == 1030u);
    CHECK(views.choices == 3); // for the new source, the name and the span

    // Choosing other frames clears the span; the name stays.
    focus->SetFrame(0);
    CHECK_FALSE(focus->Span().any);
    CHECK(focus->Name().any);
    CHECK(views.choices == 4);

    // Another source clears every choice.
    KnownSource other;
    focus->SetSource(&other);
    CHECK(focus->Frames().empty);
    CHECK_FALSE(focus->Name().any);
    focus->Leave(&views);
    CHECK(focus->ClientCount() == 0);
}

TEST_CASE("ProfileFocus: a name's time in a frame, its passes apart, a recursive scope counted once")
{
    KnownSource source;
    const NameId walk = source.Name(u8"Walk");
    const NameId other = source.Name(u8"Other");
    FrameRecord& frame = source.AddFrame(20);
    // A Walk inside a Walk (it ended first, so it is listed first), then the outer Walk, then a
    // second Walk.
    KnownSource::AddScope(frame, walk, 1, 2, 4);
    KnownSource::AddScope(frame, walk, 0, 1, 6);
    KnownSource::AddScope(frame, other, 0, 6, 8);
    KnownSource::AddScope(frame, walk, 0, 8, 11);
    KnownSource::AddPass(frame, walk, 0, 7);
    CHECK(NameTicksInFrame(frame, ChosenName{walk, false, true}) == 8u); // 5 + 3; the inner Walk is inside the outer one and not counted again
    CHECK(NameTicksInFrame(frame, ChosenName{walk, true, true}) == 7u);
    CHECK(NameTicksInFrame(frame, ChosenName{}) == 0u);
}

TEST_CASE("ProfileFocus: the longest and the median frame of a range")
{
    KnownSource source;
    const u64 lengths[] = {10, 40, 20, 30, 15};
    for (const u64 ms : lengths)
        source.AddFrame(ms);
    FrameRange all = AllFrames(&source);
    CHECK(all.first == 0u);
    CHECK(all.last == 4u);
    CHECK(LongestFrame(source, all).first == 1u);
    CHECK(MedianFrame(source, all, DefaultAllocator()).first == 2u); // 10 15 [20] 30 40
    FrameRange some;
    some.first = 2;
    some.last = 3;
    some.empty = false;
    CHECK(MedianFrame(source, some, DefaultAllocator()).first == 2u); // with two in the middle, the shorter one
    CHECK(LongestFrame(source, FrameRange{}).empty);
    CHECK(AllFrames(nullptr).empty);
}
