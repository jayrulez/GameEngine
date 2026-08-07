// Draconic GUI - modal Window + MessageBox tests: OpenModal adds a scrim and confines input to
// the dialog (background clicks are swallowed by the dispatcher's modal root); MessageBox shows
// the right buttons and reports the pressed result, then closes.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }

    // Sum of a node's positions up to the root, plus half its size = its world-space center
    // (identity transforms, matching the popup/menu model). Used to click a nested button.
    foundation::Float2 WorldCenter(Node* node)
    {
        foundation::Float2 p{0.0f, 0.0f};
        for (Node* c = node; c != nullptr && c->GetParent() != nullptr; c = c->GetParent())
            p += c->GetPosition();
        const foundation::Float2 s = node->GetSize();
        return foundation::Float2{p.x + s.x * 0.5f, p.y + s.y * 0.5f};
    }
}

TEST_CASE("window: OpenModal centers, adds a scrim, and sets the dispatcher modal root")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    auto window = Make<Window>();
    window->SetSize(foundation::Float2{200.0f, 100.0f});
    window->OpenModal(*root.Get());

    CHECK(window->IsOpen());
    CHECK(window->IsModal());
    CHECK(window->GetParent() == root.Get());
    CHECK(d->GetModalRoot() == window.Get());
    CHECK(window->GetPosition().x == doctest::Approx(100.0f)); // (400-200)/2
    CHECK(window->GetPosition().y == doctest::Approx(150.0f)); // (400-100)/2
    CHECK(root->ChildCount() == 2);                            // scrim + window
}

TEST_CASE("window: a modal swallows clicks on the background")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    int backgroundClicks = 0;
    auto background = Make<Button>();
    background->SetSize(foundation::Float2{100.0f, 40.0f});
    background->SetPosition(foundation::Float2{0.0f, 0.0f}); // top-left corner
    background->SetOnClick([&] { ++backgroundClicks; });
    root->AddChild(background.Get());

    auto window = Make<Window>();
    window->SetSize(foundation::Float2{200.0f, 100.0f}); // centered -> covers (100,150)-(300,250)
    window->OpenModal(*root.Get());

    // Click the background button (outside the modal) -> swallowed.
    d->InjectMouseDown(foundation::Float2{50.0f, 20.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{50.0f, 20.0f}, MouseButton::Left);
    CHECK(backgroundClicks == 0);
    CHECK(d->GetFocusNode() != background.Get());
}

TEST_CASE("window: Close removes the window + scrim, releases the modal, and fires onClose")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    int closed = 0;
    auto window = Make<Window>();
    window->SetSize(foundation::Float2{200.0f, 100.0f});
    window->SetOnClose([&] { ++closed; });
    window->OpenModal(*root.Get());
    REQUIRE(root->ChildCount() == 2);

    window->Close();
    CHECK_FALSE(window->IsOpen());
    CHECK(window->GetParent() == nullptr);
    CHECK(d->GetModalRoot() == nullptr);
    CHECK(root->ChildCount() == 0); // window + scrim both removed
    CHECK(closed == 1);
}

TEST_CASE("window: non-modal Open adds no scrim and no modal root")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    auto window = Make<Window>();
    window->SetSize(foundation::Float2{200.0f, 100.0f});
    window->Open(*root.Get());
    CHECK(window->IsOpen());
    CHECK_FALSE(window->IsModal());
    CHECK(d->GetModalRoot() == nullptr);
    CHECK(root->ChildCount() == 1); // just the window
}

TEST_CASE("messagebox: Configure shows the right buttons per style")
{
    auto ok = Make<MessageBox>();
    ok->Configure(foundation::StringView(u8"Hi"), foundation::StringView(u8"All good."),
                  MessageBox::Buttons::Ok);
    CHECK(ok->ButtonCount() == 1);
    CHECK(ok->GetMessage() == foundation::StringView(u8"All good."));

    auto okCancel = Make<MessageBox>();
    okCancel->Configure(foundation::StringView(u8"?"), foundation::StringView(u8"Proceed?"),
                        MessageBox::Buttons::OkCancel);
    CHECK(okCancel->ButtonCount() == 2);

    auto yesNo = Make<MessageBox>();
    yesNo->Configure(foundation::StringView(u8"?"), foundation::StringView(u8"Save?"),
                     MessageBox::Buttons::YesNo);
    CHECK(yesNo->ButtonCount() == 2);
}

TEST_CASE("messagebox: pressing a button reports the result and closes")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{500.0f, 500.0f});
    EventDispatcher* d = root->GetEventDispatcher();

    MessageBox::Result got = MessageBox::Result::Cancel;
    int results = 0;
    auto box = Make<MessageBox>();
    box->Configure(foundation::StringView(u8"Confirm"), foundation::StringView(u8"Delete it?"),
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
    const foundation::Float2 center = WorldCenter(okButton);
    d->InjectMouseDown(center, MouseButton::Left);
    d->InjectMouseUp(center, MouseButton::Left);

    CHECK(results == 1);
    CHECK(got == MessageBox::Result::Ok);
    CHECK_FALSE(box->IsOpen()); // closed after the result
    CHECK(d->GetModalRoot() == nullptr);
}
