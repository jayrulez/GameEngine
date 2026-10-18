// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - @media query tests: condition evaluation against a MediaContext, and
// end-to-end parsing + media-gated resolution.
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
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    core::RefPtr<UIWidget> Widget(const char8_t* tag)
    {
        auto w = Make<UIWidget>();
        w->SetTag(SV(tag));
        return w;
    }
}

TEST_CASE("media-query: single feature")
{
    MediaQuery mq(SV(u8"(min-width: 600px)"));
    CHECK(mq.FeatureCount() == 1);
    CHECK(mq.Evaluate(MediaContext{800.0f, 0.0f, 96.0f}));
    CHECK_FALSE(mq.Evaluate(MediaContext{400.0f, 0.0f, 96.0f}));
    CHECK(mq.Evaluate(MediaContext{600.0f, 0.0f, 96.0f})); // inclusive
}

TEST_CASE("media-query: conjunction (and)")
{
    MediaQuery mq(SV(u8"(min-width: 600px) and (max-width: 900px)"));
    CHECK(mq.FeatureCount() == 2);
    CHECK(mq.Evaluate(MediaContext{800.0f, 0.0f, 96.0f}));
    CHECK_FALSE(mq.Evaluate(MediaContext{1000.0f, 0.0f, 96.0f})); // over max
    CHECK_FALSE(mq.Evaluate(MediaContext{500.0f, 0.0f, 96.0f}));  // under min
}

TEST_CASE("media-query: height features")
{
    MediaQuery mq(SV(u8"(max-height: 480px)"));
    CHECK(mq.Evaluate(MediaContext{0.0f, 400.0f, 96.0f}));
    CHECK_FALSE(mq.Evaluate(MediaContext{0.0f, 600.0f, 96.0f}));
}

TEST_CASE("media-query: empty always matches")
{
    MediaQuery empty;
    CHECK(empty.IsEmpty());
    CHECK(empty.Evaluate(MediaContext{0.0f, 0.0f, 0.0f}));
}

TEST_CASE("media-query: @media block parses and gates resolution")
{
    StyleSheet sheet = CSSParser::Parse(
        SV(u8"button { color: black; } @media (min-width: 600px) { button { color: red; } }"));
    CHECK(sheet.RuleCount() == 2);

    auto w = Widget(u8"button");
    // Wide viewport: the @media rule is active and (later source) wins.
    CHECK(sheet.Resolve(*w.Get(), MediaContext{800.0f, 600.0f, 96.0f}).Get(SV(u8"color")) ==
          SV(u8"red"));
    // Narrow viewport: the @media rule is inactive; the base rule stands.
    CHECK(sheet.Resolve(*w.Get(), MediaContext{400.0f, 600.0f, 96.0f}).Get(SV(u8"color")) ==
          SV(u8"black"));
}

TEST_CASE("media-query: multiple rules inside one @media block")
{
    StyleSheet sheet = CSSParser::Parse(
        SV(u8"@media (min-width: 600px) { button { color: red; } .box { padding: 8; } }"));
    CHECK(sheet.RuleCount() == 2);

    auto btn = Widget(u8"button");
    CHECK(sheet.Resolve(*btn.Get(), MediaContext{800.0f, 0.0f, 96.0f}).Get(SV(u8"color")) ==
          SV(u8"red"));
    CHECK_FALSE(sheet.Resolve(*btn.Get(), MediaContext{400.0f, 0.0f, 96.0f}).Has(SV(u8"color")));
}

TEST_CASE("media-query: a feature it does not know matches nothing, rather than everything")
{
    const MediaContext wide{800.0f, 600.0f, 96.0f};
    CHECK_FALSE(MediaQuery(SV(u8"(orientation: landscape)")).Evaluate(wide));
    CHECK_FALSE(MediaQuery(SV(u8"(prefers-reduced-motion)")).Evaluate(wide));
    CHECK_FALSE(MediaQuery(SV(u8"(min-width: 600px) and (hover: hover)")).Evaluate(wide));
    CHECK(MediaQuery(SV(u8"(min-width: 600px)")).Evaluate(wide));
    StyleSheet sheet = CSSParser::Parse(SV(u8"button { color: black; } @media (orientation: landscape) { button { color: red; } }"));
    CHECK(sheet.Resolve(*Widget(u8"button").Get(), wide).Get(SV(u8"color")) == SV(u8"black"));
}
