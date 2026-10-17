// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - modal Window + MessageBox tests: OpenModal adds a scrim and confines input to
// the dialog (background clicks are swallowed by the dispatcher's modal root); MessageBox shows
// the right buttons and reports the pressed result, then closes.
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

    // Sum of a node's positions up to the root, plus half its size = its world-space center
    // (identity transforms, matching the popup/menu model). Used to click a nested button.
    core::Float2 WorldCenter(Node* node)
    {
        core::Float2 p{0.0f, 0.0f};
        for (Node* c = node; c != nullptr && c->GetParent() != nullptr; c = c->GetParent())
            p += c->GetPosition();
        const core::Float2 s = node->GetSize();
        return core::Float2{p.x + s.x * 0.5f, p.y + s.y * 0.5f};
    }
}

TEST_CASE("window: OpenModal centers it as a modal popup of its owner")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    auto window = Make<Window>();
    window->SetSize(core::Float2{200.0f, 100.0f});
    window->OpenModal(*root.Get());

    CHECK(window->IsOpen());
    CHECK(window->IsModal());
    CHECK(window->GetParent() == root.Get());
    CHECK(d->TopPopup() == window.Get());
    CHECK(window->GetPosition().x == doctest::Approx(100.0f)); // (400-200)/2
    CHECK(window->GetPosition().y == doctest::Approx(150.0f)); // (400-100)/2
    CHECK(root->ChildCount() == 1);                            // the window (its backdrop is the top layer's)
}

TEST_CASE("window: a modal swallows clicks on the background")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    int backgroundClicks = 0;
    auto background = Make<Button>();
    background->SetSize(core::Float2{100.0f, 40.0f});
    background->SetPosition(core::Float2{0.0f, 0.0f}); // top-left corner
    background->SetOnClick([&] { ++backgroundClicks; });
    root->AddChild(background.Get());

    auto window = Make<Window>();
    window->SetSize(core::Float2{200.0f, 100.0f}); // centered -> covers (100,150)-(300,250)
    window->OpenModal(*root.Get());

    // Click the background button (outside the modal) -> swallowed.
    d->InjectMouseDown(core::Float2{50.0f, 20.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{50.0f, 20.0f}, MouseButton::Left);
    CHECK(backgroundClicks == 0);
    CHECK(d->GetFocusNode() != background.Get());
}

TEST_CASE("window: Close takes the modal popup down and fires onClose")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    int closed = 0;
    auto window = Make<Window>();
    window->SetSize(core::Float2{200.0f, 100.0f});
    window->SetOnClose([&] { ++closed; });
    window->OpenModal(*root.Get());
    REQUIRE(root->ChildCount() == 1);

    window->Close();
    CHECK_FALSE(window->IsOpen());
    CHECK(window->GetParent() == nullptr);
    CHECK(d->PopupCount() == 0u);
    CHECK(root->ChildCount() == 0); // the window removed (its backdrop is the top layer's)
    CHECK(closed == 1);
}

TEST_CASE("window: non-modal Open adds no scrim and no modal root")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    auto window = Make<Window>();
    window->SetSize(core::Float2{200.0f, 100.0f});
    window->Open(*root.Get());
    CHECK(window->IsOpen());
    CHECK_FALSE(window->IsModal());
    CHECK(d->PopupCount() == 0u);
    CHECK(root->ChildCount() == 1); // just the window
}

TEST_CASE("messagebox: Configure shows the right buttons per style")
{
    auto ok = Make<MessageBox>();
    ok->Configure(core::StringView(u8"Hi"), core::StringView(u8"All good."),
                  MessageBox::Buttons::Ok);
    CHECK(ok->ButtonCount() == 1);
    CHECK(ok->GetMessage() == core::StringView(u8"All good."));

    auto okCancel = Make<MessageBox>();
    okCancel->Configure(core::StringView(u8"?"), core::StringView(u8"Proceed?"),
                        MessageBox::Buttons::OkCancel);
    CHECK(okCancel->ButtonCount() == 2);

    auto yesNo = Make<MessageBox>();
    yesNo->Configure(core::StringView(u8"?"), core::StringView(u8"Save?"),
                     MessageBox::Buttons::YesNo);
    CHECK(yesNo->ButtonCount() == 2);
}

TEST_CASE("messagebox: pressing a button reports the result and closes")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{500.0f, 500.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    MessageBox::Result got = MessageBox::Result::Cancel;
    int results = 0;
    auto box = Make<MessageBox>();
    box->Configure(core::StringView(u8"Confirm"), core::StringView(u8"Delete it?"),
                   MessageBox::Buttons::OkCancel);
    box->SetOnResult(
        [&](MessageBox::Result r)
        {
            got = r;
            ++results;
        });
    box->OpenModal(*root.Get());

    // Buttons: [Cancel, OK] (OK rightmost). Click OK.
    Button* okButton = box->ButtonAt(1);
    REQUIRE(okButton != nullptr);
    const core::Float2 center = WorldCenter(okButton);
    d->InjectMouseDown(center, MouseButton::Left);
    d->InjectMouseUp(center, MouseButton::Left);

    CHECK(results == 1);
    CHECK(got == MessageBox::Result::Ok);
    CHECK_FALSE(box->IsOpen()); // closed after the result
    CHECK(d->PopupCount() == 0u);
}
