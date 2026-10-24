// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - the editor-wide actions: each menu registers what the application can run, with
// its id, menu path and keys; one it gives nothing for is left out; one that needs a project is
// off without one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

TEST_CASE("app actions: a menu registers what can run, with its keys; what needs a project is off without one")
{
    EditorActionRegistry actions;
    int exits = 0;
    app::FileActions file;
    file.exit = [&exits]() { ++exits; };
    app::RegisterFileActions(actions, Move(file));
    CHECK(actions.Find(u8"file.exit") != nullptr);
    CHECK(actions.Find(u8"file.save") == nullptr);      // no save given: left out
    CHECK(actions.Find(u8"file.saveLayout") == nullptr);
    CHECK(actions.Execute(u8"file.exit", nullptr).IsOk());
    CHECK(exits == 1);

    app::RegisterEditActions(actions, app::EditActions{});
    CHECK(actions.Shortcut(u8"edit.undo") == EditorShortcut::Of(u8"ctrl-z"));
    CHECK(actions.AlternateShortcut(u8"edit.redo") == EditorShortcut::Of(u8"ctrl-y"));
    CHECK(actions.Find(u8"edit.preferences") == nullptr);
    CHECK_FALSE(actions.IsEnabled(u8"edit.undo", nullptr)); // no page

    EditorContext context{DefaultAllocator()};
    int reloads = 0;
    app::ProjectActions project;
    project.context = &context;
    project.reloadNativeModule = [&reloads]() { ++reloads; };
    project.reportResourceMemory = []() {};
    app::RegisterProjectActions(actions, Move(project));
    CHECK_FALSE(actions.IsEnabled(u8"project.reloadNativeModule", nullptr)); // no project open
    CHECK_FALSE(actions.Execute(u8"project.reloadNativeModule", nullptr).IsOk());
    CHECK(reloads == 0);
    CHECK(actions.Find(u8"project.reportResourceMemory")->readOnly);

    app::ViewActions view;
    view.screenshot = []() {};
    app::RegisterViewActions(actions, Move(view));
    CHECK(actions.Shortcut(u8"view.screenshot") == EditorShortcut::Of(u8"f12"));
    CHECK(actions.Find(u8"view.commandPalette") == nullptr);
}
