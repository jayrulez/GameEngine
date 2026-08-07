// Draconic GUI - Transformable tests. Derived from eepp Transformable behavior
// (position/rotation/scale composed into a lazily-cached transform).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    constexpr float kHalfPi = 1.57079632679489662f;

    void CheckPoint(foundation::Float2 got, float x, float y)
    {
        CHECK(got.x == doctest::Approx(x));
        CHECK(got.y == doctest::Approx(y));
    }
}

TEST_CASE("transformable: defaults are identity")
{
    Transformable t;
    CHECK(t.GetPosition() == foundation::Float2{0.0f, 0.0f});
    CHECK(t.GetScale() == foundation::Float2{1.0f, 1.0f});
    CHECK(t.GetRotation() == 0.0f);
    CHECK(t.GetTransform().IsIdentity());
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{3.0f, 4.0f}), 3.0f, 4.0f);
}

TEST_CASE("transformable: position translates points")
{
    Transformable t;
    t.SetPosition(foundation::Float2{10.0f, 20.0f});
    CHECK(t.GetPosition() == foundation::Float2{10.0f, 20.0f});
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{0.0f, 0.0f}), 10.0f, 20.0f);
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{5.0f, 5.0f}), 15.0f, 25.0f);
}

TEST_CASE("transformable: scale about origin")
{
    Transformable t;
    t.SetScaleOrigin(foundation::Float2{10.0f, 10.0f});
    t.SetScale(foundation::Float2{2.0f, 2.0f});
    // The scale origin stays fixed.
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{10.0f, 10.0f}), 10.0f, 10.0f);
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{11.0f, 10.0f}), 12.0f, 10.0f);
}

TEST_CASE("transformable: rotation about origin")
{
    Transformable t;
    t.SetRotationOrigin(foundation::Float2{5.0f, 5.0f});
    t.SetRotation(kHalfPi);
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{5.0f, 5.0f}), 5.0f,
               5.0f); // pivot fixed
}

TEST_CASE("transformable: relative move/rotate/scale")
{
    Transformable t;
    t.Move(foundation::Float2{1.0f, 2.0f});
    t.Move(foundation::Float2{3.0f, 4.0f});
    CHECK(t.GetPosition() == foundation::Float2{4.0f, 6.0f});

    t.Scale(foundation::Float2{2.0f, 3.0f});
    t.Scale(foundation::Float2{2.0f, 1.0f});
    CHECK(t.GetScale() == foundation::Float2{4.0f, 3.0f});

    t.Rotate(0.5f);
    t.Rotate(0.25f);
    CHECK(t.GetRotation() == doctest::Approx(0.75f));
}

TEST_CASE("transformable: inverse round-trips")
{
    Transformable t;
    t.SetPosition(foundation::Float2{10.0f, -5.0f});
    t.SetScale(foundation::Float2{2.0f, 4.0f});
    const foundation::Float2 p{3.0f, 7.0f};
    const foundation::Float2 mapped = t.GetTransform().TransformPoint(p);
    CheckPoint(t.GetInverseTransform().TransformPoint(mapped), p.x, p.y);
}

TEST_CASE("transformable: transform recomputes after change")
{
    Transformable t;
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{0.0f, 0.0f}), 0.0f, 0.0f);
    t.SetPosition(foundation::Float2{100.0f, 0.0f});
    CheckPoint(t.GetTransform().TransformPoint(foundation::Float2{0.0f, 0.0f}), 100.0f, 0.0f);
}
