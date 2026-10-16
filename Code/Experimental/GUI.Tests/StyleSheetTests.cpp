// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - StyleRule / StyleSheet cascade tests: property blocks and specificity-
// ordered resolution against a UIWidget.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    core::RefPtr<UIWidget> Widget(const char8_t* tag)
    {
        auto w = Make<UIWidget>();
        w->SetTag(core::StringView(tag));
        return w;
    }
    StyleRule Rule(const char8_t* selector) { return StyleRule(core::StringView(selector)); }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
}

TEST_CASE("style-rule: set and override declarations")
{
    StyleRule r = Rule(u8"button");
    r.SetProperty(SV(u8"color"), SV(u8"black"));
    r.SetProperty(SV(u8"padding"), SV(u8"4"));
    CHECK(r.PropertyCount() == 2);
    r.SetProperty(SV(u8"color"), SV(u8"white")); // override, not append
    CHECK(r.PropertyCount() == 2);
    CHECK(r.Specificity() == 1); // tag
}

TEST_CASE("resolved-style: set/get/has/override")
{
    ResolvedStyle s;
    CHECK_FALSE(s.Has(SV(u8"color")));
    s.Set(SV(u8"color"), SV(u8"red"));
    s.Set(SV(u8"color"), SV(u8"blue")); // override
    CHECK(s.Count() == 1);
    CHECK(s.Has(SV(u8"color")));
    CHECK(s.Get(SV(u8"color")) == SV(u8"blue"));
    CHECK(s.Get(SV(u8"missing"), SV(u8"fallback")) == SV(u8"fallback"));
}

TEST_CASE("stylesheet: higher specificity wins")
{
    auto w = Widget(u8"button");
    w->AddClass(SV(u8"primary"));

    StyleSheet sheet;
    StyleRule tagRule = Rule(u8"button");
    tagRule.SetProperty(SV(u8"color"), SV(u8"black"));
    tagRule.SetProperty(SV(u8"padding"), SV(u8"4"));
    StyleRule classRule = Rule(u8".primary");
    classRule.SetProperty(SV(u8"color"), SV(u8"white")); // class (1024) beats tag (1)
    sheet.AddRule(core::Move(tagRule));
    sheet.AddRule(core::Move(classRule));

    ResolvedStyle rs = sheet.Resolve(*w.Get());
    CHECK(rs.Get(SV(u8"color")) == SV(u8"white")); // from .primary
    CHECK(rs.Get(SV(u8"padding-top")) == SV(u8"4"));   // only in the tag rule
    CHECK_FALSE(rs.Has(SV(u8"margin")));
}

TEST_CASE("stylesheet: id beats class beats tag")
{
    auto w = Widget(u8"button");
    w->SetId(SV(u8"ok"));
    w->AddClass(SV(u8"primary"));

    StyleSheet sheet;
    StyleRule t = Rule(u8"button");
    t.SetProperty(SV(u8"color"), SV(u8"tag"));
    StyleRule c = Rule(u8".primary");
    c.SetProperty(SV(u8"color"), SV(u8"class"));
    StyleRule i = Rule(u8"#ok");
    i.SetProperty(SV(u8"color"), SV(u8"id"));
    sheet.AddRule(core::Move(t));
    sheet.AddRule(core::Move(i)); // add id before class to prove ordering is by specificity
    sheet.AddRule(core::Move(c));

    ResolvedStyle rs = sheet.Resolve(*w.Get());
    CHECK(rs.Get(SV(u8"color")) == SV(u8"id"));
}

TEST_CASE("stylesheet: equal specificity resolves by source order (last wins)")
{
    auto w = Widget(u8"button");
    w->AddClass(SV(u8"x"));

    StyleSheet sheet;
    StyleRule a = Rule(u8".x");
    a.SetProperty(SV(u8"color"), SV(u8"red"));
    StyleRule b = Rule(u8".x");
    b.SetProperty(SV(u8"color"), SV(u8"blue"));
    sheet.AddRule(core::Move(a));
    sheet.AddRule(core::Move(b));

    ResolvedStyle rs = sheet.Resolve(*w.Get());
    CHECK(rs.Get(SV(u8"color")) == SV(u8"blue")); // later source wins the tie
}

TEST_CASE("stylesheet: non-matching rules contribute nothing")
{
    auto w = Widget(u8"button");
    StyleSheet sheet;
    StyleRule other = Rule(u8"span");
    other.SetProperty(SV(u8"color"), SV(u8"nope"));
    sheet.AddRule(core::Move(other));

    ResolvedStyle rs = sheet.Resolve(*w.Get());
    CHECK(rs.Count() == 0);
}
