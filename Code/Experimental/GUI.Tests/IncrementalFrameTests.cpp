// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - incremental frames (G4 of Specs/vg-and-gui-lab.md): a frame restyles only the widgets
// that changed and their subtrees, lays out only the containers that asked, once per frame, and
// an idle GUI needs no frame at all.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
namespace vg = foundation::vg;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }
    core::Duration Sec(double s) { return core::Duration::FromSeconds(s); }

    core::RefPtr<UIWidget> Widget(const char8_t* tag)
    {
        auto w = Make<UIWidget>();
        w->SetTag(SV(tag));
        w->SetSize(core::Float2{40.0f, 20.0f});
        return w;
    }

    // One host frame: styles, ticks, lays out and draws, as a sample does.
    void Frame(SceneRoot& root, StyleManager& styles)
    {
        styles.ApplyTree(root);
        root.Update(Sec(0.016));
        vg::VGContext canvas;
        DrawContext dc{canvas};
        root.Draw(dc);
    }
}

TEST_CASE("incremental: an unchanged frame restyles nothing and needs no frame")
{
    auto root = Make<SceneRoot>();
    root->SetSize(core::Float2{200.0f, 200.0f});
    auto a = Widget(u8"panel");
    auto b = Widget(u8"button");
    root->AddChild(a.Get());
    a->AddChild(b.Get());
    StyleManager styles(CSSParser::Parse(SV(u8"panel { opacity: 0.8; } button { background-color: red; }")));

    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() >= 2u);
    CHECK_FALSE(root->NeedsFrame());

    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() == 0u);
    CHECK(root->LastLayoutCount() == 0u);
    CHECK_FALSE(root->NeedsFrame());
}

TEST_CASE("incremental: a class change restyles only that widget's subtree")
{
    auto root = Make<SceneRoot>();
    auto left = Widget(u8"panel");
    auto leftChild = Widget(u8"button");
    auto right = Widget(u8"panel");
    auto rightChild = Widget(u8"button");
    root->AddChild(left.Get());
    left->AddChild(leftChild.Get());
    root->AddChild(right.Get());
    right->AddChild(rightChild.Get());
    StyleManager styles(CSSParser::Parse(SV(u8".on button { opacity: 0.25; }")));
    Frame(*root.Get(), styles);

    left->AddClass(SV(u8"on"));
    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() == 2u); // left and its child; right's side untouched
    CHECK(leftChild->GetAlpha() == doctest::Approx(0.25f));
    CHECK(rightChild->GetAlpha() == doctest::Approx(1.0f));
}

TEST_CASE("incremental: hover restyles the hovered widget alone")
{
    auto root = Make<SceneRoot>();
    root->SetSize(core::Float2{200.0f, 200.0f});
    auto a = Widget(u8"button");
    auto b = Widget(u8"button");
    b->SetPosition(core::Float2{100.0f, 0.0f});
    root->AddChild(a.Get());
    root->AddChild(b.Get());
    StyleManager styles(CSSParser::Parse(SV(u8"button:hover { opacity: 0.5; }")));
    Frame(*root.Get(), styles);

    EventDispatcher* d = root->GetEventDispatcher();
    d->InjectMouseMove(core::Float2{10.0f, 10.0f});
    CHECK(root->NeedsFrame());
    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() == 1u);
    CHECK(a->GetAlpha() == doctest::Approx(0.5f));

    d->InjectMouseMove(core::Float2{12.0f, 12.0f}); // still over it: nothing to do
    CHECK_FALSE(root->NeedsFrame());
}

TEST_CASE("incremental: a changed custom property restyles the children that read it")
{
    auto root = Make<SceneRoot>();
    root->SetSize(core::Float2{400.0f, 400.0f});
    auto panel = Widget(u8"panel");
    panel->SetSize(core::Float2{200.0f, 200.0f});
    auto button = Widget(u8"button");
    button->SetPosition(core::Float2{100.0f, 100.0f});
    root->AddChild(panel.Get());
    panel->AddChild(button.Get());
    StyleManager styles(CSSParser::Parse(
        SV(u8"panel { --fade: 0.9; } panel.dim { --fade: 0.3; } button { opacity: var(--fade); }")));
    Frame(*root.Get(), styles);
    CHECK(button->GetAlpha() == doctest::Approx(0.9f));

    panel->AddClass(SV(u8"dim")); // marks the panel's subtree
    Frame(*root.Get(), styles);
    CHECK(button->GetAlpha() == doctest::Approx(0.3f));

    // A state change restyles the subtree too (`panel:hover button` reads it), and only that.
    root->GetEventDispatcher()->InjectMouseMove(core::Float2{10.0f, 10.0f}); // over the panel only
    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() == 2u);
    CHECK(button->GetAlpha() == doctest::Approx(0.3f));
}

