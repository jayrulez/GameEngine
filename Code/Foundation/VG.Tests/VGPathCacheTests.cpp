// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PathCache: reuse, retessellation on style changes, invalidation and eviction.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;

using namespace foundation::core;
using namespace foundation::vg;

namespace
{
    Path MakePath()
    {
        PathBuilder b;
        b.MoveTo(0, 0);
        b.LineTo(10, 0);
        b.LineTo(10, 10);
        b.Close();
        return b.ToPath();
    }
}

TEST_CASE("pathcache: same path returns cached")
{
    PathCache cache;
    const Path path = MakePath();

    Array<VGVertex> verts1;
    Array<u32> idx1;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts1, idx1);
    const usize count1 = verts1.Size();

    Array<VGVertex> verts2;
    Array<u32> idx2;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts2, idx2);

    CHECK(verts2.Size() == count1);
    CHECK(idx2.Size() == idx1.Size());
}

TEST_CASE("pathcache: different style retessellates with same geometry count")
{
    PathCache cache;
    const Path path = MakePath();

    Array<VGVertex> verts1;
    Array<u32> idx1;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts1, idx1);

    Array<VGVertex> verts2;
    Array<u32> idx2;
    cache.GetOrTessellateFill(path, Color::Blue, FillRule::EvenOdd, false, verts2, idx2);

    CHECK(verts2.Size() == verts1.Size());
}

TEST_CASE("pathcache: invalidate then re-tessellate")
{
    PathCache cache;
    const Path path = MakePath();

    Array<VGVertex> verts1;
    Array<u32> idx1;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts1, idx1);

    cache.Invalidate(path);

    Array<VGVertex> verts2;
    Array<u32> idx2;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts2, idx2);
    CHECK(verts2.Size() > 0u);
}

TEST_CASE("pathcache: clear removes all")
{
    PathCache cache;
    const Path path = MakePath();

    Array<VGVertex> verts;
    Array<u32> idx;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts, idx);

    cache.Clear();

    Array<VGVertex> verts2;
    Array<u32> idx2;
    cache.GetOrTessellateFill(path, Color::Red, FillRule::EvenOdd, false, verts2, idx2);
    CHECK(verts2.Size() > 0u);
}

TEST_CASE("pathcache: a path has an instance id - a copy is a new instance, a move keeps it")
{
    const Path a = MakePath();
    const Path b = MakePath();
    CHECK(a.InstanceId() != 0u);
    CHECK(a.InstanceId() != b.InstanceId());
    const Path copy = a;
    CHECK(copy.InstanceId() != a.InstanceId());
    const u64 id = b.InstanceId();
    Path moved = MakePath();
    moved = Move(const_cast<Path&>(b));
    CHECK(moved.InstanceId() == id);
    CHECK(b.InstanceId() != id); // the moved-from path is a fresh instance, never a twin
}

TEST_CASE("pathcache: entries key on the instance id, never the address")
{
    // Two paths with identical geometry are two entries; a copy of a cached path is a third.
    // (A pointer key would let a freed path's address serve its dead mesh to a new path.)
    PathCache cache;
    const Path a = MakePath();
    const Path b = MakePath();
    Array<VGVertex> verts;
    Array<u32> idx;
    cache.GetOrTessellateFill(a, Color::Red, FillRule::EvenOdd, false, verts, idx);
    cache.GetOrTessellateFill(a, Color::Red, FillRule::EvenOdd, false, verts, idx);
    CHECK(cache.Count() == 1u);
    cache.GetOrTessellateFill(b, Color::Red, FillRule::EvenOdd, false, verts, idx);
    CHECK(cache.Count() == 2u);
    const Path copy = a;
    cache.GetOrTessellateFill(copy, Color::Red, FillRule::EvenOdd, false, verts, idx);
    CHECK(cache.Count() == 3u);
}

TEST_CASE("pathcache: every stroke input is part of the match - dash pattern, offset, miter, tolerance")
{
    PathCache cache;
    const Path path = MakePath();
    const f32 dash[2] = {2.0f, 2.0f};
    Array<VGVertex> solid;
    Array<u32> solidIdx;
    cache.GetOrTessellateStroke(path, Color::White, StrokeStyle(1.0f), Span<const f32>{}, false,
                                solid, solidIdx);
    // A dash pattern on the same path must retessellate (more, shorter segments), not be served
    // the solid stroke.
    Array<VGVertex> dashed;
    Array<u32> dashedIdx;
    cache.GetOrTessellateStroke(path, Color::White, StrokeStyle(1.0f), Span<const f32>(dash, 2),
                                false, dashed, dashedIdx);
    CHECK(dashed.Size() != solid.Size());
    // Shifting the dash offset moves the dashes: the vertex set is not the one cached for
    // offset 0 (the first dash still starts at the path's first point, so compare them all).
    StrokeStyle shifted(1.0f);
    shifted.dashOffset = 1.0f;
    Array<VGVertex> offsetVerts;
    Array<u32> offsetIdx;
    cache.GetOrTessellateStroke(path, Color::White, shifted, Span<const f32>(dash, 2), false,
                                offsetVerts, offsetIdx);
    REQUIRE(!offsetVerts.IsEmpty());
    bool differs = offsetVerts.Size() != dashed.Size();
    for (usize i = 0; !differs && i < offsetVerts.Size(); ++i)
    {
        differs = offsetVerts[i].position.x != dashed[i].position.x ||
                  offsetVerts[i].position.y != dashed[i].position.y;
    }
    CHECK(differs);
    // The matcher itself: miter limit and tolerance are inputs too.
    CachedPath probe;
    Array<VGVertex> none;
    Array<u32> noneIdx;
    probe.SetStrokeData(none, noneIdx, Span<const f32>{}, 0.25f, Color::White,
                        StrokeStyle(1.0f, VGLineCap::Butt, VGLineJoin::Miter, 4.0f), false);
    CHECK(probe.StrokeMatches(Color::White, StrokeStyle(1.0f, VGLineCap::Butt, VGLineJoin::Miter, 4.0f),
                              Span<const f32>{}, false, 0.25f));
    CHECK_FALSE(probe.StrokeMatches(Color::White,
                                    StrokeStyle(1.0f, VGLineCap::Butt, VGLineJoin::Miter, 10.0f),
                                    Span<const f32>{}, false, 0.25f));
    CHECK_FALSE(probe.StrokeMatches(Color::White, StrokeStyle(1.0f, VGLineCap::Butt, VGLineJoin::Miter, 4.0f),
                                    Span<const f32>{}, false, 0.5f));
    CHECK_FALSE(probe.StrokeMatches(Color::White, StrokeStyle(1.0f, VGLineCap::Butt, VGLineJoin::Miter, 4.0f),
                                    Span<const f32>(dash, 2), false, 0.25f));
}

TEST_CASE("pathcache: a full cache settles at its capacity; SetCapacity trims to exactly that")
{
    PathCache cache(3);
    Array<VGVertex> verts;
    Array<u32> idx;
    Path paths[5] = {MakePath(), MakePath(), MakePath(), MakePath(), MakePath()};
    for (const Path& p : paths)
    {
        cache.GetOrTessellateFill(p, Color::Red, FillRule::EvenOdd, false, verts, idx);
    }
    CHECK(cache.Count() == 3u);
    cache.SetCapacity(2);
    CHECK(cache.Count() == 2u);
    cache.SetCapacity(8);
    CHECK(cache.Count() == 2u); // raising the cap evicts nothing
}

