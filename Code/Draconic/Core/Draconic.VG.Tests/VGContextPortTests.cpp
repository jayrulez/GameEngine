// Ported from Sedulous.VG.Tests/VGContextTests.bf. (Sedulous Color32.Yellow is
// inlined; Float4x4 != uses the C++20 rewrite of operator==.)
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;
using namespace draconic::vg;

TEST_CASE("vgcontext: FillRect produces vertices and indices")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{10, 10, 100, 50}, Color::Red);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
    CHECK(batch.IndexCount() > 0u);
    CHECK(batch.CommandCount() > 0u);
}

TEST_CASE("vgcontext: StrokeRect produces output")
{
    VGContext ctx;
    ctx.StrokeRect(Rectangle{10, 10, 100, 50}, Color::Blue, 2.0f);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
    CHECK(batch.IndexCount() > 0u);
}

TEST_CASE("vgcontext: state stack restores transform")
{
    VGContext ctx;
    const Float4x4 identity = ctx.GetTransform();

    ctx.PushState();
    ctx.Translate(100, 200);
    const Float4x4 translated = ctx.GetTransform();
    CHECK(translated != identity);

    ctx.PopState();
    const Float4x4 restored = ctx.GetTransform();
    CHECK(restored == identity);
}

TEST_CASE("vgcontext: clip rect affects commands")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::Red);
    ctx.PushClipRect(Rectangle{0, 0, 50, 50});
    ctx.FillRect(Rectangle{5, 5, 10, 10}, Color::Blue);
    ctx.PopClip();

    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.CommandCount() >= 2u);
}

TEST_CASE("vgcontext: opacity applied to vertex color")
{
    VGContext ctx;
    ctx.PushOpacity(0.5f);
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::White);

    VGBatch& batch = ctx.GetBatch();
    REQUIRE(batch.VertexCount() > 0u);
    for (usize i = 0; i < batch.VertexCount(); ++i)
        CHECK(batch.vertices[i].color.a < 0.8f);
}

TEST_CASE("vgcontext: FillCircle produces output")
{
    VGContext ctx;
    ctx.FillCircle(Float2{50, 50}, 25, Color::Green);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 8u);
    CHECK(batch.IndexCount() > 0u);
}

TEST_CASE("vgcontext: rounded rect per-corner radii")
{
    VGContext ctx;
    ctx.FillRoundedRect(Rectangle{0, 0, 100, 100}, CornerRadii(10, 20, 30, 40), Color::White);
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 4u);
    CHECK(batch.IndexCount() > 6u);
}

TEST_CASE("vgcontext: clear resets everything")
{
    VGContext ctx;
    ctx.FillRect(Rectangle{0, 0, 10, 10}, Color::Red);
    CHECK(ctx.GetBatch().VertexCount() > 0u);

    ctx.Clear();
    CHECK(ctx.GetBatch().VertexCount() == 0u);
}

TEST_CASE("vgcontext: FillStar produces output")
{
    VGContext ctx;
    ctx.FillStar(Float2{50, 50}, 30, 15, 5,
                 ToColor(Color32{255, 255, 0, 255})); // Sedulous Color32.Yellow
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
}
