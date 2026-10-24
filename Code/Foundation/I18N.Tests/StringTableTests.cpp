// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - StringTable: locales with their rules and directions, plain and plural keys,
// missing cells, renames, and the payload round trip.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.i18n;

using namespace foundation::core;
using namespace foundation::i18n;

namespace
{
    void Sample(StringTable& t)
    {
        REQUIRE(t.AddLocale(u8"en"));
        REQUIRE(t.AddLocale(u8"he"));
        REQUIRE(t.SetPluralRule(u8"en", u8"one: n = 1"));
        REQUIRE(t.SetPluralRule(u8"he", u8"one: n = 1"));
        t.SetDirection(u8"en", TextDirection::LeftToRight);
        t.SetDirection(u8"he", TextDirection::RightToLeft);
        REQUIRE(t.Set(u8"menu.play", u8"en", u8"Play"));
        REQUIRE(t.Set(u8"menu.play", u8"he", u8"שחק"));
        REQUIRE(t.SetPlural(u8"files.count", u8"en", PluralCategory::One, u8"{0} file"));
        REQUIRE(t.SetPlural(u8"files.count", u8"en", PluralCategory::Other, u8"{0} files"));
        REQUIRE(t.SetPlural(u8"files.count", u8"he", PluralCategory::Other, u8"{0} קבצים"));
    }
}

TEST_CASE("i18n.table: plain texts per locale; a missing cell is not an empty string")
{
    StringTable t;
    Sample(t);
    REQUIRE(t.Text(u8"menu.play", u8"en") != nullptr);
    CHECK(*t.Text(u8"menu.play", u8"en") == String(u8"Play"));
    CHECK(t.Set(u8"menu.quit", u8"en", u8""));
    CHECK(t.HasText(u8"menu.quit", u8"en"));      // an empty translation
    CHECK_FALSE(t.HasText(u8"menu.quit", u8"he")); // a missing one
    CHECK(t.Text(u8"menu.quit", u8"he") == nullptr);
    CHECK(t.Set(u8"menu.play", u8"en-GB", u8"Play")); // a new locale, added
    CHECK(t.HasLocale(u8"en_gb"));
    CHECK_FALSE(t.Set(u8"menu.play", u8"not a locale", u8"x"));
}

TEST_CASE("i18n.table: a plural key holds its forms; it is plural in every locale")
{
    StringTable t;
    Sample(t);
    CHECK(t.IsPlural(u8"files.count"));
    CHECK(*t.PluralForm(u8"files.count", u8"en", PluralCategory::One) == String(u8"{0} file"));
    CHECK(*t.Text(u8"files.count", u8"en") == String(u8"{0} files")); // its other form
    CHECK_FALSE(t.Set(u8"files.count", u8"en", u8"plain"));             // plural stays plural
    CHECK_FALSE(t.SetPlural(u8"menu.play", u8"en", PluralCategory::One, u8"x")); // plain stays plain
    // What a locale's rule needs that an entry lacks.
    const Array<PluralCategory> heMissing = t.MissingForms(u8"files.count", u8"he", t.GetPluralRule(u8"he"));
    REQUIRE(heMissing.Size() == 1u);
    CHECK(heMissing[0] == PluralCategory::One);
    CHECK(t.MissingForms(u8"files.count", u8"en", t.GetPluralRule(u8"en")).IsEmpty());
}

TEST_CASE("i18n.table: keys renamed (every form with them) and removed; locales removed")
{
    StringTable t;
    Sample(t);
    CHECK(t.RenameKey(u8"files.count", u8"files.total"));
    CHECK_FALSE(t.HasKey(u8"files.count"));
    CHECK(*t.PluralForm(u8"files.total", u8"en", PluralCategory::One) == String(u8"{0} file"));
    CHECK_FALSE(t.RenameKey(u8"menu.play", u8"files.total")); // taken
    CHECK(t.Remove(u8"menu.play"));
    CHECK_FALSE(t.HasKey(u8"menu.play"));
    CHECK(t.RemoveLocale(u8"he"));
    CHECK(t.LocaleCount() == 1u);
    CHECK(*t.Text(u8"files.total", u8"en") == String(u8"{0} files"));
}

