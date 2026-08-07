// Ported from Sedulous.VG.Tests/PathCacheTests.bf.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

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
