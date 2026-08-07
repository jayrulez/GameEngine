// Ported from Sedulous.UI.Tests/src/ThicknessTests.bf (faithful; Beef Test.Assert -> doctest CHECK,
// Beef properties -> methods).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;

TEST_CASE("thickness: Default_IsZero")
{
    Thickness t{};
    CHECK(t.IsZero());
    CHECK((t.Left == 0.0f && t.Top == 0.0f && t.Right == 0.0f && t.Bottom == 0.0f));
}

TEST_CASE("thickness: Uniform_AllSidesEqual")
{
    Thickness t{10.0f};
    CHECK((t.Left == 10.0f && t.Top == 10.0f && t.Right == 10.0f && t.Bottom == 10.0f));
    CHECK_FALSE(t.IsZero());
}

TEST_CASE("thickness: HorizontalVertical_SetsPairs")
{
    Thickness t{8.0f, 4.0f};
    CHECK((t.Left == 8.0f && t.Right == 8.0f));
    CHECK((t.Top == 4.0f && t.Bottom == 4.0f));
}

TEST_CASE("thickness: Explicit_SetsEachSide")
{
    Thickness t{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK((t.Left == 1.0f && t.Top == 2.0f && t.Right == 3.0f && t.Bottom == 4.0f));
}

TEST_CASE("thickness: TotalHorizontal")
{
    Thickness t{10.0f, 5.0f, 20.0f, 5.0f};
    CHECK(t.TotalHorizontal() == 30.0f);
}

TEST_CASE("thickness: TotalVertical")
{
    Thickness t{10.0f, 5.0f, 10.0f, 15.0f};
    CHECK(t.TotalVertical() == 20.0f);
}
