// Ported from Sedulous.UI.Tests/src/ViewTransformTests.bf (faithful; Vector2 -> foundation::Float2).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
namespace foundation = draconic::foundation;

TEST_CASE("view-transform: Default_IsIdentity")
{
    ViewTransform t;
    CHECK(t.IsIdentity());
}

TEST_CASE("view-transform: WithTranslation_NotIdentity")
{
    ViewTransform t;
    t.Translation = foundation::Float2{10.0f, 20.0f};
    CHECK_FALSE(t.IsIdentity());
}

TEST_CASE("view-transform: WithRotation_NotIdentity")
{
    ViewTransform t;
    t.Rotation = 0.5f;
    CHECK_FALSE(t.IsIdentity());
}

TEST_CASE("view-transform: WithScale_NotIdentity")
{
    ViewTransform t;
    t.Scale = foundation::Float2{2.0f, 2.0f};
    CHECK_FALSE(t.IsIdentity());
}

TEST_CASE("view-transform: Identity_DefaultOriginIsCenter")
{
    ViewTransform t;
    CHECK(t.Origin.x == 0.5f);
    CHECK(t.Origin.y == 0.5f);
}

TEST_CASE("view-transform: Identity_Constant") { CHECK(ViewTransform::Identity.IsIdentity()); }
