// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - the keyboard's breadth (E12 of Specs/gui-lab-evolution-2.md): tab-index, focus groups,
// Tab captured by a widget, a menu's keys through the keymap, the next key captured, mnemonics and
// type-ahead from text input.
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

    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::Array<core::RefPtr<Button>> buttons;
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        Button& Add(const char8_t* id, Node* parent = nullptr)
        {
            core::RefPtr<Button> b = Make<Button>();
            b->SetId(SV(id));
            b->SetSize(core::Float2{40.0f, 20.0f});
            (parent != nullptr ? parent : static_cast<Node*>(root.Get()))->AddChild(b.Get());
            buttons.PushBack(b);
            return *b;
        }
        static core::StringView IdOf(Node* n)
        {
            const UIWidget* w = core::Cast<UIWidget>(n);
            return w != nullptr ? w->GetId() : core::StringView{};
        }
        core::String Focused() { return core::String(IdOf(D().GetFocusNode())); }
        void Tab(bool back = false) { D().InjectKeyDown(static_cast<core::u32>(KeyCode::Tab), back ? KeyModShift : 0u); }
    };
}

TEST_CASE("keyboard: CaptureNextKey takes the next key before bindings and widgets, with its modifiers, and drops its text")
{
    Bed bed;
    core::RefPtr<TextField> field = Make<TextField>();
    field->SetSize(core::Float2{200.0f, 24.0f});
    bed.root->AddChild(field.Get());
    bed.D().SetFocusNode(field.Get());
    int quits = 0;
    (void)bed.root->OnCommand(SV(u8"quit"), [&](CommandEvent&) { ++quits; });
    REQUIRE(bed.root->GetContext().GetKeymap().Bind(SV(u8"ctrl-q"), SV(u8"quit")));

    core::Array<Keystroke> taken;
    int cancels = 0;
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); }, [&] { ++cancels; });
    CHECK(bed.D().IsCapturingKey());
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::LeftCtrl), KeyModCtrl); // waits: maybe Ctrl+something
    CHECK(taken.IsEmpty());
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Q), KeyModCtrl);
    REQUIRE(taken.Size() == 1u);
    CHECK(taken[0] == ParseKeystroke(SV(u8"ctrl-q")).Value());
    CHECK(quits == 0); // the binding never saw it
    CHECK_FALSE(bed.D().IsCapturingKey());

    // A plain key's text is dropped; the field gets nothing.
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); });
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::W), 0);
    bed.D().InjectText(SV(u8"w"));
    CHECK(taken.Size() == 2u);
    CHECK(field->GetText().IsEmpty());
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::E), 0); // capture over: as ever
    bed.D().InjectText(SV(u8"e"));
    CHECK(field->GetText() == SV(u8"e"));

    // A modifier alone, taken as it is released.
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); });
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::LeftShift), KeyModShift);
    bed.D().InjectKeyUp(static_cast<core::u32>(KeyCode::LeftShift), 0);
    REQUIRE(taken.Size() == 3u);
    CHECK(taken[2] == ParseKeystroke(SV(u8"leftshift")).Value());

    // A pad button.
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); });
    bed.D().InjectPadButton(PadButton::North);
    REQUIRE(taken.Size() == 4u);
    CHECK(taken[3] == ParseKeystroke(SV(u8"pad-north")).Value());
    CHECK(cancels == 0);
}

TEST_CASE("keyboard: Escape cancels a capture, unless the caller takes it")
{
    Bed bed;
    core::Array<Keystroke> taken;
    int cancels = 0;
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); }, [&] { ++cancels; });
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape), 0);
    CHECK(cancels == 1);
    CHECK(taken.IsEmpty());
    CHECK_FALSE(bed.D().IsCapturingKey());
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); }, [&] { ++cancels; }, true);
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape), 0);
    REQUIRE(taken.Size() == 1u);
    CHECK(taken[0].Code == static_cast<core::u32>(KeyCode::Escape));
    CHECK(cancels == 1);
    // A second capture replaces a waiting one, which is cancelled.
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); }, [&] { ++cancels; });
    bed.D().CaptureNextKey([&](const Keystroke& k) { taken.PushBack(k); });
    CHECK(cancels == 2);
}

TEST_CASE("keyboard: type-ahead reads typed text, so it follows the layout and an IME: a Cyrillic list types ahead")
{
    Bed bed;
    core::RefPtr<ComboBox> combo = Make<ComboBox>();
    combo->SetSize(core::Float2{140.0f, 26.0f});
    combo->SetItemsText(core::String(SV(u8"Яблоко|Груша|Арбуз")));
    combo->SetSelectedIndex(0);
    bed.root->AddChild(combo.Get());
    combo->RequestFocus();
    CHECK(bed.D().WantsTextInput()); // it asks for typed text
    // A Russian layout's key types a Cyrillic letter; its key code is a Latin letter's.
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::U), 0);
    bed.D().InjectText(SV(u8"г"));
    CHECK(combo->GetSelectedIndex() == 1);
    // A binding of a bare letter does not take it from under the type-ahead.
    int ran = 0;
    (void)bed.root->OnCommand(SV(u8"jump"), [&](CommandEvent&) { ++ran; });
    REQUIRE(bed.root->GetContext().GetKeymap().Bind(SV(u8"f"), SV(u8"jump")));
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::F), 0);
    bed.D().InjectText(SV(u8"а"));
    CHECK(ran == 0);
    CHECK(combo->GetSelectedIndex() == 2);
}

TEST_CASE("keyboard: type-ahead matches letters in either case in Latin, Greek and Cyrillic")
{
    CHECK(StartsWithIgnoringCase(SV(u8"Banana"), SV(u8"ba")));
    CHECK(StartsWithIgnoringCase(SV(u8"Éclair"), SV(u8"é")));       // É, é
    CHECK(StartsWithIgnoringCase(SV(u8"Łódź"), SV(u8"ł"))); // Ł, ł
    CHECK(StartsWithIgnoringCase(SV(u8"Δέλτα"), SV(u8"δ"))); // Δ, δ
    CHECK(StartsWithIgnoringCase(SV(u8"Груша"), SV(u8"гр"))); // Гр, гр
    CHECK(StartsWithIgnoringCase(SV(u8"Ёлка"), SV(u8"ё"))); // Ё, ё
    CHECK_FALSE(StartsWithIgnoringCase(SV(u8"Apple"), SV(u8"b")));
    CHECK_FALSE(StartsWithIgnoringCase(SV(u8"A"), SV(u8"ab")));
}
