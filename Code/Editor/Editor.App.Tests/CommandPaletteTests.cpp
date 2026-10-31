// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The command palette over a registry and its own markup (Data/Editor/CommandPalette.xml): the rows
// a query yields and their order; the arrow keys moving the lit row (the palette handles them before
// the search field does); Enter running the lit action and closing the palette, or keeping it open
// with a message when the action is disabled; a row's chord and dimmed state; and Escape closing it.

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
    EditorActionDeclaration Declare(StringView id, StringView label, StringView description, StringView menuPath)
    {
        EditorActionDeclaration d;
        d.id = String(id);
        d.label = String(label);
        d.description = String(description);
        d.menuPath = String(menuPath);
        d.execute = [](EditorPage*) {};
        return d;
    }
    String Labels(const app::CommandPalette& palette)
    {
        String out;
        for (const EditorActionDeclaration* row : palette.Shown())
        {
            if (!out.IsEmpty())
                out += u8"|";
            out += row->label.AsView();
        }
        return out;
    }
    RefPtr<gui::Node> LoadPalette()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), u8"Editor/CommandPalette.xml");
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* FindKind(gui::Node& node)
    {
        if (auto* found = Cast<T>(&node))
            return found;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindKind<T>(*node.GetChildAt(i)))
                return found;
        return nullptr;
    }
}

TEST_CASE("command-palette: the rows follow the query - label starts first, label holds next, description or id last; "
          "the lit row moves clamped under the keys; Enter runs it and closes, a disabled one keeps the palette open")
{
    EditorActionRegistry actions;
    u32 saves = 0;
    u32 resets = 0;
    bool dirty = false;
    EditorActionDeclaration save = Declare(u8"file.save", u8"Save", u8"Save the active page", u8"File/Save");
    save.shortcut = EditorShortcut::Of(u8"ctrl-s");
    save.enabled = [&dirty](EditorPage*) { return dirty; };
    save.execute = [&saves](EditorPage*) { ++saves; };
    REQUIRE(actions.Register(Move(save)));
    REQUIRE(actions.Register(Declare(u8"file.saveAs", u8"Save As...", u8"Save the active page under a new name", u8"File/Save As...")));
    EditorActionDeclaration reset = Declare(u8"view.resetLayout", u8"Reset Layout", u8"Reset the panel layout to the default", u8"View/Reset Layout");
    reset.execute = [&resets](EditorPage*) { ++resets; };
    REQUIRE(actions.Register(Move(reset)));
    REQUIRE(actions.Register(Declare(u8"edit.undo", u8"Undo", u8"Take the last edit back", u8"Edit/Undo")));
    REQUIRE(actions.Register(Declare(u8"scene.entity.saveSelection", u8"Duplicate", u8"Copy the selection beside itself", u8"Scene/Entity/Duplicate")));

    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{1000.0f, 700.0f});
    RefPtr<gui::Node> content = LoadPalette();
    REQUIRE(content.Get() != nullptr);
    app::CommandPalette palette(DefaultAllocator(), actions, content);
    REQUIRE(palette.Open(*root));
    CHECK(palette.IsOpen());
    gui::TextField* field = FindKind<gui::TextField>(*content);
    REQUIRE(field != nullptr);
    CHECK(field->IsFocused());

    // No query: everything, in registration order, the first row lit; a row shows its chord.
    CHECK(Labels(palette) == String(u8"Save|Save As...|Reset Layout|Undo|Duplicate"));
    CHECK(palette.Lit() == 0);
    auto* first = FindKind<app::CommandPaletteRow>(*content);
    REQUIRE(first != nullptr);
    CHECK(first->HasClass(u8"current"));
    CHECK(first->HasClass(u8"disabled")); // Save: nothing to save
    bool chordShown = false;
    for (usize i = 0; i < first->ChildCount(); ++i)
        if (auto* label = Cast<gui::Label>(first->GetChildAt(i)); label != nullptr && label->HasClass(u8"key"))
            chordShown = label->GetText() == FormatShortcut(EditorShortcut::Of(u8"ctrl-s")).AsView();
    CHECK(chordShown);

    // "sav": the labels starting with it first, then the one whose id holds it (Duplicate's id
    // has "save"), never Undo or Reset Layout.
    palette.SetQuery(u8"sav");
    CHECK(Labels(palette) == String(u8"Save|Save As...|Duplicate"));
    palette.SetQuery(u8"LAYOUT");
    CHECK(Labels(palette) == String(u8"Reset Layout"));
    palette.SetQuery(u8"last edit"); // a description alone
    CHECK(Labels(palette) == String(u8"Undo"));
    palette.SetQuery(u8"zzz");
    CHECK(palette.Shown().IsEmpty());
    CHECK(palette.Lit() == -1);
    CHECK(palette.RunLit().Code() == ErrorCode::NotFound);
    CHECK(palette.Hint() == StringView(u8"Nothing matches zzz."));

    // The arrow keys move the lit row, clamped at the ends. The search field has the focus, but the
    // palette handles the keys first.
    palette.SetQuery({});
    gui::EventDispatcher& d = *root->GetEventDispatcher();
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Down));
    CHECK(palette.Lit() == 1);
    palette.Step(10);
    CHECK(palette.Lit() == 4);
    for (int i = 0; i < 9; ++i)
        d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Up));
    CHECK(palette.Lit() == 0);

    // Enter on a disabled action refuses and keeps the palette open, saying so.
    palette.SetQuery(u8"sav");
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Return));
    CHECK(saves == 0u);
    CHECK(palette.IsOpen());
    CHECK(palette.Hint() == StringView(u8"Save is not available now."));
    // Enabled: it runs and the palette closes.
    dirty = true;
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Return));
    CHECK(saves == 1u);
    CHECK_FALSE(palette.IsOpen());

    // Opened again, the query is empty; a row runs by its index; Escape closes the palette.
    REQUIRE(palette.Open(*root));
    CHECK(Labels(palette) == String(u8"Save|Save As...|Reset Layout|Undo|Duplicate"));
    CHECK(palette.Run(2).IsOk());
    CHECK(resets == 1u);
    REQUIRE(palette.Open(*root));
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::Escape));
    CHECK_FALSE(palette.IsOpen());
}
