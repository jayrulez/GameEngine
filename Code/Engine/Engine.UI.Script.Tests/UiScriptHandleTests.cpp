// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.ui.script - the reflected view handles (native, no VM).
//
// These prove the properties the scripted parity suite relies on but cannot express: the OWNING
// handle keeps a view alive after its screen is dropped (no use-after-free - Fable finding B), and the
// typed finders are loud-null on a missing name OR a type mismatch, with ops on a null handle safe.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.script; // IScriptDelegate (a click handler)
import engine.ui.script;

using namespace foundation::core;
namespace ui = foundation::ui;
namespace uis = engine::uiscript;

namespace
{
    // Wrap a borrowed view in a group handle (the finders live on it).
    [[nodiscard]] uis::ViewGroup Group(ui::ViewGroup* g)
    {
        uis::ViewGroup h;
        h.view = RefPtr<ui::View>(g);
        return h;
    }

    [[nodiscard]] RefPtr<ui::Label> MakeLabel(StringView name, StringView text)
    {
        auto l = MakeRef<ui::Label>(DefaultAllocator());
        l->Name = String(name);
        l->Text.SetValue(String(text));
        return l;
    }

    // A click handler that counts its calls, in place of a script function.
    class CountingDelegate final : public foundation::script::IScriptDelegate
    {
    public:
        int calls = 0;
        Result<Variant> Invoke(Span<Variant>) override
        {
            ++calls;
            return Variant{};
        }
    };
}

TEST_CASE("uiscript.handle: an owning handle keeps its view alive after the tree drops it")
{
    auto group = MakeRef<ui::FrameLayout>(DefaultAllocator());
    auto label = MakeLabel(u8"status", u8"Idle");
    group->AddView(label.Get());

    // A script would hold this handle (e.g. a cached HUD label). It owns a ref.
    uis::Label held = Group(group.Get()).findLabel(u8"status");
    REQUIRE(held.isValid());

    // The screen is popped: drop the tree's + our local strong refs. The handle's RefPtr is the last
    // owner, so the label stays alive - ops must be safe, never a UAF.
    group->RemoveView(label.Get());
    group = nullptr;
    label = nullptr;

    CHECK(held.isValid());
    held.setText(u8"Loading"); // safe: operates on the detached-but-alive view
    CHECK(held.text() == StringView(u8"Loading"));
}

TEST_CASE("uiscript.handle: typed finders are loud-null on missing name or type mismatch")
{
    auto group = MakeRef<ui::FrameLayout>(DefaultAllocator());
    group->AddView(MakeLabel(u8"status", u8"Idle").Get());
    auto button = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Go"));
    button->Name = String(u8"go");
    group->AddView(button.Get());

    uis::ViewGroup g = Group(group.Get());

    CHECK(g.findLabel(u8"status").isValid());   // present + right type
    CHECK_FALSE(g.findLabel(u8"go").isValid());  // present but a Button -> loud null
    CHECK_FALSE(g.findLabel(u8"nope").isValid()); // absent -> loud null
    CHECK(g.findButton(u8"go").isValid());
    CHECK_FALSE(g.findButton(u8"status").isValid());

    // Ops on a null handle are safe no-ops (no crash, nothing happens).
    uis::Label absent = g.findLabel(u8"nope");
    absent.setText(u8"ignored");
    CHECK(absent.text() == StringView(u8""));
}

TEST_CASE("uiscript.handle: findLabel searches the subtree deeply, first match")
{
    auto root = MakeRef<ui::FrameLayout>(DefaultAllocator());
    auto mid = MakeRef<ui::FrameLayout>(DefaultAllocator());
    auto inner = MakeRef<ui::FrameLayout>(DefaultAllocator());
    root->AddView(mid.Get());
    mid->AddView(inner.Get());
    inner->AddView(MakeLabel(u8"deep", u8"found").Get());

    uis::Label deep = Group(root.Get()).findLabel(u8"deep");
    REQUIRE(deep.isValid());
    CHECK(deep.text() == StringView(u8"found"));
}