TEST_CASE("i18n.table: a locale's rule and direction, set, left unset, and refused when the rule does not parse")
{
    StringTable t;
    Sample(t);
    CHECK(t.GetDirection(u8"he") == TextDirection::RightToLeft);
    t.SetDirection(u8"he", TextDirection::Unset);
    CHECK(t.GetDirection(u8"he") == TextDirection::Unset);
    PluralRuleError error;
    CHECK_FALSE(t.SetPluralRule(u8"en", u8"one: n ==", &error));
    REQUIRE(t.GetPluralRule(u8"en") != nullptr);
    CHECK(t.GetPluralRule(u8"en")->Source() == StringView(u8"one: n = 1")); // the old rule stays
    t.ClearPluralRule(u8"en");
    CHECK(t.GetPluralRule(u8"en") == nullptr);
    CHECK_FALSE(t.SetPluralRule(u8"fr", u8"one: n = 1")); // not the table's locale
}

TEST_CASE("i18n.table: texts, plural entries, rules and directions survive the payload's round trip")
{
    RegisterStringTableType();
    StringTable written;
    Sample(written);
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        written.Serialize(ar);
        REQUIRE(ar.IsPayloadOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    StringTable read;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        read.Serialize(ar);
        REQUIRE(ar.IsPayloadOk());
    }
    CHECK(read.LocaleCount() == 2u);
    CHECK(*read.Text(u8"menu.play", u8"he") == String(u8"שחק"));
    CHECK(read.IsPlural(u8"files.count"));
    CHECK(*read.PluralForm(u8"files.count", u8"en", PluralCategory::One) == String(u8"{0} file"));
    REQUIRE(read.GetPluralRule(u8"en") != nullptr);
    CHECK(read.GetPluralRule(u8"en")->CategoryOf(1) == PluralCategory::One);
    CHECK(read.GetDirection(u8"he") == TextDirection::RightToLeft);
    CHECK(read.HasKey(u8"menu.play")); // the index rebuilt
}

namespace
{
    // A payload shaped as a table's, but whose one entry has a cell for one of its two locales: what
    // a damaged file, or a fuzzer, hands the reader.
    void WriteShortEntry(ISerializer& ar)
    {
        BeginVersionedPayload(ar, StringTable::StaticType());
        u32 locales = 2;
        ar.Key("locales");
        ar.BeginArray(locales);
        const char8_t* ids[2] = {u8"en", u8"fr"};
        for (const char8_t* id : ids)
        {
            String localeId(id);
            bool hasRule = false;
            String rule;
            String direction(u8"unset");
            ar.BeginObject();
            foundation::core::Serialize(ar, "id", localeId);
            foundation::core::Serialize(ar, "hasRule", hasRule);
            foundation::core::Serialize(ar, "rule", rule);
            foundation::core::Serialize(ar, "direction", direction);
            ar.EndObject();
        }
        ar.EndArray();
        u32 entries = 1;
        ar.Key("entries");
        ar.BeginArray(entries);
        String key(u8"menu.play");
        bool plural = false;
        ar.BeginObject();
        foundation::core::Serialize(ar, "key", key);
        foundation::core::Serialize(ar, "plural", plural);
        u32 cells = 1;
        ar.Key("cells");
        ar.BeginArray(cells);
        bool present = true;
        String text(u8"Play");
        u8 forms = 0;
        ar.BeginObject();
        foundation::core::Serialize(ar, "present", present);
        foundation::core::Serialize(ar, "text", text);
        foundation::core::Serialize(ar, "forms", forms);
        for (u32 f = 0; f < kPluralCategoryCount; ++f)
        {
            String form;
            const String name(PluralCategoryName(static_cast<PluralCategory>(f)));
            ar.Key(reinterpret_cast<const char*>(name.CStr()));
            ar.Text(form);
        }
        ar.EndObject();
        ar.EndArray();
        ar.EndObject();
        ar.EndArray();
        EndVersionedPayload(ar);
    }
}

TEST_CASE("i18n.table: an entry short of its locales' cells fails the read and leaves the table safe to ask")
{
    // Found by the GUI's string table fuzzer: the read failed, but kept the short entry, and a
    // caller that did not check asked a locale past its cells.
    RegisterStringTableType();
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        WriteShortEntry(ar);
        REQUIRE(ar.IsPayloadOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    StringTable read;
    BinarySerializer ar(stream, SerializeMode::Read);
    read.Serialize(ar);
    CHECK_FALSE(ar.IsPayloadOk());
    CHECK(*read.Text(u8"menu.play", u8"en") == String(u8"Play"));
    CHECK(read.Text(u8"menu.play", u8"fr") == nullptr); // the missing cell: missing, not past the end
}
