// Ported from Sedulous.VG.Tests/DashGeneratorTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("dash: simple pattern correct segment count")
{
    Float2 points[2] = {{0, 0}, {20, 0}};
    f32 pattern[2] = {5, 5};
    Array<Array<Float2>> output;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false, Span<const f32>(pattern, 2),
                                  0, output);
    CHECK(output.Size() == 2u);
}

TEST_CASE("dash: offset shifts pattern")
{
    Float2 points[2] = {{0, 0}, {20, 0}};
    f32 pattern[2] = {5, 5};

    Array<Array<Float2>> output1;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false, Span<const f32>(pattern, 2),
                                  0, output1);

    Array<Array<Float2>> output2;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 2), false, Span<const f32>(pattern, 2),
                                  5.0f, output2);

    CHECK(output2.Size() > 0u);
}

TEST_CASE("dash: closed path wraps")
{
    Float2 points[3] = {{0, 0}, {10, 0}, {5, 10}};
    f32 pattern[2] = {3, 3};
    Array<Array<Float2>> output;
    DashGenerator::GenerateDashes(Span<const Float2>(points, 3), true, Span<const f32>(pattern, 2),
                                  0, output);
    CHECK(output.Size() >= 2u);
}