// A card is a ContentButton (a picture and labels as its content): the Button handle covers it, its
// click runs the script's handler, and a label inside its content is found from the screen.
TEST_CASE("uiscript.handle: a ContentButton is a Button, its labels found through its content")
{
    auto root = MakeRef<ui::FrameLayout>(DefaultAllocator());
    auto content = MakeRef<ui::FrameLayout>(DefaultAllocator());
    content->AddView(MakeLabel(u8"card-best", u8"Gold").Get());
    auto card = MakeRef<ui::ContentButton>(DefaultAllocator(), RefPtr<ui::View>(content.Get()));
    card->Name = String(u8"card");
    root->AddView(card.Get());

    uis::ViewGroup g = Group(root.Get());
    uis::Button button = g.findButton(u8"card");
    REQUIRE(button.isValid());
    CHECK(button.text() == StringView(u8"")); // no text of its own: its content is its face

    auto handler = MakeRef<CountingDelegate>(DefaultAllocator());
    button.onClick(RefPtr<foundation::script::IScriptDelegate>(handler.Get()));
    card->OnClick.Invoke(card.Get()); // headless (no context): the handler runs at once
    CHECK(handler->calls == 1);

    uis::Label best = g.findLabel(u8"card-best");
    REQUIRE(best.isValid());
    CHECK(best.text() == StringView(u8"Gold"));
}

// Sedulous 3bffde51: a view's opacity at once, or faded on the UI's frame clock (which runs while
// the game is paused); a set stops a running fade; a null handle takes nothing.
TEST_CASE("uiscript.handle: a view fades on the frame clock, and a set stops the fade")
{
    ui::UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800.0f, 600.0f};
    ctx.AddRootView(root.Get());
    auto panel = MakeRef<ui::FrameLayout>(DefaultAllocator());
    panel->Name = String(u8"panel");
    root->AddView(panel.Get());
    panel->AddView(MakeLabel(u8"title", u8"Paused").Get());
    uis::ViewGroup rootGroup = Group(root.Get());

    uis::Label label = rootGroup.findLabel(u8"title");
    REQUIRE(label.isValid());
    CHECK(label.opacity() == doctest::Approx(1.0f));
    label.setOpacity(0.25f);
    CHECK(label.opacity() == doctest::Approx(0.25f));
    label.setOpacity(3.0f);
    CHECK(label.opacity() == doctest::Approx(1.0f)); // clamped

    label.fadeTo(0.0f, 1.0f);
    CHECK(label.opacity() == doctest::Approx(1.0f)); // from where it was
    ctx.BeginFrame(0.5f);
    CHECK(label.opacity() > 0.0f);
    CHECK(label.opacity() < 1.0f); // half way
    ctx.BeginFrame(0.6f);
    CHECK(label.opacity() == doctest::Approx(0.0f));

    label.fadeTo(1.0f, 1.0f);
    ctx.BeginFrame(0.2f);
    label.setOpacity(0.5f);
    ctx.BeginFrame(1.0f);
    CHECK(label.opacity() == doctest::Approx(0.5f)); // the set stopped the fade

    // Zero seconds is a set; a group fades too; a null handle takes nothing.
    label.fadeTo(0.1f, 0.0f);
    CHECK(label.opacity() == doctest::Approx(0.1f));
    uis::ViewGroup group = rootGroup.findGroup(u8"panel");
    REQUIRE(group.isValid());
    group.fadeTo(0.0f, 0.2f);
    ctx.BeginFrame(0.3f);
    CHECK(group.opacity() == doctest::Approx(0.0f));
    uis::Label missing = rootGroup.findLabel(u8"nope");
    missing.setOpacity(0.5f);
    missing.fadeTo(0.5f, 1.0f);
    CHECK(missing.opacity() == doctest::Approx(0.0f));
}

