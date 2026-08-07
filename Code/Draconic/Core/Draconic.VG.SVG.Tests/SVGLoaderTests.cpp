// Extra coverage (not from Sedulous.VG.Tests): exercise SVGLoader + SVGRenderer
// end-to-end on a small document.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.vg.svg;

using namespace draconic::foundation;
using namespace draconic::vg;
using namespace draconic::vg::svg;

TEST_CASE("svg.loader: parses viewBox + shapes")
{
    const StringView svg =
        u8"<svg viewBox=\"0 0 100 100\">"
        u8"  <rect x=\"10\" y=\"10\" width=\"30\" height=\"30\" fill=\"#ff0000\"/>"
        u8"  <circle cx=\"50\" cy=\"50\" r=\"20\" fill=\"blue\" stroke=\"black\" "
        u8"stroke-width=\"2\"/>"
        u8"</svg>";

    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());
    const SVGDocument& doc = r.Value();
    CHECK(doc.width == doctest::Approx(100.0f));
    CHECK(doc.height == doctest::Approx(100.0f));
    REQUIRE(doc.elements.Size() == 2u);

    CHECK(doc.elements[0].type == SVGElementType::Rectangle);
    REQUIRE(doc.elements[0].fillColor.HasValue());
    CHECK(doc.elements[0].fillColor.Value() == Color::Red);
    CHECK(doc.elements[0].path.HasValue());

    CHECK(doc.elements[1].type == SVGElementType::Circle);
    REQUIRE(doc.elements[1].strokeColor.HasValue());
    CHECK(doc.elements[1].strokeColor.Value() == Color::Black);
    CHECK(doc.elements[1].strokeWidth == doctest::Approx(2.0f));
}

TEST_CASE("svg.loader: nested group with transform")
{
    const StringView svg = u8"<svg width=\"64\" height=\"64\">"
                           u8"  <g transform=\"translate(10,10)\">"
                           u8"    <path d=\"M0 0 L10 0 L10 10 Z\" fill=\"green\"/>"
                           u8"  </g>"
                           u8"</svg>";

    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());
    const SVGDocument& doc = r.Value();
    REQUIRE(doc.elements.Size() == 1u);
    CHECK(doc.elements[0].type == SVGElementType::Group);
    CHECK(doc.elements[0].IsGroup());
    REQUIRE(doc.elements[0].children.Size() == 1u);
    CHECK(doc.elements[0].children[0].type == SVGElementType::Path);
    // translate(10,10) lands in the transform's last row.
    CHECK(doc.elements[0].transform.m[3][0] == doctest::Approx(10.0f));
}

TEST_CASE("svg.renderer: renders a document into a VG batch")
{
    const StringView svg =
        u8"<svg viewBox=\"0 0 100 100\">"
        u8"  <rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#00ff00\"/>"
        u8"</svg>";

    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());

    VGContext ctx;
    SVGRenderer::Render(ctx, r.Value(), Rectangle{0, 0, 200, 200});

    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.VertexCount() > 0u);
    CHECK(batch.IndexCount() > 0u);
}

TEST_CASE("svg.renderer: tint overrides fill")
{
    const StringView svg =
        u8"<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"red\"/></svg>";
    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());

    VGContext ctx;
    SVGRenderer::Render(ctx, r.Value(), Rectangle{0, 0, 10, 10}, Optional<Color>(Color::Blue));
    CHECK(ctx.GetBatch().VertexCount() > 0u);
}

TEST_CASE("svg.loader: parses defs gradients (attrs, style stops, spreadMethod, url fill)")
{
    const StringView svg =
        u8"<svg viewBox=\"0 0 100 100\">"
        u8"  <defs>"
        u8"    <linearGradient id=\"lin\" x1=\"0%\" y1=\"0%\" x2=\"100%\" y2=\"0%\""
        u8"                    spreadMethod=\"reflect\">"
        u8"      <stop offset=\"0%\" stop-color=\"#ff0000\"/>"
        u8"      <stop offset=\"100%\" style=\"stop-color:#0000ff;stop-opacity:0.5\"/>"
        u8"    </linearGradient>"
        u8"    <radialGradient id=\"rad\" cx=\"0.5\" cy=\"0.5\" r=\"0.25\""
        u8"                    gradientUnits=\"userSpaceOnUse\" spreadMethod=\"repeat\">"
        u8"      <stop offset=\"0\" stop-color=\"white\"/>"
        u8"      <stop offset=\"1\" stop-color=\"black\"/>"
        u8"    </radialGradient>"
        u8"  </defs>"
        u8"  <rect width=\"100\" height=\"100\" fill=\"url(#lin)\"/>"
        u8"</svg>";
    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());
    const SVGDocument& doc = r.Value();

    // defs content never reaches the element tree; only the rect renders.
    REQUIRE(doc.elements.Size() == 1u);
    CHECK(doc.elements[0].fillGradientId.AsView() == u8"lin");

    const draconic::vg::svg::SVGGradient* lin = doc.gradients.Find(String(u8"lin"));
    REQUIRE(lin != nullptr);
    CHECK(!lin->radial);
    CHECK(!lin->userSpace);
    CHECK(lin->x2 == doctest::Approx(1.0f)); // "100%" -> 1.0
    CHECK(lin->spread == draconic::vg::VGGradientSpread::Reflect);
    REQUIRE(lin->stops.Size() == 2u);
    CHECK(lin->stops[0].color.r == doctest::Approx(1.0f));
    CHECK(lin->stops[1].color.b == doctest::Approx(1.0f)); // style= form parsed
    CHECK(lin->stops[1].color.a == doctest::Approx(0.5f)); // stop-opacity applied

    const draconic::vg::svg::SVGGradient* rad = doc.gradients.Find(String(u8"rad"));
    REQUIRE(rad != nullptr);
    CHECK(rad->radial);
    CHECK(rad->userSpace);
    CHECK(rad->r == doctest::Approx(0.25f));
    CHECK(rad->spread == draconic::vg::VGGradientSpread::Repeat);
}

TEST_CASE("svg.renderer: a gradient fill bakes a LUT and spreads ride the command")
{
    const StringView svg =
        u8"<svg viewBox=\"0 0 10 10\">"
        u8"  <linearGradient id=\"g\" spreadMethod=\"repeat\">"
        u8"    <stop offset=\"0\" stop-color=\"red\"/>"
        u8"    <stop offset=\"1\" stop-color=\"blue\"/>"
        u8"  </linearGradient>"
        u8"  <rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"
        u8"</svg>";
    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());

    VGContext ctx;
    SVGRenderer::Render(ctx, r.Value(), Rectangle{0, 0, 10, 10});
    VGBatch& batch = ctx.GetBatch();
    CHECK(batch.textures.Size() == 2u); // white + the baked gradient LUT
    REQUIRE(!batch.commands.IsEmpty());
    CHECK(batch.commands[batch.commands.Size() - 1].gradientSpread ==
          draconic::vg::VGGradientSpread::Repeat);

    // A missing reference falls back to the fallback fill color (no crash, no LUT).
    const StringView broken =
        u8"<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\""
        u8" fill=\"url(#nope)\"/></svg>";
    const Result<SVGDocument> rb = SVGLoader::Load(broken);
    REQUIRE(rb.HasValue());
    VGContext ctx2;
    SVGRenderer::Render(ctx2, rb.Value(), Rectangle{0, 0, 10, 10});
    CHECK(ctx2.GetBatch().textures.Size() == 1u); // white only - solid fallback
    CHECK(ctx2.GetBatch().VertexCount() > 0u);
}
