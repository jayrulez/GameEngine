// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - Localization: the fallback chain, the last table winning, misses, plural forms
// by the language the text came from, positional formatting, direction, pseudolocalization and
// change callbacks.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.i18n;

using namespace foundation::core;
using namespace foundation::i18n;

namespace
{
    // English (the fallback) and Portuguese, with a Brazilian override for one key.
    void Base(StringTable& t)
    {
        REQUIRE(t.AddLocale(u8"en"));
        REQUIRE(t.AddLocale(u8"pt"));
        REQUIRE(t.SetPluralRule(u8"en", u8"one: n = 1"));
        REQUIRE(t.Set(u8"menu.play", u8"en", u8"Play"));
        REQUIRE(t.Set(u8"menu.play", u8"pt", u8"Jogar"));
        REQUIRE(t.Set(u8"menu.quit", u8"en", u8"Quit"));
        REQUIRE(t.Set(u8"menu.options", u8"pt-BR", u8"Opções"));
        REQUIRE(t.Set(u8"menu.options", u8"en", u8"Options"));
        REQUIRE(t.SetPlural(u8"files.count", u8"en", PluralCategory::One, u8"{0} file"));
        REQUIRE(t.SetPlural(u8"files.count", u8"en", PluralCategory::Other, u8"{0} files"));
    }
}

TEST_CASE("i18n.localization: each hop of the chain: the language, its parent, the fallback, the key")
{
    StringTable t;
    Base(t);
    Localization loc;
    loc.AddTable(&t);
    loc.SetLanguage(u8"pt-BR");
    CHECK(loc.Text(u8"menu.options") == StringView(u8"Opções")); // the language itself
    CHECK(loc.Text(u8"menu.play") == StringView(u8"Jogar"));     // its parent
    CHECK(loc.Text(u8"menu.quit") == StringView(u8"Quit"));      // the fallback
    CHECK(loc.Text(u8"menu.none") == StringView(u8"menu.none")); // a miss: the key itself
    CHECK(loc.Has(u8"menu.quit"));
    CHECK_FALSE(loc.Has(u8"menu.none"));
    loc.SetFallbackLanguage(u8"en-GB"); // the fallback's parent
    CHECK(loc.Text(u8"menu.quit") == StringView(u8"Quit"));
}

TEST_CASE("i18n.localization: a miss is kept once, with the language it was asked in")
{
    StringTable t;
    Base(t);
    Localization loc;
    loc.AddTable(&t);
    loc.SetLanguage(u8"pt");
    (void)loc.Text(u8"menu.none");
    (void)loc.Text(u8"menu.none");
    (void)loc.Text(u8"menu.other");
    REQUIRE(loc.MissingKeys().Size() == 2u);
    CHECK(loc.MissingKeys()[0].Key == String(u8"menu.none"));
    CHECK(loc.MissingKeys()[0].Language == String(u8"pt"));
    loc.ClearMissingKeys();
    CHECK(loc.MissingKeys().IsEmpty());
}

TEST_CASE("i18n.localization: the last table wins, and removing it uncovers the one below")
{
    StringTable base;
    Base(base);
    StringTable patch;
    REQUIRE(patch.Set(u8"menu.play", u8"en", u8"Start"));
    Localization loc;
    loc.AddTable(&base);
    loc.AddTable(&patch);
    CHECK(loc.Text(u8"menu.play") == StringView(u8"Start"));
    loc.RemoveTable(&patch);
    CHECK(loc.Text(u8"menu.play") == StringView(u8"Play"));
}

TEST_CASE("i18n.localization: a plural form by the rule of the language whose text was found")
{
    StringTable t;
    Base(t);
    Localization loc;
    loc.AddTable(&t);
    CHECK(loc.Plural(u8"files.count", 1) == StringView(u8"{0} file"));
    CHECK(loc.Plural(u8"files.count", 2) == StringView(u8"{0} files"));
    // Portuguese has no text for it: English's text, by English's rule.
    loc.SetLanguage(u8"pt");
    CHECK(loc.Plural(u8"files.count", 1) == StringView(u8"{0} file"));
    // A locale with no rule counts all as other.
    REQUIRE(t.SetPlural(u8"files.count", u8"pt", PluralCategory::One, u8"{0} arquivo"));
    REQUIRE(t.SetPlural(u8"files.count", u8"pt", PluralCategory::Other, u8"{0} arquivos"));
    CHECK(loc.PluralCategoryOf(u8"pt", 1) == PluralCategory::Other);
    CHECK(loc.Plural(u8"files.count", 1) == StringView(u8"{0} arquivos"));
    // Another table gives Portuguese its rule; the last such wins.
    StringTable rules;
    REQUIRE(rules.AddLocale(u8"pt"));
    REQUIRE(rules.SetPluralRule(u8"pt", u8"one: n = 1"));
    loc.AddTable(&rules);
    CHECK(loc.Plural(u8"files.count", 1) == StringView(u8"{0} arquivo"));
    CHECK(loc.Text(u8"files.count") == StringView(u8"{0} arquivos")); // Text is the other form
    CHECK(loc.Plural(u8"menu.play", 3) == StringView(u8"Jogar"));     // a plain key: its text
}

