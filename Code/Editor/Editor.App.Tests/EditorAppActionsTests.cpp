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

namespace
{
    // A page that saves into a count, as a page writes its asset.
    class CookedPage final : public EditorPage
    {
    public:
        CookedPage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"Crate"; }
        [[nodiscard]] Status Save() override
        {
            ++saves;
            ClearDirty();
            return Status{};
        }
        void Edit() { MarkDirty(); }
        void Show(const Guid& id) { SetInstanceId(id); }
        int saves = 0;
    };
}

TEST_CASE("app actions: an asset page's Cook and Rebuild, off with no project or no asset; each runs over the page it is given")
{
    EditorActionRegistry actions;
    EditorContext context{DefaultAllocator()};
    Array<String> ran;
    app::BuildActions build;
    build.context = &context;
    build.cookAll = []() {};
    build.rebuildAll = []() {};
    build.cookPage = [&ran](EditorPage& page) { ran.PushBack(Format(u8"cook {}", page.Title())); };
    build.rebuildPage = [&ran](EditorPage& page) { ran.PushBack(Format(u8"rebuild {}", page.Title())); };
    app::RegisterBuildActions(actions, Move(build));
    REQUIRE(actions.Find(u8"build.cookPage") != nullptr);
    REQUIRE(actions.Find(u8"build.rebuildPage") != nullptr);
    CHECK(actions.Find(u8"build.cookPage")->menuPath == StringView(u8"Build/Cook This Asset"));

    CookedPage page;
    CHECK_FALSE(actions.IsEnabled(u8"build.cookPage", &page));
    CHECK(actions.DisabledReason(u8"build.cookPage", &page) == StringView(u8"Open a project first"));

    const String dir(u8"scratch_app_actions_cook_project");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    context.SetProject(project.Get());
    CHECK_FALSE(actions.IsEnabled(u8"build.cookPage", &page)); // a page with no asset (a Game tab)
    CHECK(actions.DisabledReason(u8"build.cookPage", &page) == StringView(u8"This page shows no asset"));
    page.Show(Guid{0x5, 0x6});
    CHECK(actions.IsEnabled(u8"build.cookPage", &page));
    CHECK(actions.Execute(u8"build.cookPage", &page).IsOk());
    CHECK(actions.Execute(u8"build.rebuildPage", &page).IsOk());
    REQUIRE(ran.Size() == 2u);
    CHECK(ran[0] == StringView(u8"cook Crate"));
    CHECK(ran[1] == StringView(u8"rebuild Crate"));
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir.AsView());
}
