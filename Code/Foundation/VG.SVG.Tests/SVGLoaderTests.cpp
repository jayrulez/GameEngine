// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Exercise SVGLoader and SVGRenderer end to end on small documents.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import foundation.vg.svg;

using namespace foundation::core;
using namespace foundation::vg;
using namespace foundation::vg::svg;

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

    const foundation::vg::svg::SVGGradient* lin = doc.gradients.Find(String(u8"lin"));
    REQUIRE(lin != nullptr);
    CHECK(!lin->radial);
    CHECK(!lin->userSpace);
    CHECK(lin->x2 == doctest::Approx(1.0f)); // "100%" is 1.0
    CHECK(lin->spread == foundation::vg::VGGradientSpread::Reflect);
    REQUIRE(lin->stops.Size() == 2u);
    CHECK(lin->stops[0].color.r == doctest::Approx(1.0f));
    CHECK(lin->stops[1].color.b == doctest::Approx(1.0f)); // style= form parsed
    CHECK(lin->stops[1].color.a == doctest::Approx(0.5f)); // stop-opacity applied

    const foundation::vg::svg::SVGGradient* rad = doc.gradients.Find(String(u8"rad"));
    REQUIRE(rad != nullptr);
    CHECK(rad->radial);
    CHECK(rad->userSpace);
    CHECK(rad->r == doctest::Approx(0.25f));
    CHECK(rad->spread == foundation::vg::VGGradientSpread::Repeat);
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
          foundation::vg::VGGradientSpread::Repeat);

    // A missing reference falls back to the fallback fill color (no crash, no LUT).
    const StringView broken =
        u8"<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\""
        u8" fill=\"url(#nope)\"/></svg>";
    const Result<SVGDocument> rb = SVGLoader::Load(broken);
    REQUIRE(rb.HasValue());
    VGContext ctx2;
    SVGRenderer::Render(ctx2, rb.Value(), Rectangle{0, 0, 10, 10});
    CHECK(ctx2.GetBatch().textures.Size() == 1u); // white only: solid fallback
    CHECK(ctx2.GetBatch().VertexCount() > 0u);
}

TEST_CASE("svg.loader: an unrecognised container is skipped WHOLE - the siblings after it survive")
{
    // Editor exports lead with <metadata>, <desc>, <style> and comments. Skipping only the
    // opening tag would parse the container's children at the current level and let its closing
    // tag end that level, dropping every shape after it. Nested unknowns, a comment holding
    // '>' and a style block with a CSS child combinator must all be stepped over as units.
    const StringView svg =
        u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\">"
        u8"<metadata><rdf:RDF><cc:Work><dc:title>x</dc:title></cc:Work></rdf:RDF></metadata>"
        u8"<desc>a description</desc>"
        u8"<!-- a comment with a > in it and a <fake> tag -->"
        u8"<style>rect > circle { fill: red; }</style>"
        u8"<unknown attr=\"a > b\"><child/><child><grandchild/></child></unknown>"
        u8"<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\"/>"
        u8"<circle cx=\"5\" cy=\"5\" r=\"2\"/>"
        u8"<g><rect x=\"1\" y=\"1\" width=\"1\" height=\"1\"/></g>"
        u8"</svg>";
    const Result<SVGDocument> loaded = SVGLoader::Load(svg);
    REQUIRE(loaded.HasValue());
    const SVGDocument& doc = loaded.Value();
    // rect, circle, g (with its own rect), and nothing from the skipped containers.
    REQUIRE(doc.elements.Size() == 3u);
    CHECK(doc.elements[0].type == SVGElementType::Rectangle);
    CHECK(doc.elements[1].type == SVGElementType::Circle);
    CHECK(doc.elements[2].type == SVGElementType::Group);
    REQUIRE(doc.elements[2].children.Size() == 1u);
    CHECK(doc.elements[2].children[0].type == SVGElementType::Rectangle);
}


TEST_CASE("svg.loader: paint is inherited from the svg element and groups")
{
    // An icon written once on its root: every shape strokes white, with no fill, round ends.
    const StringView svg =
        u8"<svg viewBox=\"0 0 16 16\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1.5\" "
        u8"stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        u8"  <path d=\"M3 8h10\"/>"
        u8"  <g stroke=\"#ff0000\" stroke-linecap=\"square\">"
        u8"    <circle cx=\"8\" cy=\"8\" r=\"4\"/>"
        u8"    <rect x=\"1\" y=\"1\" width=\"2\" height=\"2\" fill=\"blue\" stroke-linejoin=\"bevel\"/>"
        u8"  </g>"
        u8"</svg>";

    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());
    const SVGDocument& doc = r.Value();
    REQUIRE(doc.elements.Size() == 2u);

    const SVGElement& line = doc.elements[0];
    CHECK_FALSE(line.fillColor.HasValue());
    REQUIRE(line.strokeColor.HasValue());
    CHECK(line.strokeColor.Value() == Color::White);
    CHECK(line.strokeWidth == doctest::Approx(1.5f));
    CHECK(line.strokeCap == VGLineCap::Round);
    CHECK(line.strokeJoin == VGLineJoin::Round);

    REQUIRE(doc.elements[1].children.Size() == 2u);
    const SVGElement& circle = doc.elements[1].children[0];
    CHECK_FALSE(circle.fillColor.HasValue());
    REQUIRE(circle.strokeColor.HasValue());
    CHECK(circle.strokeColor.Value() == Color::Red); // the group's
    CHECK(circle.strokeWidth == doctest::Approx(1.5f)); // the root's, through the group
    CHECK(circle.strokeCap == VGLineCap::Square);
    CHECK(circle.strokeJoin == VGLineJoin::Round);

    const SVGElement& box = doc.elements[1].children[1];
    REQUIRE(box.fillColor.HasValue());
    CHECK(box.fillColor.Value() == Color::Blue); // its own
    CHECK(box.strokeJoin == VGLineJoin::Bevel);
}

TEST_CASE("svg.loader: a shape with no paint of its own keeps SVG's defaults")
{
    const StringView svg = u8"<svg viewBox=\"0 0 8 8\"><path d=\"M0 0h8v8z\"/></svg>";
    const Result<SVGDocument> r = SVGLoader::Load(svg);
    REQUIRE(r.HasValue());
    const SVGElement& shape = r.Value().elements[0];
    REQUIRE(shape.fillColor.HasValue());
    CHECK(shape.fillColor.Value() == Color::Black);
    CHECK_FALSE(shape.strokeColor.HasValue());
    CHECK(shape.strokeCap == VGLineCap::Butt);
    CHECK(shape.strokeJoin == VGLineJoin::Miter);
}
