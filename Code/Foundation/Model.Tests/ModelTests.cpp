// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.model;

using namespace foundation::core;
using namespace foundation::model;

TEST_CASE("model: core data types - names round-trip as wide strings")
{
    ModelMaterial mat;
    mat.setName(u8"steel");
    CHECK(mat.name() == u8"steel");

    Model model;
    CHECK(model.meshes().Size() == 0u);
}

TEST_CASE("model: ModelBone::updateLocalTransform builds the engine's row-vector TRS matrix")
{
    // TransformPoint reads the translation from the last row. A matrix with the translation in
    // the last column, composed T * (R * S) (column-vector convention), would send (1,0,0) to
    // (2,0,0), dropping the translation entirely.
    ModelBone bone;
    bone.translation = Float3{10.0f, 0.0f, 0.0f};
    bone.scale = Float3{2.0f, 2.0f, 2.0f};
    bone.rotation = Quaternion::Identity;
    bone.updateLocalTransform();
    const Float3 p = TransformPoint(Float3{1.0f, 0.0f, 0.0f}, bone.localTransform);
    CHECK(p.x == doctest::Approx(12.0f));
    CHECK(p.y == doctest::Approx(0.0f));
    CHECK(p.z == doctest::Approx(0.0f));

    // Non-uniform scale and a real rotation, checked without Float4x4 composition: under
    // row-vector semantics p * (S * R) is "scale, then rotate", so the matrix must agree with
    // rotating the pre-scaled point by the quaternion directly, then translating. With
    // non-uniform scale the other order (rotate, then scale) gives a different point, so this
    // discriminates S * R from R * S rather than comparing the builder to itself.
    bone.translation = Float3{1.0f, -2.0f, 3.0f};
    bone.scale = Float3{2.0f, 3.0f, 4.0f};
    bone.rotation = FromYawPitchRoll(0.7f, -0.3f, 1.1f);
    bone.updateLocalTransform();
    const Float3 q{0.5f, -1.0f, 2.0f};
    const Float3 scaleThenRotate = RotateVector(bone.rotation, q * bone.scale) + bone.translation;
    const Float3 rotateThenScale = RotateVector(bone.rotation, q) * bone.scale + bone.translation;
    const Float3 actual = TransformPoint(q, bone.localTransform);
    CHECK(actual.x == doctest::Approx(scaleThenRotate.x));
    CHECK(actual.y == doctest::Approx(scaleThenRotate.y));
    CHECK(actual.z == doctest::Approx(scaleThenRotate.z));
    // The case is discriminating: the wrong order lands somewhere else.
    CHECK(Length(rotateThenScale - scaleThenRotate) > 0.1f);
}

