// VG batch/cache/clip: VGBatch bookkeeping, PathCache reuse + LRU, ClipPathManager.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

namespace
{
    Path Square()
    {
        PathBuilder b;
        b.MoveTo(0, 0);
        b.LineTo(10, 0);
        b.LineTo(10, 10);
        b.LineTo(0, 10);
        b.Close();
        return b.ToPath();
    }
}

TEST_CASE("vg.batch: command/texture bookkeeping")
{
    VGBatch batch;
    CHECK(batch.IsEmpty());

    batch.vertices.PushBack(VGVertex::Solid(Float2{0, 0}, Color::Red));
    batch.indices.PushBack(0);
    VGCommand cmd;
    cmd.indexCount = 1;
    cmd.textureIndex = -1;
    batch.commands.PushBack(cmd);

    CHECK_FALSE(batch.IsEmpty());
    CHECK(batch.CommandCount() == 1u);
    CHECK(batch.GetTextureForCommand(0) == nullptr); // -1 => no texture

    batch.Clear();
    CHECK(batch.VertexCount() == 0u);
    CHECK(batch.IndexCount() == 0u);
}

TEST_CASE("vg.cache: fill is tessellated once then reused")
{
    const Path square = Square();
    PathCache cache;

    Array<VGVertex> v1;
    Array<u32> i1;
    cache.GetOrTessellateFill(square, Color::Red, FillRule::NonZero, false, v1, i1);
    REQUIRE(v1.Size() == 4u);
    REQUIRE(i1.Size() == 6u);

    // Same path+style: identical mesh appended again (served from cache).
    Array<VGVertex> v2;
    Array<u32> i2;
    cache.GetOrTessellateFill(square, Color::Red, FillRule::NonZero, false, v2, i2);
    CHECK(v2.Size() == 4u);
    CHECK(i2.Size() == 6u);
}

TEST_CASE("vg.cache: appended indices are offset by prior vertex count")
{
    const Path square = Square();
    PathCache cache;

    Array<VGVertex> verts;
    Array<u32> idx;
    cache.GetOrTessellateFill(square, Color::Green, FillRule::NonZero, false, verts, idx);
    cache.GetOrTessellateFill(square, Color::Green, FillRule::NonZero, false, verts, idx);

    CHECK(verts.Size() == 8u); // two copies
    CHECK(idx.Size() == 12u);
    // Second batch's indices must reference the second vertex block (>= 4).
    bool sawOffset = false;
    for (usize k = 6; k < idx.Size(); ++k)
        if (idx[k] >= 4u)
            sawOffset = true;
    CHECK(sawOffset);
}

TEST_CASE("vg.cache: stroke tessellation through the cache")
{
    const Path square = Square();
    PathCache cache;
    StrokeStyle style(2.0f);

    Array<VGVertex> verts;
    Array<u32> idx;
    cache.GetOrTessellateStroke(square, Color::Blue, style, Span<const f32>{}, false, verts, idx);
    CHECK(verts.Size() > 0u);
    CHECK(idx.Size() > 0u);
}

TEST_CASE("vg.clip: push emits stencil-write geometry and a stencil command")
{
    const Path square = Square();
    VGBatch batch;
    ClipPathManager clip;

    clip.PushClipPath(square, FillRule::NonZero, batch);
    CHECK(clip.Depth() == 1u);
    CHECK(clip.CurrentStencilRef() == 1);
    REQUIRE(batch.CommandCount() == 1u);
    CHECK(batch.GetCommand(0).clipMode == VGClipMode::Stencil);
    CHECK(batch.GetCommand(0).indexCount == 6); // square fill, no AA

    clip.PopClip();
    CHECK(clip.Depth() == 0u);
    CHECK(clip.CurrentStencilRef() == 0);
}