TEST_CASE("incremental: a sheet change restyles everything once")
{
    auto root = Make<SceneRoot>();
    auto a = Widget(u8"button");
    root->AddChild(a.Get());
    StyleManager styles(CSSParser::Parse(SV(u8"button { opacity: 1; }")));
    Frame(*root.Get(), styles);

    styles.SetStyleSheet(CSSParser::Parse(SV(u8"button { opacity: 0.6; }")));
    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() >= 1u);
    CHECK(a->GetAlpha() == doctest::Approx(0.6f));
    Frame(*root.Get(), styles);
    CHECK(styles.LastRestyleCount() == 0u);
}

TEST_CASE("incremental: many adds to a layout in a scene lay it out once, at the frame")
{
    auto root = Make<SceneRoot>();
    auto column = Make<LinearLayout>();
    column->SetSize(core::Float2{100.0f, 400.0f});
    root->AddChild(column.Get());
    root->UpdateLayout();

    core::Array<core::RefPtr<UIWidget>> rows{core::DefaultAllocator()};
    for (int i = 0; i < 10; ++i)
    {
        auto row = Widget(u8"row");
        column->AddChild(row.Get());
        rows.PushBack(row);
    }
    CHECK(rows[9]->GetPosition().y == doctest::Approx(0.0f)); // deferred: not yet placed
    CHECK(root->NeedsFrame());

    root->UpdateLayout();
    CHECK(root->LastLayoutCount() == 1u);
    CHECK(rows[9]->GetPosition().y == doctest::Approx(180.0f)); // nine 20-high rows above
    root->UpdateLayout();
    CHECK(root->LastLayoutCount() == 0u);
}

TEST_CASE("incremental: a layout outside a scene still lays out at once")
{
    auto column = Make<LinearLayout>();
    column->SetSize(core::Float2{100.0f, 400.0f});
    auto a = Widget(u8"row");
    auto b = Widget(u8"row");
    column->AddChild(a.Get());
    column->AddChild(b.Get());
    CHECK(b->GetPosition().y == doctest::Approx(20.0f));
}

TEST_CASE("incremental: a running animation keeps frames coming until it ends")
{
    auto root = Make<SceneRoot>();
    auto a = Widget(u8"button");
    root->AddChild(a.Get());
    StyleManager styles(CSSParser::Parse(
        SV(u8"button { opacity: 1; transition: opacity 0.1s linear; } button.off { opacity: 0; }")));
    Frame(*root.Get(), styles);
    CHECK_FALSE(root->NeedsFrame());

    a->AddClass(SV(u8"off"));
    Frame(*root.Get(), styles);
    CHECK(root->NeedsFrame()); // the transition runs
    for (int i = 0; i < 20; ++i)
        Frame(*root.Get(), styles);
    CHECK(a->GetAlpha() == doctest::Approx(0.0f));
    CHECK_FALSE(root->NeedsFrame());
}

TEST_CASE("incremental: setting geometry to what it is changes nothing")
{
    auto root = Make<SceneRoot>();
    auto a = Widget(u8"button");
    a->SetPosition(core::Float2{5.0f, 6.0f});
    root->AddChild(a.Get());
    StyleManager styles(CSSParser::Parse(SV(u8"button { opacity: 1; }")));
    Frame(*root.Get(), styles);
    CHECK_FALSE(root->NeedsFrame());

    a->SetPosition(core::Float2{5.0f, 6.0f});
    a->SetSize(core::Float2{40.0f, 20.0f});
    a->SetScale(core::Float2{1.0f, 1.0f});
    a->SetRotation(0.0f);
    CHECK_FALSE(root->NeedsFrame());

    a->SetPosition(core::Float2{7.0f, 6.0f});
    CHECK(root->NeedsFrame());
}

TEST_CASE("incremental: a child resized after a layout lays its container out again, once")
{
    auto root = Make<SceneRoot>();
    auto column = Make<FlexLayout>();
    column->SetDirection(FlexDirection::Column);
    column->SetSize(core::Float2{200.0f, 400.0f});
    root->AddChild(column.Get());
    auto a = Make<UIWidget>(); // unsized when the layout first meets them
    auto b = Make<UIWidget>();
    a->SetTag(SV(u8"item"));
    b->SetTag(SV(u8"item"));
    column->AddChild(a.Get());
    column->AddChild(b.Get());
    root->UpdateLayout();
    CHECK(b->GetPosition().y == doctest::Approx(0.0f)); // both empty, stacked at the top

    // The sheet sizes them afterwards (a frame's restyle): the flex takes the new basis.
    StyleManager styles(CSSParser::Parse(SV(u8"item { width: 100px; height: 40px; }")));
    styles.ApplyTree(*root.Get());
    root->UpdateLayout();
    CHECK(b->GetPosition().y == doctest::Approx(40.0f));
    CHECK(b->GetSize().y == doctest::Approx(40.0f));

    root->UpdateLayout(); // its own writes asked for nothing more
    CHECK(root->LastLayoutCount() == 0u);
}
