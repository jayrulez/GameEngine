// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the keys generated from the action registry: one binding per effective
// chord and alternate, run by the root through the registry, bound again on a rebind and on a
// registration; a focused widget's own binding of the same keys first; and a set that dies
// leaving the keymap and the root as they were.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    EditorActionDeclaration Declare(StringView id, StringView label, StringView keys)
    {
        EditorActionDeclaration d;
        d.id = String(id);
        d.label = String(label);
        d.shortcut = EditorShortcut::Of(keys);
        d.execute = [](EditorPage*) {};
        return d;
    }

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        RefPtr<gui::Panel> frame = MakeRef<gui::Panel>(DefaultAllocator());
        Bed()
        {
            root->SetSize(Float2{800.0f, 600.0f});
            root->AddChild(frame.Get());
        }
        [[nodiscard]] gui::Keymap& Keymap() { return root->GetContext().GetKeymap(); }
        bool Press(gui::KeyCode key, u32 modifiers = 0)
        {
            return root->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(key), modifiers);
        }
    };
}

TEST_CASE("action-shortcuts: one binding per effective chord and alternate, run through the registry; bound again on a "
          "rebind and on a registration")
{
    EditorActionRegistry actions;
    u32 saves = 0;
    u32 redos = 0;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"ctrl-s");
    save.execute = [&saves](EditorPage*) { ++saves; };
    REQUIRE(actions.Register(Move(save)));
    EditorActionDeclaration redo = Declare(u8"edit.redo", u8"Redo", u8"ctrl-shift-z");
    redo.alternateShortcut = EditorShortcut::Of(u8"ctrl-y");
    redo.execute = [&redos](EditorPage*) { ++redos; };
    REQUIRE(actions.Register(Move(redo)));
    REQUIRE(actions.Register(Declare(u8"view.reset", u8"Reset Layout", u8""))); // no chord

    Bed bed;
    app::ActionShortcuts shortcuts(DefaultAllocator(), bed.Keymap(), *bed.root, actions);
    CHECK(shortcuts.BoundCount() == 3u); // save, redo, redo's alternate
    bed.Press(gui::KeyCode::S, gui::KeyModCtrl);
    CHECK(saves == 1u);
    bed.Press(gui::KeyCode::Y, gui::KeyModCtrl);
    CHECK(redos == 1u);
    bed.Press(gui::KeyCode::Z, gui::KeyModCtrl | gui::KeyModShift);
    CHECK(redos == 2u);
    bed.Press(gui::KeyCode::S);
    CHECK(saves == 1u); // S alone runs nothing

    // A rebind moves the binding: the old chord is free, the new one runs it.
    REQUIRE(actions.Rebind(u8"file.save", EditorShortcut::Of(u8"f2")).IsOk());
    CHECK(shortcuts.BoundCount() == 3u);
    bed.Press(gui::KeyCode::S, gui::KeyModCtrl);
    CHECK(saves == 1u);
    bed.Press(gui::KeyCode::F2);
    CHECK(saves == 2u);
    // Clearing an override unbinds; a registration with a chord binds.
    REQUIRE(actions.Rebind(u8"file.save", EditorShortcut{}).IsOk());
    CHECK(shortcuts.BoundCount() == 2u);
    bed.Press(gui::KeyCode::F2);
    CHECK(saves == 2u);
    EditorActionDeclaration run = Declare(u8"sim.run", u8"Simulate", u8"f5");
    u32 runs = 0;
    run.execute = [&runs](EditorPage*) { ++runs; };
    REQUIRE(actions.Register(Move(run)));
    CHECK(shortcuts.BoundCount() == 3u);
    bed.Press(gui::KeyCode::F5);
    CHECK(runs == 1u);
}

TEST_CASE("action-shortcuts: a focused widget's own binding of the same keys runs first")
{
    EditorActionRegistry actions;
    u32 undos = 0;
    EditorActionDeclaration undo = Declare(u8"edit.undo", u8"Undo", u8"ctrl-z");
    undo.execute = [&undos](EditorPage*) { ++undos; };
    REQUIRE(actions.Register(Move(undo)));
    Bed bed;
    app::ActionShortcuts shortcuts(DefaultAllocator(), bed.Keymap(), *bed.root, actions);
    // A field of its own context with its own undo on the same keys.
    RefPtr<gui::Button> field = MakeRef<gui::Button>(DefaultAllocator());
    field->SetKeyContext(u8"field");
    u32 own = 0;
    (void)field->OnCommand(u8"field.undo", [&own](gui::CommandEvent&) { ++own; });
    REQUIRE(bed.Keymap().Bind(u8"ctrl-z", u8"field.undo", u8"field"));
    bed.frame->AddChild(field.Get());
    field->RequestFocus();
    bed.Press(gui::KeyCode::Z, gui::KeyModCtrl);
    CHECK(own == 1u);
    CHECK(undos == 0u);
    bed.root->GetEventDispatcher()->SetFocusNode(nullptr);
    bed.Press(gui::KeyCode::Z, gui::KeyModCtrl);
    CHECK(undos == 1u);
}

TEST_CASE("action-shortcuts: a set that dies takes its bindings and handlers; the registry no longer reaches it")
{
    EditorActionRegistry actions;
    u32 saves = 0;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"ctrl-s");
    save.execute = [&saves](EditorPage*) { ++saves; };
    REQUIRE(actions.Register(Move(save)));
    Bed bed;
    const usize before = bed.Keymap().BindingCount();
    {
        app::ActionShortcuts shortcuts(DefaultAllocator(), bed.Keymap(), *bed.root, actions);
        CHECK(bed.Keymap().BindingCount() == before + 1u);
        CHECK(bed.root->HandlesCommand(u8"file.save"));
    }
    CHECK(bed.Keymap().BindingCount() == before);
    CHECK_FALSE(bed.root->HandlesCommand(u8"file.save"));
    bed.Press(gui::KeyCode::S, gui::KeyModCtrl);
    CHECK(saves == 0u);
    // A registration after it is gone reaches nobody.
    REQUIRE(actions.Register(Declare(u8"file.exit", u8"Exit", u8"ctrl-q")));
    CHECK(bed.Keymap().BindingCount() == before);
}
