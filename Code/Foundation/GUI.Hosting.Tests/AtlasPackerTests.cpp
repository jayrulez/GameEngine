// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - the atlas's shelf packer: pictures in rows across a page, each row as tall as the
// first it took; a picture on the shortest row it fits wasting no more than a third, else a new
// row under the last; none when the page is full; emptied by a reset.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui.hosting;

using namespace foundation::core;
using namespace foundation::gui;

TEST_CASE("gui.hosting.packer: pictures go across a row, a taller one on a new row under it")
{
    ShelfPacker packer(DefaultAllocator(), 64, 64);
    const Optional<PackedPlace> a = packer.Place(18, 18);
    const Optional<PackedPlace> b = packer.Place(18, 18);
    REQUIRE(a.HasValue());
    REQUIRE(b.HasValue());
    CHECK(a.Value().X == 0u);
    CHECK(a.Value().Y == 0u);
    CHECK(b.Value().X == 18u); // beside it
    CHECK(b.Value().Y == 0u);
    const Optional<PackedPlace> tall = packer.Place(10, 34); // too tall for the first row
    REQUIRE(tall.HasValue());
    CHECK(tall.Value().Y == 18u);
    CHECK(tall.Value().X == 0u);
    // A small one goes on the row it wastes least of: the 18 high, not the 34.
    const Optional<PackedPlace> small = packer.Place(10, 16);
    REQUIRE(small.HasValue());
    CHECK(small.Value().Y == 0u);
    CHECK(small.Value().X == 36u);
    // A short one would waste more than a third of either: a row of its own.
    const Optional<PackedPlace> flat = packer.Place(10, 6);
    REQUIRE(flat.HasValue());
    CHECK(flat.Value().Y == 52u);
    CHECK(packer.AreaUsed() == 18u * 18u * 2u + 10u * 34u + 10u * 16u + 10u * 6u);
}

TEST_CASE("gui.hosting.packer: a full page places nothing more, and a reset empties it")
{
    ShelfPacker packer(DefaultAllocator(), 32, 32);
    for (int i = 0; i < 4; ++i)
        CHECK(packer.Place(16, 16).HasValue());
    CHECK_FALSE(packer.Place(16, 16).HasValue());
    CHECK_FALSE(packer.Place(40, 4).HasValue()); // wider than the page
    packer.Reset();
    CHECK(packer.AreaUsed() == 0u);
    const Optional<PackedPlace> again = packer.Place(16, 16);
    REQUIRE(again.HasValue());
    CHECK(again.Value().X == 0u);
    CHECK(again.Value().Y == 0u);
}
