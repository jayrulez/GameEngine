#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.profiler;

using namespace draconic::foundation;
using namespace draconic::profiler;

TEST_CASE("profiler: a single scope is recorded")
{
    Profiler& p = Profiler::Get();
    p.SetEnabled(true);
    p.BeginFrame();
    {
        ScopedProfile s("Alpha");
    }
    p.EndFrame();

    const ProfileFrame& f = p.CompletedFrame();
    REQUIRE(f.samples.Size() == 1u);
    CHECK(StringView(reinterpret_cast<const char8_t*>(f.samples[0].name)) == StringView(u8"Alpha"));
    CHECK(f.samples[0].depth == 0u);
}

TEST_CASE("profiler: nested scopes get increasing depth")
{
    Profiler& p = Profiler::Get();
    p.SetEnabled(true);
    p.BeginFrame();
    {
        ScopedProfile outer("Outer");
        {
            ScopedProfile inner("Inner");
        }
        {
            ScopedProfile inner2("Inner2");
        }
    }
    p.EndFrame();

    const ProfileFrame& f = p.CompletedFrame();
    REQUIRE(f.samples.Size() == 3u);
    // EndScope records on close, so children appear before the parent; find by name.
    u32 outerDepth = 99, innerDepth = 99;
    for (usize i = 0; i < f.samples.Size(); ++i)
    {
        StringView n(reinterpret_cast<const char8_t*>(f.samples[i].name));
        if (n == StringView(u8"Outer"))
        {
            outerDepth = f.samples[i].depth;
        }
        if (n == StringView(u8"Inner"))
        {
            innerDepth = f.samples[i].depth;
        }
    }
    CHECK(outerDepth == 0u);
    CHECK(innerDepth == 1u);
}

TEST_CASE("profiler: disabled records nothing")
{
    Profiler& p = Profiler::Get();
    p.SetEnabled(true);
    p.BeginFrame();
    p.EndFrame(); // clean baseline: an empty completed frame
    REQUIRE(p.CompletedFrame().samples.Size() == 0u);

    p.SetEnabled(false);
    p.BeginFrame();
    {
        ScopedProfile s("Ignored");
    }
    p.EndFrame();
    p.SetEnabled(true);
    CHECK(p.CompletedFrame().samples.Size() == 0u); // unchanged - disabled did nothing
}

TEST_CASE("profiler: frame number advances and a report builds")
{
    Profiler& p = Profiler::Get();
    p.SetEnabled(true);
    p.BeginFrame();
    {
        ScopedProfile s("Work");
    }
    p.EndFrame();
    const u64 n0 = p.CompletedFrame().frameNumber;

    p.BeginFrame();
    {
        ScopedProfile s("Work");
    }
    p.EndFrame();
    CHECK(p.CompletedFrame().frameNumber == n0 + 1u);

    String report = p.BuildReport();
    CHECK(report.Size() > 0u);
}
