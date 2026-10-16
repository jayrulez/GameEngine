// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Rich text runs and their markup (F2 of Specs/vg-and-gui-lab.md): what each tag sets, how runs
// merge and nest, and that nothing malformed is ever dropped.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;

using namespace foundation::core;
using namespace foundation::ui;

TEST_CASE("text runs: plain text is one unstyled run")
{
    const TextDocument doc = ParseTextMarkup(u8"Your mark wears a gown");
    REQUIRE(doc.runs.Size() == 1u);
    CHECK(doc.runs[0].text == StringView(u8"Your mark wears a gown"));
    CHECK_FALSE(doc.runs[0].color.HasValue());
    CHECK_FALSE(doc.runs[0].bold);
    CHECK(doc.diagnostics.IsEmpty());
}

TEST_CASE("text runs: tags style their span and nest")
{
    const TextDocument doc =
        ParseTextMarkup(u8"Your mark wears [color=#d04060][b]a rose[/b] gown[/color] [size=24][i]now[/]");
    REQUIRE(doc.runs.Size() == 5u);
    CHECK(doc.runs[0].text == StringView(u8"Your mark wears "));
    CHECK(doc.runs[1].text == StringView(u8"a rose"));
    CHECK(doc.runs[1].bold);
    REQUIRE(doc.runs[1].color.HasValue());
    CHECK(doc.runs[1].color.Value().r == doctest::Approx(0xd0 / 255.0f));
    CHECK(doc.runs[2].text == StringView(u8" gown"));
    CHECK_FALSE(doc.runs[2].bold);
    CHECK(doc.runs[2].color.HasValue()); // still inside the colour
    CHECK(doc.runs[3].text == StringView(u8" "));
    CHECK_FALSE(doc.runs[3].color.HasValue());
    CHECK(doc.runs[4].text == StringView(u8"now"));
    CHECK(doc.runs[4].italic);
    REQUIRE(doc.runs[4].size.HasValue());
    CHECK(doc.runs[4].size.Value() == doctest::Approx(24.0f));
    CHECK(doc.diagnostics.IsEmpty());
    CHECK(doc.PlainText() == StringView(u8"Your mark wears a rose gown now"));
}

TEST_CASE("text runs: fonts, underline, links and icons")
{
    const TextDocument doc =
        ParseTextMarkup(u8"[font=Chewy][u]Hi[/u][/font] [icon=Icons/rose.png] [link=hint]what?[/link]");
    REQUIRE(doc.runs.Size() == 5u);
    REQUIRE(doc.runs[0].family.HasValue());
    CHECK(doc.runs[0].family.Value() == StringView(u8"Chewy"));
    CHECK(doc.runs[0].underline);
    CHECK(doc.runs[2].IsIcon());
    CHECK(doc.runs[2].icon == StringView(u8"Icons/rose.png"));
    CHECK(doc.runs[2].text.IsEmpty());
    CHECK(doc.runs[4].link == StringView(u8"hint"));
    CHECK(doc.runs[4].text == StringView(u8"what?"));
    CHECK(doc.PlainText() == StringView(u8"Hi ￼ what?"));
}

TEST_CASE("text runs: a doubled bracket is a literal one, and a lone ] is text")
{
    const TextDocument doc = ParseTextMarkup(u8"[[b]] is not bold ]");
    REQUIRE(doc.runs.Size() == 1u);
    CHECK(doc.runs[0].text == StringView(u8"[b]] is not bold ]"));
    CHECK_FALSE(doc.runs[0].bold);
    CHECK(doc.diagnostics.IsEmpty());
}

TEST_CASE("text runs: malformed tags stay as text and are reported")
{
    const TextDocument doc =
        ParseTextMarkup(u8"a [blink]b[/blink] [size=big]c [color=nope]d [/i] [b=1]e [ f");
    // Every character of the source is still there.
    CHECK(doc.PlainText() == StringView(u8"a [blink]b[/blink] [size=big]c [color=nope]d [/i] [b=1]e [ f"));
    for (const TextRun& run : doc.runs)
    {
        CHECK_FALSE(run.bold);
        CHECK_FALSE(run.size.HasValue());
        CHECK_FALSE(run.color.HasValue());
    }
    // blink, /blink, size, color, /i, b=1, and the unclosed bracket.
    CHECK(doc.diagnostics.Size() == 7u);
    CHECK(doc.diagnostics[0].offset == 2u);
}

TEST_CASE("text runs: a close past inner tags closes them too, and colour names resolve")
{
    const TextMarkupColorResolver palette = [](StringView name) -> Optional<Color>
    {
        if (name == StringView(u8"gold"))
            return Color{1.0f, 0.8f, 0.2f, 1.0f};
        return {};
    };
    const TextDocument doc = ParseTextMarkup(u8"[color=$gold][b]x[/color]y [color=red]z", &palette);
    REQUIRE(doc.runs.Size() == 3u);
    CHECK(doc.runs[0].bold);
    REQUIRE(doc.runs[0].color.HasValue());
    CHECK(doc.runs[0].color.Value().g == doctest::Approx(0.8f));
    CHECK_FALSE(doc.runs[1].bold); // closed with the colour
    CHECK_FALSE(doc.runs[1].color.HasValue());
    REQUIRE(doc.runs[2].color.HasValue()); // a named CSS colour without the resolver's help
    CHECK(doc.runs[2].color.Value().r == doctest::Approx(1.0f));
    CHECK(doc.diagnostics.Size() == 1u); // [/color] closing the [b] inside it
}
