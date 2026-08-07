// Smoke test for the toolkit CurveCanvas: set channels, set keys, read them back, check defaults.
// No font/VG rendering, no input simulation (events fire only from mouse handlers).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-curvecanvas: SetChannelsAndKeys")
{
    auto cv = foundation::MakeRef<CurveCanvas>(foundation::DefaultAllocator());

    // Defaults on a fresh canvas.
    CHECK(cv->ChannelCount() == 0);
    CHECK(cv->SelectedChannel() == -1);
    CHECK(cv->SelectedKeyIndex() == -1);
    CHECK(cv->MaxKeys == 8);
    CHECK(cv->LinkedTime == false);
    CHECK(cv->AutoFitValueRange == true);

    // Configure two channels.
    ChannelDescriptor descs[2];
    descs[0].Name = String(u8"X");
    descs[0].StrokeColor = foundation::Color{1, 0, 0, 1};
    descs[0].Interpolation = CurveInterpolation::Hermite;
    descs[1].Name = String(u8"Y");
    descs[1].StrokeColor = foundation::Color{0, 1, 0, 1};
    descs[1].Interpolation = CurveInterpolation::Linear;

    cv->SetChannels(Span<const ChannelDescriptor>(descs, 2));
    CHECK(cv->ChannelCount() == 2);
    // Selection resets to channel 0 when channels exist.
    CHECK(cv->SelectedChannel() == 0);
    CHECK(cv->SelectedKeyIndex() == -1);

    // Descriptor round-trips.
    CHECK(cv->GetChannelDescriptor(1).Interpolation == CurveInterpolation::Linear);

    // Channels start with no keys.
    CHECK(cv->GetKeyCount(0) == 0);
    CHECK(cv->GetKeyCount(1) == 0);
}

TEST_CASE("toolkit-curvecanvas: SetKeysRoundTrip")
{
    auto cv = foundation::MakeRef<CurveCanvas>(foundation::DefaultAllocator());

    ChannelDescriptor descs[1];
    descs[0].Name = String(u8"V");
    cv->SetChannels(Span<const ChannelDescriptor>(descs, 1));

    CurveCanvas::Key keys[3] = {
        CurveCanvas::Key{0.0f, 0.0f},
        CurveCanvas::Key{0.5f, 1.0f, 0.25f, -0.25f, TangentMode::Free},
        CurveCanvas::Key{1.0f, 0.0f},
    };
    cv->SetKeys(0, Span<const CurveCanvas::Key>(keys, 3));

    CHECK(cv->GetKeyCount(0) == 3);

    const CurveCanvas::Key k1 = cv->GetKey(0, 1);
    CHECK(k1.Time == doctest::Approx(0.5f));
    CHECK(k1.Value == doctest::Approx(1.0f));
    CHECK(k1.TangentIn == doctest::Approx(0.25f));
    CHECK(k1.TangentOut == doctest::Approx(-0.25f));
    CHECK(k1.Mode == TangentMode::Free);

    // Out-of-range channel index is a no-op (does not crash / mutate).
    cv->SetKeys(5, Span<const CurveCanvas::Key>(keys, 3));
    CHECK(cv->GetKeyCount(0) == 3);
}

TEST_CASE("toolkit-curvecanvas: ValueRangeDefaults")
{
    auto cv = foundation::MakeRef<CurveCanvas>(foundation::DefaultAllocator());
    CHECK(cv->ValueMin == doctest::Approx(0.0f));
    CHECK(cv->ValueMax == doctest::Approx(1.0f));

    // Explicit value range is honored when auto-fit is disabled.
    cv->AutoFitValueRange = false;
    cv->ValueMin = -2.0f;
    cv->ValueMax = 3.0f;
    CHECK(cv->ValueMin == doctest::Approx(-2.0f));
    CHECK(cv->ValueMax == doctest::Approx(3.0f));
}
