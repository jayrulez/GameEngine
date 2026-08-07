// Draconic GUI - ProgressBar tests: clamping and draw.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

TEST_CASE("progressbar: clamps to [0,1] and draws track + fill")
{
    auto p = foundation::MakeRef<ProgressBar>(foundation::DefaultAllocator());
    p->SetProgress(0.5f);
    CHECK(p->GetProgress() == doctest::Approx(0.5f));
    p->SetProgress(2.0f);
    CHECK(p->GetProgress() == doctest::Approx(1.0f));
    p->SetProgress(-1.0f);
    CHECK(p->GetProgress() == doctest::Approx(0.0f));

    p->SetSize(foundation::Float2{120.0f, 10.0f});
    p->SetProgress(0.5f);
    draconic::vg::VGContext ctx;
    DrawContext dc{ctx};
    p->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}
