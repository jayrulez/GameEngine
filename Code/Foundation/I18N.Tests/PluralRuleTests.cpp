// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - plural rules as data: CLDR's integer syntax parsed once, evaluated per count.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.i18n;

using namespace foundation::core;
using namespace foundation::i18n;

TEST_CASE("i18n.plural: English's rule, one and other")
{
    PluralRule rule;
    REQUIRE(rule.Parse(u8"one: n = 1"));
    CHECK(rule.CategoryOf(0) == PluralCategory::Other);
    CHECK(rule.CategoryOf(1) == PluralCategory::One);
    CHECK(rule.CategoryOf(2) == PluralCategory::Other);
    CHECK(rule.Uses(PluralCategory::One));
    CHECK(rule.Uses(PluralCategory::Other));
    CHECK_FALSE(rule.Uses(PluralCategory::Few));
}

TEST_CASE("i18n.plural: a rule with modulo, ranges and or (Russian's, as data)")
{
    PluralRule rule;
    REQUIRE(rule.Parse(u8"one: n % 10 = 1 and n % 100 != 11; "
                       u8"few: n % 10 = 2..4 and n % 100 != 12..14; "
                       u8"many: n % 10 = 0 or n % 10 = 5..9 or n % 100 = 11..14; "
                       u8"other:"));
    CHECK(rule.CategoryOf(1) == PluralCategory::One);
    CHECK(rule.CategoryOf(21) == PluralCategory::One);
    CHECK(rule.CategoryOf(11) == PluralCategory::Many);
    CHECK(rule.CategoryOf(2) == PluralCategory::Few);
    CHECK(rule.CategoryOf(22) == PluralCategory::Few);
    CHECK(rule.CategoryOf(12) == PluralCategory::Many);
    CHECK(rule.CategoryOf(5) == PluralCategory::Many);
    CHECK(rule.CategoryOf(25) == PluralCategory::Many);
    CHECK(rule.CategoryOf(-1) == PluralCategory::One); // a negative count counts as its magnitude
}

TEST_CASE("i18n.plural: an empty rule counts everything as other; a list of values and ranges")
{
    PluralRule none;
    REQUIRE(none.Parse(u8""));
    CHECK(none.CategoryOf(1) == PluralCategory::Other);
    PluralRule list;
    REQUIRE(list.Parse(u8"few: n = 3, 5..7, 10"));
    CHECK(list.CategoryOf(3) == PluralCategory::Few);
    CHECK(list.CategoryOf(6) == PluralCategory::Few);
    CHECK(list.CategoryOf(10) == PluralCategory::Few);
    CHECK(list.CategoryOf(4) == PluralCategory::Other);
}

TEST_CASE("i18n.plural: a rule that does not parse is refused with where, and the old one stays")
{
    PluralRule rule;
    REQUIRE(rule.Parse(u8"one: n = 1"));
    PluralRuleError error;
    CHECK_FALSE(rule.Parse(u8"one: n == 1", &error));
    CHECK(error.Position == 8u); // the second =, where a number was due
    CHECK_FALSE(rule.Parse(u8"some: n = 1", &error));
    CHECK(error.Position == 0u);
    CHECK_FALSE(rule.Parse(u8"one: x = 1", &error));
    CHECK_FALSE(rule.Parse(u8"one: n = 4..2", &error));
    CHECK_FALSE(rule.Parse(u8"one: n = 1; one: n = 2", &error));
    CHECK_FALSE(rule.Parse(u8"one: n % 0 = 1", &error));
    CHECK(rule.Source() == StringView(u8"one: n = 1")); // unchanged
    CHECK(rule.CategoryOf(1) == PluralCategory::One);
}
