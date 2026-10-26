// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The chord capture button: a click begins a capture, its focus taking every key; the next key not
// a modifier is the chord, with the modifiers held; Escape cancels, Delete chooses none, a modifier
// alone keeps waiting, and losing focus cancels. A chord a binding holds goes to the button while
// it waits, not to the binding.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

TEST_CASE("shortcut-capture: click, press, chosen - with the cancel, clear, modifier-alone and focus-lost rules")
{
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{400.0f, 300.0f});
    gui::EventDispatcher& d = *root->GetEventDispatcher();
    RefPtr<app::ShortcutCaptureButton> button = MakeRef<app::ShortcutCaptureButton>(DefaultAllocator());
    RefPtr<gui::Button> other = MakeRef<gui::Button>(DefaultAllocator());
    root->AddChild(button.Get());
    root->AddChild(other.Get());
    button->SetChord(EditorShortcut::Of(u8"ctrl-s"));
    CHECK(button->GetText() == FormatShortcut(EditorShortcut::Of(u8"ctrl-s")).AsView());
    CHECK_FALSE(button->IsCapturing());
    Array<EditorShortcut> chosen;
    button->SetOnChordChosen([&chosen](EditorShortcut chord) { chosen.PushBack(chord); });
    // A binding of the keys the user will press: while the button waits, it does not run.
    int bound = 0;
    REQUIRE(root->GetContext().GetKeymap().Bind(u8"ctrl-shift-k", u8"test.bound"));
    (void)root->OnCommand(u8"test.bound", [&bound](gui::CommandEvent&) { ++bound; });

    CHECK(button->HandleActivate()); // a click (or Return): it waits
    CHECK(button->IsCapturing());
    CHECK(button->IsFocused());
    CHECK(button->HasClass(u8"capturing"));

    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::LeftCtrl), gui::KeyModCtrl); // a modifier alone
    CHECK(button->IsCapturing());
    CHECK(chosen.IsEmpty());
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::K), gui::KeyModCtrl | gui::KeyModShift);
    CHECK_FALSE(button->IsCapturing());
    REQUIRE(chosen.Size() == 1u);
    CHECK(chosen[0] == EditorShortcut::Of(u8"ctrl-shift-k"));
    CHECK(bound == 0); // the button's, not the binding's
    CHECK(button->GetText() == FormatShortcut(EditorShortcut::Of(u8"ctrl-shift-k")).AsView());

    // Escape cancels: the chord stays.
    CHECK(button->HandleActivate());
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Escape));
    CHECK_FALSE(button->IsCapturing());
    CHECK(chosen.Size() == 1u);
    CHECK(button->Chord() == EditorShortcut::Of(u8"ctrl-shift-k"));

    // Delete chooses none.
    CHECK(button->HandleActivate());
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Delete));
    REQUIRE(chosen.Size() == 2u);
    CHECK_FALSE(chosen[1].IsSet());
    CHECK(button->GetText() == StringView(u8"Not set"));

    // The focus leaving cancels; the keys are the GUI's again.
    CHECK(button->HandleActivate());
    other->RequestFocus();
    CHECK_FALSE(button->IsCapturing());
    CHECK_FALSE(button->HasClass(u8"capturing"));
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::K), gui::KeyModCtrl | gui::KeyModShift);
    CHECK(bound == 1);
    CHECK(chosen.Size() == 2u);
}
