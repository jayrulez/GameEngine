// EasingType -> foundation easing function mapping. Ported from Sedulous.Animation.Tests.EasingTypeTests.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

TEST_CASE("easing: Linear returns input unchanged")
{
    CHECK(ApplyEasing(EasingType::Linear, 0.0f) == 0.0f);
    CHECK(ApplyEasing(EasingType::Linear, 0.5f) == 0.5f);
    CHECK(ApplyEasing(EasingType::Linear, 1.0f) == 1.0f);
}

TEST_CASE("easing: ToFunction non-null + Apply boundary values for all types")
{
    for (i32 i = 0; i < static_cast<i32>(EasingType::Count); ++i)
    {
        const EasingType type = static_cast<EasingType>(i);
        CHECK(ToFunction(type) != nullptr);
        CHECK(Abs(ApplyEasing(type, 0.0f)) < 0.01f);
        CHECK(Abs(ApplyEasing(type, 1.0f) - 1.0f) < 0.01f);
    }
}

TEST_CASE("easing: in/out shape at midpoint")
{
    CHECK(ApplyEasing(EasingType::EaseInQuadratic, 0.5f) < 0.5f);             // slow start
    CHECK(ApplyEasing(EasingType::EaseOutQuadratic, 0.5f) > 0.5f);            // fast start
    CHECK(Abs(ApplyEasing(EasingType::EaseInOutCubic, 0.5f) - 0.5f) < 0.01f); // symmetric
}
