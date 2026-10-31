// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CollisionShapeAsset's and PhysicalMaterialAsset's reflected surface, and the
// CollisionCookKind enum. Verifies authored properties enumerate with attributes and round-trip
// through get and set, and that the cook-mode enum resolves named values.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import physics.pipeline;

using namespace foundation::core;
using namespace pipeline;

namespace
{
    bool CEq(const char* a, const char* b)
    {
        if (a == nullptr || b == nullptr)
        {
            return a == b;
        }
        while (*a != '\0' && *b != '\0')
        {
            if (*a != *b)
            {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }
}

TEST_CASE("reflection-p1: PhysicalMaterialAsset properties round-trip with ranges")
{
    pipeline::RegisterPhysicsAssets();
    const TypeInfo& type = pipeline::PhysicalMaterialAsset::StaticType();

    CHECK(CEq(type.name, "PhysicalMaterialAsset"));
    CHECK(PropertyCount(type) == 3u);

    const PropertyInfo* friction = FindProperty(type, "friction");
    const PropertyInfo* restitution = FindProperty(type, "restitution");
    const PropertyInfo* density = FindProperty(type, "density");
    REQUIRE(friction != nullptr);
    REQUIRE(restitution != nullptr);
    REQUIRE(density != nullptr);
    CHECK(FindAttribute(*friction, u8"range") != nullptr);

    pipeline::PhysicalMaterialAsset mat;
    Instance inst = Instance::From(&mat);
    CHECK(SetProperty(*friction, inst, Variant::From(0.8f)).IsOk());
    CHECK(GetProperty(*friction, inst).Get<f32>() == doctest::Approx(0.8f));
    CHECK(mat.friction == doctest::Approx(0.8f));
    CHECK(SetProperty(*density, inst, Variant::From(500.0f)).IsOk());
    CHECK(mat.density == doctest::Approx(500.0f));
}

TEST_CASE("reflection-p1: CollisionShapeAsset exposes the cook enum + conditional field")
{
    pipeline::RegisterPhysicsAssets();
    const TypeInfo& type = pipeline::CollisionShapeAsset::StaticType();

    CHECK(PropertyCount(type) == 3u);
    const PropertyInfo* cook = FindProperty(type, "cook");
    REQUIRE(cook != nullptr);
    REQUIRE(cook->type != nullptr);
    CHECK(IsEnum(*cook->type));
    CHECK(CEq(EnumValueName(*cook->type,
                            static_cast<i64>(pipeline::CollisionCookKind::TriangleMesh)),
              "TriangleMesh"));

    // hullTolerance is convex-hull only (visibleWhen the generic page evaluates).
    const PropertyInfo* tol = FindProperty(type, "hullTolerance");
    REQUIRE(tol != nullptr);
    const Attribute* vis = FindAttribute(*tol, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    CHECK(vis->value.Get<String>().AsView() == StringView(u8"cook=0"));
}

TEST_CASE("reflection-p1: CollisionCookKind enum reflects both values")
{
    pipeline::RegisterPhysicsAssets();
    const TypeInfo& kind = TypeOf<pipeline::CollisionCookKind>();
    CHECK(IsEnum(kind));
    CHECK(EnumeratorCount(kind) == 2u);
    CHECK(CEq(EnumValueName(kind, 0), "ConvexHull"));
    CHECK(CEq(EnumValueName(kind, 1), "TriangleMesh"));
}
