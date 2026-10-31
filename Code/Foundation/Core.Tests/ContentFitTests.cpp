// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Core tests: ContentFit, placing content in a region by a FitMode and turning it as it is
// presented. Covers a landscape game fitted into a portrait screen turned a quarter, points that
// round-trip through the turn, deltas turned with it, and the unturned fit.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;

using namespace foundation::core;

namespace
{
    void CheckRect(const Rectangle& r, f32 x, f32 y, f32 w, f32 h)
    {
        CHECK(r.x == doctest::Approx(x));
        CHECK(r.y == doctest::Approx(y));
        CHECK(r.width == doctest::Approx(w));
        CHECK(r.height == doctest::Approx(h));
    }
    void CheckPoint(Float2 p, f32 x, f32 y)
    {
        CHECK(p.x == doctest::Approx(x));
        CHECK(p.y == doctest::Approx(y));
    }
    constexpr ContentRotation kRotations[] = {ContentRotation::None, ContentRotation::Quarter,
                                              ContentRotation::Half, ContentRotation::ThreeQuarter};
    constexpr ContentRotation kQuarterTurns[] = {ContentRotation::Quarter, ContentRotation::ThreeQuarter};
    constexpr FitMode kModes[] = {FitMode::Stretch, FitMode::Letterbox, FitMode::Crop, FitMode::IntegerScale};
}

TEST_CASE("content fit: unturned, as it always was")
{
    // 320x180 letterboxed into a 1280x1080 region at (100, 50): scale 4, bars of 180 above and
    // below.
    ContentFit fit{Rectangle{100, 50, 1280, 1080}, Float2{320, 180}, FitMode::Letterbox};
    CheckRect(fit.DstRect(), 100, 50 + 180, 1280, 720);
    CheckRect(fit.SrcRect(), 0, 0, 320, 180);
    Float2 c;
    REQUIRE(fit.ToContent(Float2{100 + 640, 50 + 540}, c));
    CheckPoint(c, 160, 90);
    CHECK_FALSE(fit.ToContent(Float2{100 + 640, 50 + 90}, c)); // in a bar
    CheckPoint(fit.FromContent(Float2{0, 0}), 100, 50 + 180);
    CheckPoint(fit.Scale(), 0.25f, 0.25f);
    CheckPoint(fit.ToContentDelta(Float2{8, -4}), 2, -1);

    // Crop: the whole region shows a centred slice of the content.
    ContentFit crop{Rectangle{0, 0, 100, 100}, Float2{200, 100}, FitMode::Crop};
    CheckRect(crop.DstRect(), 0, 0, 100, 100);
    CheckRect(crop.SrcRect(), 50, 0, 100, 100);
}

TEST_CASE("content fit: a landscape game turned a quarter fills a portrait screen")
{
    // 1920x1080 content on a 1080x1920 portrait region: the turned-back region is 1920x1080, so it
    // fits exactly whichever way it is turned a quarter.
    for (const ContentRotation rotation : kQuarterTurns)
    {
        ContentFit fit{Rectangle{0, 0, 1080, 1920}, Float2{1920, 1080}, FitMode::Letterbox, rotation};
        CheckRect(fit.DstRect(), 0, 0, 1080, 1920);
        CheckRect(fit.SrcRect(), 0, 0, 1920, 1080);
        CheckPoint(fit.Scale(), 1, 1);
    }

    // Three quarters (the phone held upright, the game's top on the screen's left): the content's
    // top-left corner is the screen's bottom-left, its top-right the screen's top-left.
    ContentFit fit{Rectangle{0, 0, 1080, 1920}, Float2{1920, 1080}, FitMode::Letterbox,
                   ContentRotation::ThreeQuarter};
    CheckPoint(fit.FromContent(Float2{0, 0}), 0, 1920);
    CheckPoint(fit.FromContent(Float2{1920, 0}), 0, 0);
    CheckPoint(fit.FromContent(Float2{1920, 1080}), 1080, 0);
    Float2 c;
    REQUIRE(fit.ToContent(Float2{1, 1919}, c)); // near the screen's bottom-left: the content's top-left
    CheckPoint(c, 1, 1);
    // Moving a finger up the screen moves right across the content; right across the screen, down.
    CheckPoint(fit.ToContentDelta(Float2{0, -10}), 10, 0);
    CheckPoint(fit.ToContentDelta(Float2{10, 0}), 0, 10);

    // A quarter: the content's top-left is the screen's top-right.
    ContentFit quarter{Rectangle{0, 0, 1080, 1920}, Float2{1920, 1080}, FitMode::Letterbox,
                       ContentRotation::Quarter};
    CheckPoint(quarter.FromContent(Float2{0, 0}), 1080, 0);
    CheckPoint(quarter.ToContentDelta(Float2{0, 10}), 10, 0);
}

