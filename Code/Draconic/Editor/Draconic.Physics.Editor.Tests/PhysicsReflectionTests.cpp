// Reflection track P1: CollisionShapeAsset + PhysicalMaterialAsset reflected surface, and the
// CollisionCookKind enum. Verifies authored properties enumerate with attributes, round-trip
// through get/set, and that the cook-mode enum resolves named values.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.physics.editor;

using namespace draconic::foundation;

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
    draconic::physics::RegisterPhysicsAssets();
    const TypeInfo& type = draconic::physics::PhysicalMaterialAsset::StaticType();

    CHECK(CEq(type.name, "PhysicalMaterialAsset"));
    CHECK(PropertyCount(type) == 3u);

    const PropertyInfo* friction = FindProperty(type, "friction");
    const PropertyInfo* restitution = FindProperty(type, "restitution");
    const PropertyInfo* density = FindProperty(type, "density");
    REQUIRE(friction != nullptr);
    REQUIRE(restitution != nullptr);
    REQUIRE(density != nullptr);
    CHECK(FindAttribute(*friction, u8"range") != nullptr);

    draconic::physics::PhysicalMaterialAsset mat;
    Instance inst = Instance::From(&mat);
    CHECK(SetProperty(*friction, inst, Variant::From(0.8f)).IsOk());
    CHECK(GetProperty(*friction, inst).Get<f32>() == doctest::Approx(0.8f));
    CHECK(mat.friction == doctest::Approx(0.8f));
    CHECK(SetProperty(*density, inst, Variant::From(500.0f)).IsOk());
    CHECK(mat.density == doctest::Approx(500.0f));
}

TEST_CASE("reflection-p1: CollisionShapeAsset exposes the cook enum + conditional field")
{
    draconic::physics::RegisterPhysicsAssets();
    const TypeInfo& type = draconic::physics::CollisionShapeAsset::StaticType();

    CHECK(PropertyCount(type) == 3u);
    const PropertyInfo* cook = FindProperty(type, "cook");
    REQUIRE(cook != nullptr);
    REQUIRE(cook->type != nullptr);
    CHECK(IsEnum(*cook->type));
    CHECK(CEq(EnumValueName(*cook->type,
                            static_cast<i64>(draconic::physics::CollisionCookKind::TriangleMesh)),
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
    draconic::physics::RegisterPhysicsAssets();
    const TypeInfo& kind = TypeOf<draconic::physics::CollisionCookKind>();
    CHECK(IsEnum(kind));
    CHECK(EnumeratorCount(kind) == 2u);
    CHECK(CEq(EnumValueName(kind, 0), "ConvexHull"));
    CHECK(CEq(EnumValueName(kind, 1), "TriangleMesh"));
}
