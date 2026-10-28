// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit vector fields: construct each, set/get value, fire OnValueChanged.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-vectorfields: Vector2FieldRoundTrip")
{
    auto f = core::MakeRef<Vector2Field>(core::DefaultAllocator());
    CHECK(f->ChildCount() == 2u);
    f->SetValue(Float2{3.0f, 4.0f});
    CHECK(f->Value().x == doctest::Approx(3.0f));
    CHECK(f->Value().y == doctest::Approx(4.0f));
    f->SetRange(-10.0, 10.0);
    f->SetStep(0.5);
    CHECK(f->Step() == doctest::Approx(0.5));
}

TEST_CASE("toolkit-vectorfields: Vector3And4")
{
    auto f3 = core::MakeRef<Vector3Field>(core::DefaultAllocator());
    CHECK(f3->ChildCount() == 3u);
    f3->SetValue(Float3{1.0f, 2.0f, 3.0f});
    CHECK(f3->Value().z == doctest::Approx(3.0f));

    auto f4 = core::MakeRef<Vector4Field>(core::DefaultAllocator());
    CHECK(f4->ChildCount() == 4u);
    f4->SetValue(Float4{1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(f4->Value().w == doctest::Approx(4.0f));
    f4->SetDecimalPlaces(2);
    CHECK(f4->DecimalPlaces() == 2);
}

TEST_CASE("toolkit-vectorfields: QuaternionEulerRoundTrip")
{
    auto q = core::MakeRef<QuaternionField>(core::DefaultAllocator());
    CHECK(q->ChildCount() == 3u);
    // Identity quaternion -> zero Euler.
    q->SetValue(Quaternion::Identity);
    const Quaternion v = q->Value();
    CHECK(v.w == doctest::Approx(1.0f));
    CHECK(v.x == doctest::Approx(0.0f));
}

TEST_CASE("toolkit-vectorfields: QuaternionEulerCompoundRotationsReadBackExactly")
{
    // The forward composition is Z, then Y, then X, so the middle axis (Y) comes from an
    // arcsine and X/Z from arctangents. The arcsine used to sit on X: single-axis rotations
    // survived and 30/45/60 read back as -16.3/50.4/39.6 (found by the Beef port).
    const Float3 triples[] = {Float3{30, 45, 60}, Float3{-20, 10, 5}, Float3{90, 0, 0},
                              Float3{0, 60, 0}, Float3{0, 0, -120}, Float3{170, -30, 45}};
    for (const Float3& e : triples)
    {
        const Quaternion q = QuaternionField::EulerDegreesToQuaternion(e);
        const Float3 back = QuaternionField::QuaternionToEulerDegrees(q);
        CHECK(back.x == doctest::Approx(e.x).epsilon(0.001));
        CHECK(back.y == doctest::Approx(e.y).epsilon(0.001));
        CHECK(back.z == doctest::Approx(e.z).epsilon(0.001));
        // And the decomposition rebuilds the SAME rotation (the field caches the Eulers and
        // rebuilds from them on the first keystroke).
        const Quaternion again = QuaternionField::EulerDegreesToQuaternion(back);
        CHECK(Abs(again.x * q.x + again.y * q.y + again.z * q.z + again.w * q.w) == doctest::Approx(1.0f).epsilon(0.0001));
    }
    // Through the control: the three fields display the compound angles.
    auto field = core::MakeRef<QuaternionField>(core::DefaultAllocator());
    field->SetValue(QuaternionField::EulerDegreesToQuaternion(Float3{30, 45, 60}));
    REQUIRE(field->ChildCount() == 3u);
    CHECK(Cast<NumericField>(field->GetChildAt(0))->Value() == doctest::Approx(30).epsilon(0.001));
    CHECK(Cast<NumericField>(field->GetChildAt(1))->Value() == doctest::Approx(45).epsilon(0.001));
    CHECK(Cast<NumericField>(field->GetChildAt(2))->Value() == doctest::Approx(60).epsilon(0.001));
}