TEST_CASE("content fit: points round-trip through every turn and mode")
{
    // An offset region of another shape than the content: bars, crops and a floored scale in every
    // turn. A point of the content maps to the region and back to itself; the drawn rectangle's
    // corners map to the content's.
    const Rectangle region{30, 20, 700, 1300};
    for (const ContentRotation rotation : kRotations)
    {
        for (const FitMode mode : kModes)
        {
            CAPTURE(static_cast<int>(rotation));
            CAPTURE(static_cast<int>(mode));
            ContentFit fit{region, Float2{640, 360}, mode, rotation};
            const Rectangle src = fit.SrcRect();
            const Float2 inside[] = {Float2{src.x + src.width * 0.5f, src.y + src.height * 0.5f},
                                     Float2{src.x + src.width * 0.2f, src.y + src.height * 0.7f},
                                     Float2{src.x + src.width * 0.9f, src.y + src.height * 0.1f}};
            for (const Float2 point : inside)
            {
                const Float2 there = fit.FromContent(point);
                Float2 back;
                REQUIRE(fit.ToContent(there, back));
                CHECK(back.x == doctest::Approx(point.x).epsilon(0.001));
                CHECK(back.y == doctest::Approx(point.y).epsilon(0.001));
                CHECK(fit.DstRect().Contains(there));
                // The unclamped map agrees wherever the clamped one maps.
                const Float2 free = fit.ToContentUnclamped(there);
                CHECK(free.x == doctest::Approx(back.x));
                CHECK(free.y == doctest::Approx(back.y));
            }
            // The drawn rectangle sits inside the region.
            const Rectangle dst = fit.DstRect();
            CHECK(dst.x >= region.x - 0.01f);
            CHECK(dst.y >= region.y - 0.01f);
            CHECK(dst.x + dst.width <= region.x + region.width + 0.01f);
            CHECK(dst.y + dst.height <= region.y + region.height + 0.01f);
        }
    }
}

TEST_CASE("content fit: a half turn and the bars of a turned letterbox")
{
    // A half turn: the content's top-left is the region's bottom-right.
    ContentFit half{Rectangle{0, 0, 200, 100}, Float2{200, 100}, FitMode::Stretch, ContentRotation::Half};
    CheckPoint(half.FromContent(Float2{0, 0}), 200, 100);
    CheckPoint(half.ToContentDelta(Float2{3, 4}), -3, -4);

    // 320x180 in a 1080x1920 portrait region turned a quarter: the turned-back region is 1920x1080,
    // the content letterboxed in it at scale 6 (1920x1080 exactly), so no bars; at 300x180 there
    // are bars along what is the screen's top and bottom (the turned-back region's sides).
    ContentFit bars{Rectangle{0, 0, 1080, 1920}, Float2{300, 180}, FitMode::Letterbox, ContentRotation::Quarter};
    const Rectangle dst = bars.DstRect();
    CHECK(dst.width == doctest::Approx(1080));
    CHECK(dst.height == doctest::Approx(1800)); // 300 x 6
    CHECK(dst.y == doctest::Approx(60));        // bars of 60 above and below
    Float2 c;
    CHECK_FALSE(bars.ToContent(Float2{540, 30}, c)); // in a bar
    // Unclamped, the bar maps past the content's edge: turned a quarter, the screen's top is the
    // content's left.
    const Float2 free = bars.ToContentUnclamped(Float2{540, 30});
    CHECK(free.x < 0.0f);
    CHECK(free.y == doctest::Approx(90.0f));
}

TEST_CASE("content fit: the turn an orientation asks of a window")
{
    // Only a mismatched shape turns, and always three quarters (the game's top on the screen's
    // left); a square window or Any never turns.
    CHECK(TurnFor(ScreenOrientation::Landscape, 1080, 1920) == ContentRotation::ThreeQuarter);
    CHECK(TurnFor(ScreenOrientation::Landscape, 1920, 1080) == ContentRotation::None);
    CHECK(TurnFor(ScreenOrientation::Portrait, 1920, 1080) == ContentRotation::ThreeQuarter);
    CHECK(TurnFor(ScreenOrientation::Portrait, 1080, 1920) == ContentRotation::None);
    CHECK(TurnFor(ScreenOrientation::Landscape, 800, 800) == ContentRotation::None);
    CHECK(TurnFor(ScreenOrientation::Any, 1080, 1920) == ContentRotation::None);
    CHECK(TurnFor(ScreenOrientation::Any, 1920, 1080) == ContentRotation::None);
}
