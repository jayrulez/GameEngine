// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for BreadcrumbBar: SetPath split, SetSegments, GetSegment / GetPathUpTo round-trips.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-breadcrumb: PathSplitAndQueries")
{
    auto bar = core::MakeRef<BreadcrumbBar>(core::DefaultAllocator());
    CHECK(bar->SegmentCount() == 0);

    // Split on '/', trimming and dropping empty segments.
    bar->SetPath(u8"/home/ robert /Dev/");
    CHECK(bar->SegmentCount() == 3);
    CHECK(bar->GetSegment(0) == StringView{u8"home"});
    CHECK(bar->GetSegment(1) == StringView{u8"robert"});
    CHECK(bar->GetSegment(2) == StringView{u8"Dev"});
    CHECK(bar->GetSegment(99).IsEmpty()); // out of range

    String upTo(core::DefaultAllocator());
    bar->GetPathUpTo(1, upTo);
    CHECK(upTo == StringView{u8"home/robert"});

    // Replace via explicit segment list.
    core::Array<StringView> segs;
    segs.PushBack(StringView{u8"A"});
    segs.PushBack(StringView{u8"B"});
    bar->SetSegments(segs.AsSpan());
    CHECK(bar->SegmentCount() == 2);
    CHECK(bar->GetSegment(1) == StringView{u8"B"});
}
