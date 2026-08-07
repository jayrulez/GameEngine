#include <doctest/doctest.h>

import draconic.foundation;

using namespace draconic::foundation;

TEST_CASE("random: deterministic and seed-dependent")
{
    Random a(12345);
    Random b(12345);
    Random c(99999);

    bool sameAB = true;
    bool differsC = false;
    for (int i = 0; i < 16; ++i)
    {
        const u32 va = a.NextU32();
        if (va != b.NextU32())
        {
            sameAB = false;
        }
        if (va != c.NextU32())
        {
            differsC = true;
        }
    }
    CHECK(sameAB);   // same seed -> identical stream
    CHECK(differsC); // different seed -> diverges
}

TEST_CASE("random: ranges are respected")
{
    Random rng(2024);
    for (int i = 0; i < 1000; ++i)
    {
        const f32 f = rng.NextFloat();
        CHECK(f >= 0.0f);
        CHECK(f < 1.0f);

        const f32 r = rng.NextFloat(-2.0f, 5.0f);
        CHECK(r >= -2.0f);
        CHECK(r <= 5.0f);

        const i32 n = rng.NextInt(10, 20);
        CHECK(n >= 10);
        CHECK(n <= 20);
    }
}
