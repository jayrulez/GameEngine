// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The asset slot: a verb shows once wired (open, reveal and clear only with something referenced), an
// empty slot says None, each verb runs once per click, a click on the slot picks, and a dragged
// asset of a type it takes is dropped on it while one it does not take is refused, saying why.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.app;

using namespace foundation::core;
using editor::app::AssetDrag;
using editor::app::AssetPickerSlot;
namespace gui = foundation::gui;

TEST_CASE("asset-slot: the verbs show once wired; open, reveal and clear only with something referenced; empty says None")
{
    RefPtr<AssetPickerSlot> slot = MakeRef<AssetPickerSlot>(DefaultAllocator());
    CHECK(slot->ShownName() == StringView(u8"Not set"));
    CHECK(slot->HasClass(u8"empty"));
    CHECK_FALSE(slot->PickButton()->IsDisplayed());
    int picks = 0;
    int opens = 0;
    int reveals = 0;
    int clears = 0;
    slot->SetOnPick([&picks]() { ++picks; });
    slot->SetOnOpen([&opens]() { ++opens; });
    slot->SetOnReveal([&reveals]() { ++reveals; });
    slot->SetOnClear([&clears]() { ++clears; });
    CHECK(slot->PickButton()->IsDisplayed());
    CHECK_FALSE(slot->OpenButton()->IsDisplayed()); // nothing to open yet
    CHECK_FALSE(slot->RevealButton()->IsDisplayed());
    CHECK_FALSE(slot->ClearButton()->IsDisplayed());

    slot->SetValue(u8"Wood");
    CHECK_FALSE(slot->HasClass(u8"empty"));
    CHECK(slot->ShownName() == StringView(u8"Wood"));
    CHECK(slot->OpenButton()->IsDisplayed());
    CHECK(slot->RevealButton()->IsDisplayed());
    CHECK(slot->ClearButton()->IsDisplayed());

    CHECK(slot->PickButton()->HandleActivate());
    CHECK(slot->OpenButton()->HandleActivate());
    CHECK(slot->RevealButton()->HandleActivate());
    CHECK(slot->ClearButton()->HandleActivate());
    CHECK(picks == 1);
    CHECK(opens == 1);
    CHECK(reveals == 1);
    CHECK(clears == 1);
}

TEST_CASE("asset-slot: a dragged asset of a type it takes drops on it; another type is refused, saying why")
{
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{400.0f, 200.0f});
    RefPtr<AssetPickerSlot> slot = MakeRef<AssetPickerSlot>(DefaultAllocator());
    slot->EditLayoutStyle([](gui::LayoutStyle& s)
    {
        s.Width = gui::LayoutLength::Px(200.0f);
        s.Height = gui::LayoutLength::Px(24.0f);
    });
    root->AddChild(slot.Get());
    Array<String> materials;
    materials.PushBack(String(u8"MaterialAsset"));
    slot->SetAcceptedTypes(Move(materials));
    Array<Guid> dropped;
    slot->SetOnDropped([&dropped](const Guid& id) { dropped.PushBack(id); });
    (void)root->UpdateLayout();

    // A source the drags start from.
    RefPtr<gui::Panel> source = MakeRef<gui::Panel>(DefaultAllocator());
    root->AddChild(source.Get());
    (void)root->UpdateLayout();
    gui::EventDispatcher& d = *root->GetEventDispatcher();
    const gui::Rect at = slot->GetScreenBounds();
    const Float2 over{at.x + 20.0f, at.y + 12.0f};
    const auto drag = [&](gui::DragPayload payload)
    {
        gui::DragRequest request;
        request.Payload = Move(payload);
        d.InjectMouseMove(over);
        d.StartDrag(*source, Move(request));
        d.RequeryDrag();
    };

    const Guid wood{0x1234u, 0x5678u};
    drag(AssetDrag::Payload(wood, u8"MaterialAsset", u8"Wood"));
    CHECK(d.GetDropTarget() == slot.Get());
    CHECK(d.GetDropReason() == StringView(u8"Use Wood"));
    d.InjectMouseUp(over, gui::MouseButton::Left);
    REQUIRE(dropped.Size() == 1u);
    CHECK(dropped[0] == wood);

    drag(AssetDrag::Payload(Guid{0x9u, 0x9u}, u8"TextureAsset", u8"Brick"));
    CHECK(d.GetDropReason() == StringView(u8"Brick is a TextureAsset, not one this takes"));
    d.InjectMouseUp(over, gui::MouseButton::Left);
    CHECK(dropped.Size() == 1u); // refused: nothing assigned
}
