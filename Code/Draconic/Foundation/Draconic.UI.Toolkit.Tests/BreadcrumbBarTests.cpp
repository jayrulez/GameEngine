// Smoke test for BreadcrumbBar: SetPath split, SetSegments, GetSegment / GetPathUpTo round-trips.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-breadcrumb: PathSplitAndQueries")
{
    auto bar = foundation::MakeRef<BreadcrumbBar>(foundation::DefaultAllocator());
    CHECK(bar->SegmentCount() == 0);

    // Split on '/', trimming and dropping empty segments.
    bar->SetPath(u8"/home/ robert /Dev/");
    CHECK(bar->SegmentCount() == 3);
    CHECK(bar->GetSegment(0) == StringView{u8"home"});
    CHECK(bar->GetSegment(1) == StringView{u8"robert"});
    CHECK(bar->GetSegment(2) == StringView{u8"Dev"});
    CHECK(bar->GetSegment(99).IsEmpty()); // out of range

    String upTo(foundation::DefaultAllocator());
    bar->GetPathUpTo(1, upTo);
    CHECK(upTo == StringView{u8"home/robert"});

    // Replace via explicit segment list.
    foundation::Array<StringView> segs;
    segs.PushBack(StringView{u8"A"});
    segs.PushBack(StringView{u8"B"});
    bar->SetSegments(segs.AsSpan());
    CHECK(bar->SegmentCount() == 2);
    CHECK(bar->GetSegment(1) == StringView{u8"B"});
}
