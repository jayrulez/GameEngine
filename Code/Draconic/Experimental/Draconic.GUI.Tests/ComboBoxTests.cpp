// Draconic GUI - ComboBox tests: item management, opening/closing the dropdown popup, picking
// an item through the dropdown, and dismissal (outside click / Escape) via the dispatcher's
// popup support.
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

    foundation::RefPtr<ComboBox> MakeCombo(SceneNode* root)
    {
        auto cb = foundation::MakeRef<ComboBox>(foundation::DefaultAllocator());
        cb->SetSize(foundation::Float2{120.0f, 26.0f});
        cb->SetItemHeight(24.0f);
        cb->AddItem(foundation::StringView(u8"Red"));
        cb->AddItem(foundation::StringView(u8"Green"));
        cb->AddItem(foundation::StringView(u8"Blue"));
        root->AddChild(cb.Get());
        return cb;
    }
}

TEST_CASE("combobox: item management and selection")
{
    auto cb = Make<ComboBox>();
    cb->AddItem(foundation::StringView(u8"One"));
    cb->AddItem(foundation::StringView(u8"Two"));
    CHECK(cb->ItemCount() == 2);
    CHECK(cb->GetSelectedIndex() == -1);

    int changes = 0;
    cb->SetOnSelectionChanged([&](int) { ++changes; });
    cb->SetSelectedIndex(1);
    CHECK(cb->GetSelectedIndex() == 1);
    CHECK(cb->GetSelectedText() == foundation::StringView(u8"Two"));
    CHECK(changes == 1);
    cb->SetSelectedIndex(1); // no change
    CHECK(changes == 1);
}

TEST_CASE("combobox: click opens the dropdown; it registers as the dispatcher popup")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    CHECK_FALSE(cb->IsOpen());
    CHECK(d->GetPopup() == nullptr);

    // Click the combo (a press+release on it).
    d->InjectMouseDown(foundation::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{20.0f, 13.0f}, MouseButton::Left);
    CHECK(cb->IsOpen());
    CHECK(d->GetPopup() != nullptr); // the dropdown is the active popup
}

TEST_CASE("combobox: clicking a dropdown row selects it and closes")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{20.0f, 13.0f}, MouseButton::Left); // open
    REQUIRE(cb->IsOpen());

    // The dropdown sits just below the combo (combo is 26 tall at y=0). Row 1 (Green) spans
    // y in [26 + 24, 26 + 48) = [50, 74).
    d->InjectMouseDown(foundation::Float2{20.0f, 60.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{20.0f, 60.0f}, MouseButton::Left);
    CHECK(cb->GetSelectedIndex() == 1);
    CHECK(cb->GetSelectedText() == foundation::StringView(u8"Green"));
    CHECK_FALSE(cb->IsOpen());
    CHECK(root->GetEventDispatcher()->GetPopup() == nullptr);
}

TEST_CASE("combobox: clicking outside dismisses the dropdown")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{20.0f, 13.0f}, MouseButton::Left); // open
    REQUIRE(cb->IsOpen());

    // Press far away from the combo and its dropdown -> dismissed.
    d->InjectMouseDown(foundation::Float2{350.0f, 350.0f}, MouseButton::Left);
    CHECK_FALSE(cb->IsOpen());
    CHECK(d->GetPopup() == nullptr);
}

TEST_CASE("combobox: Escape dismisses the dropdown")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 400.0f});
    auto cb = MakeCombo(root.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{20.0f, 13.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{20.0f, 13.0f}, MouseButton::Left); // open
    REQUIRE(cb->IsOpen());

    d->InjectKeyDown(static_cast<foundation::u32>(KeyCode::Escape));
    CHECK_FALSE(cb->IsOpen());
    CHECK(d->GetPopup() == nullptr);
}