TEST_CASE("i18n.localization: positional formatting, reordered, escaped, and a missing argument shown")
{
    StringTable t;
    REQUIRE(t.Set(u8"greet", u8"en", u8"{1}, {0}!"));
    REQUIRE(t.Set(u8"brace", u8"en", u8"{{{0}}} and {3}"));
    REQUIRE(t.SetPluralRule(u8"en", u8"one: n = 1"));
    REQUIRE(t.SetPlural(u8"lives", u8"en", PluralCategory::One, u8"{0} life left"));
    REQUIRE(t.SetPlural(u8"lives", u8"en", PluralCategory::Other, u8"{0} lives left"));
    Localization loc;
    loc.AddTable(&t);
    const Variant greetArgs[2] = {Variant::From<String>(String(u8"Ana")), Variant::From<String>(String(u8"Hello"))};
    CHECK(loc.Format(u8"greet", Span<const Variant>(greetArgs, 2)) == String(u8"Hello, Ana!"));
    const Variant one[1] = {Variant::From<i32>(7)};
    CHECK(loc.Format(u8"brace", Span<const Variant>(one, 1)) == String(u8"{7} and {3}"));
    const Variant count[1] = {Variant::From<i64>(3)};
    CHECK(loc.FormatPlural(u8"lives", 3, Span<const Variant>(count, 1)) == String(u8"3 lives left"));
    const Variant mixed[3] = {Variant::From<bool>(true), Variant::From<f64>(1.5), Variant::From<StringView>(StringView(u8"x"))};
    CHECK(loc.Format(u8"none.{0}", Span<const Variant>(mixed, 3)) == String(u8"none.true")); // a missing key formats the key
}

TEST_CASE("i18n.localization: the direction the tables say, through parents, left to right when none says")
{
    StringTable t;
    REQUIRE(t.AddLocale(u8"ar"));
    t.SetDirection(u8"ar", TextDirection::RightToLeft);
    Localization loc;
    loc.AddTable(&t);
    CHECK_FALSE(loc.IsRightToLeft());
    loc.SetLanguage(u8"ar-EG");
    CHECK(loc.IsRightToLeft());
    CHECK_FALSE(loc.IsRightToLeft(u8"fr"));
    StringTable over;
    REQUIRE(over.AddLocale(u8"ar"));
    over.SetDirection(u8"ar", TextDirection::LeftToRight); // the last table wins
    loc.AddTable(&over);
    CHECK_FALSE(loc.IsRightToLeft());
}

TEST_CASE("i18n.localization: pseudolocalization transforms text, leaving placeholders and named markup")
{
    StringTable t;
    REQUIRE(t.Set(u8"msg", u8"en", u8"Hi {0} [b]x[/b]"));
    Localization loc;
    loc.AddTable(&t);
    PseudoOptions pseudo;
    pseudo.Enabled = true;
    pseudo.Lengthen = 0.0f;
    pseudo.Untouched.PushBack(UntouchedSpan{String(u8"["), String(u8"]")});
    loc.SetPseudolocalization(pseudo);
    CHECK(loc.Text(u8"msg") == StringView(u8"[Ĥî {0} [b]ẋ[/b]]"));
    pseudo.Lengthen = 0.5f;
    pseudo.Brackets = false;
    pseudo.Untouched.Clear();
    pseudo.ShowKeys = true;
    loc.SetPseudolocalization(pseudo);
    REQUIRE(t.Set(u8"two", u8"en", u8"ab"));
    CHECK(loc.Text(u8"two") == StringView(u8"two: áƀ~"));        // half again as long
    CHECK(loc.Text(u8"gone") == StringView(u8"gone: ĝöñé~~"));    // a miss transformed too
}

TEST_CASE("i18n.localization: changes call back; a removed token does not")
{
    StringTable t;
    Localization loc;
    int calls = 0;
    const u32 token = loc.OnChanged([&calls]() { ++calls; });
    loc.SetLanguage(u8"de");
    loc.AddTable(&t);
    loc.RemoveTable(&t);
    loc.SetPseudolocalization(PseudoOptions{});
    CHECK(calls == 4);
    loc.RemoveOnChanged(token);
    loc.SetLanguage(u8"fr");
    CHECK(calls == 4);
}
