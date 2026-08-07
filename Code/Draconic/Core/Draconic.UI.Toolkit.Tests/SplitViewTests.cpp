// Smoke test for SplitView: panes, ratio clamp + event, and a basic horizontal layout split.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-splitview: PanesRatioAndLayout")
{
    auto split = foundation::MakeRef<SplitView>(foundation::DefaultAllocator());
    CHECK(split->Orientation == Orientation::Horizontal);
    CHECK(split->SplitRatio() == doctest::Approx(0.5f));

    auto first = foundation::MakeRef<Panel>(foundation::DefaultAllocator());
    auto second = foundation::MakeRef<Panel>(foundation::DefaultAllocator());
    split->SetPanes(first.Get(), second.Get());
    CHECK(split->FirstPane() == first.Get());
    CHECK(split->SecondPane() == second.Get());
    CHECK(split->ChildCount() == 2u);

    // Ratio clamps to [0,1] and fires OnSplitChanged.
    bool fired = false;
    f32 lastRatio = -1.0f;
    split->OnSplitChanged.Add(
        [&](SplitView*, f32 r)
        {
            fired = true;
            lastRatio = r;
        });
    split->SetSplitRatio(2.0f);
    CHECK(split->SplitRatio() == doctest::Approx(1.0f));
    CHECK(fired);
    CHECK(lastRatio == doctest::Approx(1.0f));

    // Basic horizontal layout: available = 200 - DividerSize(6) = 194, ratio 0.5 -> 97 each.
    split->SetSplitRatio(0.5f);
    split->Measure(BoxConstraints::Tight(200.0f, 100.0f));
    split->Layout(0.0f, 0.0f, 200.0f, 100.0f);
    CHECK(first->Width() == doctest::Approx(97.0f));
    CHECK(second->Width() == doctest::Approx(97.0f));
    CHECK(first->Height() == doctest::Approx(100.0f));
}
