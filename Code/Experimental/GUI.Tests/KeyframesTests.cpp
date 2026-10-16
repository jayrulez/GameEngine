// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - @keyframes tests: parsing keyframe blocks, and the animation runtime (an
// `animation` property spawns a KeyframeAction that interpolates opacity across the stops as the
// scene ticks, looping when requested).
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
    core::Duration Secs(double s) { return core::Duration::FromSeconds(s); }
}

TEST_CASE("keyframes: parse names, offsets (%, from/to), and stop declarations")
{
    StyleSheet sheet = CSSParser::Parse(core::StringView(
        u8"@keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.25; } 100% { opacity: 1; } }"
        u8"@keyframes fade  { from { opacity: 1; } to { opacity: 0; } }"));

    CHECK(sheet.KeyframesCount() == 2);

    const Keyframes* pulse = sheet.FindKeyframes(core::StringView(u8"pulse"));
    REQUIRE(pulse != nullptr);
    REQUIRE(pulse->Stops.Size() == 3);
    CHECK(pulse->Stops[0].Offset == doctest::Approx(0.0f));
    CHECK(pulse->Stops[1].Offset == doctest::Approx(0.5f));
    CHECK(pulse->Stops[2].Offset == doctest::Approx(1.0f));
    CHECK(pulse->Stops[1].Properties.Size() == 1);

    const Keyframes* fade = sheet.FindKeyframes(core::StringView(u8"fade"));
    REQUIRE(fade != nullptr);
    REQUIRE(fade->Stops.Size() == 2);
    CHECK(fade->Stops[0].Offset == doctest::Approx(0.0f)); // from
    CHECK(fade->Stops[1].Offset == doctest::Approx(1.0f)); // to
}

TEST_CASE("keyframes: a comma offset list shares the block")
{
    StyleSheet sheet = CSSParser::Parse(
        core::StringView(u8"@keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0; } }"));
    const Keyframes* blink = sheet.FindKeyframes(core::StringView(u8"blink"));
    REQUIRE(blink != nullptr);
    CHECK(blink->Stops.Size() == 3); // 0%, 100%, 50%
}

TEST_CASE("keyframes: the animation property drives opacity across the stops")
{
    auto root = Make<SceneNode>();
    auto w = Make<UIWidget>();
    w->AddClass(core::StringView(u8"anim"));
    root->AddChild(w.Get());

    StyleManager mgr;
    mgr.SetStyleSheet(CSSParser::Parse(core::StringView(
        u8"@keyframes fade { 0% { opacity: 1; } 50% { opacity: 0; } 100% { opacity: 1; } }"
        u8".anim { animation: fade 2s; }")));
    mgr.ApplyTree(*root.Get()); // spawns the KeyframeAction; Start() applies t=0

    CHECK(w->GetAlpha() == doctest::Approx(1.0f)); // stop 0% -> 1

    root->Update(Secs(0.5)); // t=0.25 -> between 1 and 0, halfway -> 0.5
    CHECK(w->GetAlpha() == doctest::Approx(0.5f));

    root->Update(Secs(0.5)); // t=0.5 -> 0
    CHECK(w->GetAlpha() == doctest::Approx(0.0f));

    root->Update(Secs(0.5)); // t=0.75 -> halfway back -> 0.5
    CHECK(w->GetAlpha() == doctest::Approx(0.5f));

    root->Update(Secs(0.5)); // t=1.0 -> 1 (non-looping: ends here)
    CHECK(w->GetAlpha() == doctest::Approx(1.0f));
}

TEST_CASE("keyframes: infinite animations loop")
{
    auto root = Make<SceneNode>();
    auto w = Make<UIWidget>();
    w->AddClass(core::StringView(u8"loop"));
    root->AddChild(w.Get());

    StyleManager mgr;
    mgr.SetStyleSheet(CSSParser::Parse(core::StringView(
        u8"@keyframes fade { 0% { opacity: 1; } 50% { opacity: 0; } 100% { opacity: 1; } }"
        u8".loop { animation: fade 2s infinite; }")));
    mgr.ApplyTree(*root.Get());

    root->Update(Secs(1.0)); // t=0.5 -> 0
    CHECK(w->GetAlpha() == doctest::Approx(0.0f));
    root->Update(Secs(1.0)); // elapsed 2.0 -> wraps to t=0 -> 1 (still running)
    CHECK(w->GetAlpha() == doctest::Approx(1.0f));
    root->Update(Secs(1.0)); // elapsed 3.0 -> t=0.5 -> 0 again (looped)
    CHECK(w->GetAlpha() == doctest::Approx(0.0f));
}

TEST_CASE("keyframes: the animation is spawned once, not re-spawned each ApplyTree")
{
    auto root = Make<SceneNode>();
    auto w = Make<UIWidget>();
    w->AddClass(core::StringView(u8"anim"));
    root->AddChild(w.Get());

    StyleManager mgr;
    mgr.SetStyleSheet(CSSParser::Parse(core::StringView(
        u8"@keyframes fade { 0% { opacity: 1; } 50% { opacity: 0; } 100% { opacity: 1; } }"
        u8".anim { animation: fade 2s; }")));

    mgr.ApplyTree(*root.Get());
    root->Update(Secs(1.0));                       // t=0.5 -> 0
    mgr.ApplyTree(*root.Get());                    // re-apply must NOT restart the animation
    CHECK(w->GetAlpha() == doctest::Approx(0.0f)); // still mid-animation, not reset to t=0
}

TEST_CASE("keyframes: animates background-color across the stops")
{
    auto root = Make<SceneNode>();
    auto w = Make<UIWidget>();
    w->AddClass(core::StringView(u8"cycle"));
    root->AddChild(w.Get());

    StyleManager mgr;
    mgr.SetStyleSheet(CSSParser::Parse(core::StringView(
        u8"@keyframes cyc { 0% { background-color: #000000; } 100% { background-color: #ffffff; } }"
        u8".cycle { animation: cyc 2s; }")));
    mgr.ApplyTree(*root.Get());

    // At t=0 the background is black; halfway it's mid-grey; at the end white.
    auto grey = [&](float expect)
    {
        CHECK(w->GetBox().BackgroundColor.r == doctest::Approx(expect));
    };
    grey(0.0f);
    root->Update(Secs(1.0)); // t=0.5 -> mid grey
    grey(0.5f);
    root->Update(Secs(1.0)); // t=1.0 -> white
    grey(1.0f);
}