// A minimap marker moves and turns without a relayout: the handle writes the view's post-layout
// transform (translation in pixels, rotation in degrees stored as radians); a null handle takes
// nothing.
TEST_CASE("uiscript.handle: a view is translated and rotated through its transform")
{
    auto group = MakeRef<ui::FrameLayout>(DefaultAllocator());
    auto marker = MakeLabel(u8"marker", u8"^");
    group->AddView(marker.Get());
    uis::ViewGroup g = Group(group.Get());

    uis::Label handle = g.findLabel(u8"marker");
    REQUIRE(handle.isValid());
    CHECK(handle.translation().x == doctest::Approx(0.0f));
    CHECK(handle.rotation() == doctest::Approx(0.0f));

    handle.setTranslation(40.0f, -12.5f);
    handle.setRotation(90.0f);
    CHECK(marker->Transform.Translation.x == doctest::Approx(40.0f));
    CHECK(marker->Transform.Translation.y == doctest::Approx(-12.5f));
    CHECK(marker->Transform.Rotation == doctest::Approx(DegreesToRadians(90.0f)));
    CHECK(handle.translation().y == doctest::Approx(-12.5f));
    CHECK(handle.rotation() == doctest::Approx(90.0f));

    // Every handle type has it: the bare view the generic finder returns moves the same way.
    uis::View bare = g.find(u8"marker");
    REQUIRE(bare.isValid());
    bare.setTranslation(1.0f, 2.0f);
    CHECK(marker->Transform.Translation.x == doctest::Approx(1.0f));

    uis::Label missing = g.findLabel(u8"nope");
    missing.setTranslation(5.0f, 5.0f);
    missing.setRotation(45.0f);
    CHECK(missing.translation().x == doctest::Approx(0.0f));
    CHECK(missing.rotation() == doctest::Approx(0.0f));
}

// A score popping up: it rises, fades and swells at once, each tween on its own property, and a
// new tween of one property replaces only that one.
TEST_CASE("uiscript.handle: views move, scale, turn and pulse on the frame clock, each on its own")
{
    ui::UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800.0f, 600.0f};
    ctx.AddRootView(root.Get());
    auto popup = MakeLabel(u8"popup", u8"+100");
    root->AddView(popup.Get());
    uis::Label label = Group(root.Get()).findLabel(u8"popup");
    REQUIRE(label.isValid());
    CHECK(label.scale() == doctest::Approx(1.0f));

    label.moveTo(0.0f, -40.0f, 1.0f, uis::Ease::Linear);
    label.fadeTo(0.0f, 1.0f, uis::Ease::Linear);
    label.scaleTo(2.0f, 1.0f, uis::Ease::Linear);
    ctx.BeginFrame(0.5f);
    CHECK(label.translation().y == doctest::Approx(-20.0f));
    CHECK(label.opacity() == doctest::Approx(0.5f));
    CHECK(label.scale() == doctest::Approx(1.5f));

    // A new move replaces the running move, from where the view is; the fade and swell go on.
    label.moveTo(100.0f, -20.0f, 0.5f, uis::Ease::Linear);
    ctx.BeginFrame(0.5f);
    CHECK(label.translation().x == doctest::Approx(100.0f));
    CHECK(label.opacity() == doctest::Approx(0.0f));
    CHECK(label.scale() == doctest::Approx(2.0f));

    // A set stops only its own property's tween.
    label.rotateTo(90.0f, 1.0f);
    label.scaleTo(1.0f, 1.0f);
    ctx.BeginFrame(0.25f);
    label.setScale(3.0f);
    ctx.BeginFrame(1.0f);
    CHECK(label.scale() == doctest::Approx(3.0f));
    CHECK(label.rotation() == doctest::Approx(90.0f));

    // A pulse swells out and settles at the normal size, whatever it started from.
    label.pulse(1.5f, 0.4f);
    ctx.BeginFrame(0.2f);
    CHECK(label.scale() == doctest::Approx(1.5f));
    ctx.BeginFrame(0.1f);
    CHECK(label.scale() > 1.0f);
    CHECK(label.scale() < 1.5f);
    ctx.BeginFrame(0.2f);
    CHECK(label.scale() == doctest::Approx(1.0f));

    // Zero seconds is a set; a view in no tree has no clock, so a tween is a set; a null handle
    // takes nothing.
    label.moveTo(7.0f, 8.0f, 0.0f);
    CHECK(label.translation().x == doctest::Approx(7.0f));
    auto loose = MakeRef<ui::FrameLayout>(DefaultAllocator());
    loose->AddView(MakeLabel(u8"loose", u8"x").Get());
    uis::Label unrooted = Group(loose.Get()).findLabel(u8"loose");
    unrooted.scaleTo(2.0f, 1.0f, uis::Ease::OutBack);
    CHECK(unrooted.scale() == doctest::Approx(2.0f));
    uis::Label missing = Group(root.Get()).findLabel(u8"nope");
    missing.moveTo(1.0f, 1.0f, 1.0f);
    missing.pulse(2.0f, 1.0f);
    CHECK(missing.scale() == doctest::Approx(1.0f));
}
