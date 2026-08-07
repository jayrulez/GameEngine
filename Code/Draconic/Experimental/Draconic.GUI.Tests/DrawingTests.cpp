// Draconic GUI - Drawing/render-seam tests. Metadata + behavior (drawing needs no GPU:
// a VGContext tessellates into a CPU vertex batch, so we can assert geometry is produced).
// StateList fallback semantics mirror the eepp/draconic.ui pattern.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

// === Drawable color/alpha state ===

TEST_CASE("drawing: Drawable color and alpha")
{
    RectangleDrawable r{foundation::Color::Red};
    CHECK(r.GetColor().r == doctest::Approx(1.0f));
    r.SetColor(foundation::Color::Blue);
    CHECK(r.GetColor().b == doctest::Approx(1.0f));
    r.SetAlpha(0.5f);
    CHECK(r.GetAlpha() == doctest::Approx(0.5f));
    CHECK(r.GetColor().a == doctest::Approx(0.5f));
}

TEST_CASE("drawing: RectangleDrawable corner radii")
{
    RectangleDrawable r;
    CHECK(r.GetCornerRadii().topLeft == 0.0f);
    r.SetCornerRadii(vg::CornerRadii{8.0f});
    CHECK(r.GetCornerRadii().topLeft == doctest::Approx(8.0f));
    CHECK(r.GetCornerRadii().bottomRight == doctest::Approx(8.0f));
}

TEST_CASE("drawing: BorderDrawable width")
{
    BorderDrawable b;
    CHECK(b.GetWidth() == doctest::Approx(1.0f));
    b.SetWidth(3.0f);
    CHECK(b.GetWidth() == doctest::Approx(3.0f));
}

// === StateListDrawable dispatch (eepp/draconic.ui fallback pattern) ===

TEST_CASE("drawing: StateList falls back to Normal")
{
    StateListDrawable sl;
    sl.Set(ControlState::Normal,
           foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Red));
    Drawable* normal = sl.Get(ControlState::Normal);

    CHECK(sl.Get(ControlState::Normal) == normal);
    CHECK(sl.Get(ControlState::Hover) == normal);   // fallback
    CHECK(sl.Get(ControlState::Pressed) == normal); // fallback
    CHECK(sl.IsStateful());
}

TEST_CASE("drawing: StateList returns specific state")
{
    StateListDrawable sl;
    sl.Set(ControlState::Normal,
           foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Red));
    sl.Set(ControlState::Hover,
           foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Blue));
    Drawable* normal = sl.Get(ControlState::Normal);
    Drawable* hover = sl.Get(ControlState::Hover);

    CHECK(normal != hover);
    CHECK(sl.Get(ControlState::Hover) == hover);
    CHECK(sl.Get(ControlState::Pressed) == normal); // fallback
}

TEST_CASE("drawing: StateList empty returns null")
{
    StateListDrawable sl;
    CHECK(sl.Get(ControlState::Normal) == nullptr);
    CHECK(sl.Get(ControlState::Hover) == nullptr);
}

// === Actual VG geometry produced (GPU-free) ===

TEST_CASE("drawing: RectangleDrawable produces VG geometry")
{
    vg::VGContext ctx;
    DrawContext dc{ctx};
    RectangleDrawable r{foundation::Color::Red};
    r.Draw(dc, Rect{0.0f, 0.0f, 100.0f, 50.0f});
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("drawing: rounded RectangleDrawable produces VG geometry")
{
    vg::VGContext ctx;
    DrawContext dc{ctx};
    RectangleDrawable r{foundation::Color::Red};
    r.SetCornerRadii(vg::CornerRadii{10.0f});
    r.Draw(dc, Rect{0.0f, 0.0f, 100.0f, 50.0f});
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("drawing: BorderDrawable produces VG geometry")
{
    vg::VGContext ctx;
    DrawContext dc{ctx};
    BorderDrawable b{foundation::Color{0.0f, 0.0f, 0.0f, 1.0f}, 2.0f};
    b.Draw(dc, Rect{0.0f, 0.0f, 100.0f, 50.0f});
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}

TEST_CASE("drawing: zero-width BorderDrawable produces nothing")
{
    vg::VGContext ctx;
    DrawContext dc{ctx};
    BorderDrawable b;
    b.SetWidth(0.0f);
    b.Draw(dc, Rect{0.0f, 0.0f, 100.0f, 50.0f});
    CHECK(ctx.GetBatch().vertices.Size() == 0);
}

TEST_CASE("drawing: StateList draws via VG")
{
    vg::VGContext ctx;
    DrawContext dc{ctx};
    StateListDrawable sl;
    sl.Set(ControlState::Normal,
           foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Red));
    sl.Draw(dc, Rect{0.0f, 0.0f, 20.0f, 20.0f}, ControlState::Hover); // falls back to Normal
    CHECK(ctx.GetBatch().vertices.Size() > 0);
}
