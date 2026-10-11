// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the Project page: the project's scenes (the assets of the default-scene
// setting's type), every reflected setting with its value (an asset by its name), the Game and
// Project menus' actions, and a rebuild when the settings change.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.scene.resource;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

namespace
{
    [[nodiscard]] bool Has(Span<const String> items, StringView item)
    {
        for (const String& entry : items)
        {
            if (entry.AsView() == item)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] EditorActionDeclaration Action(StringView id, StringView label, StringView menuPath, i32 order,
                                                 i32& runs)
    {
        EditorActionDeclaration d;
        d.id = String(id);
        d.label = String(label);
        d.menuPath = String(menuPath);
        d.menuOrder = order;
        d.execute = [&runs](EditorPage*) { ++runs; };
        return d;
    }
}

TEST_CASE("project page: the scenes, the settings and the project's actions")
{
    const StringView dir = u8"scratch_project_home";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Harbour").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    foundation::content::Group& root = *project->SourceDb().RootGroup();
    foundation::content::Instance* docks =
        root.CreateInstance(u8"Docks", foundation::scene::SceneDocument::StaticType());
    foundation::content::Instance* arrival =
        root.CreateInstance(u8"Arrival", foundation::scene::SceneDocument::StaticType());
    REQUIRE(docks != nullptr);
    REQUIRE(arrival != nullptr);
    project->Settings().defaultSceneId = docks->Id();

    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    i32 plays = 0;
    i32 exports = 0;
    i32 others = 0;
    // Registered out of menu order: the page orders them as the menu does.
    REQUIRE(context.Actions().Register(Action(u8"test.export", u8"Export...", u8"Project/Export...", 300, exports)));
    REQUIRE(context.Actions().Register(Action(u8"test.play", u8"Play", u8"Game/Play", 100, plays)));
    REQUIRE(context.Actions().Register(Action(u8"test.reset", u8"Reset Layout", u8"View/Reset Layout", 100, others)));
    REQUIRE(context.Actions().Register(
        Action(u8"test.settings", u8"Project Settings...", u8"Project/Project Settings...", 100, others)));

    app::ProjectHomeView home(DefaultAllocator(), context, nullptr);

    // Every scene, in the asset tree's order (by name).
    REQUIRE(home.Scenes().Size() == 2u);
    CHECK(home.Scenes()[0] == arrival->Id());
    CHECK(home.Scenes()[1] == docks->Id());

    // Every labelled setting, an asset by its name, an unset one by its empty text.
    CHECK(Has(home.SettingRows(), u8"Name: Harbour"));
    CHECK(Has(home.SettingRows(), u8"Default scene: Docks"));
    CHECK(Has(home.SettingRows(), u8"Default bus layout: (built-in)"));
    CHECK(Has(home.SettingRows(), u8"MSAA: Off"));
    CHECK(Has(home.SettingRows(), u8"Window mode: Windowed"));
    CHECK(Has(home.SettingRows(), u8"Window resizable: On"));

    // The Game menu's actions, then the Project menu's, each in menu order; no others.
    REQUIRE(home.ActionIds().Size() == 3u);
    CHECK(home.ActionIds()[0] == StringView(u8"test.play"));
    CHECK(home.ActionIds()[1] == StringView(u8"test.settings"));
    CHECK(home.ActionIds()[2] == StringView(u8"test.export"));

    // A change to the settings shows at the next refresh, once marked.
    project->Settings().defaultSceneId = arrival->Id();
    home.Refresh();
    CHECK(Has(home.SettingRows(), u8"Default scene: Docks"));
    home.MarkChanged();
    home.Refresh();
    CHECK(Has(home.SettingRows(), u8"Default scene: Arrival"));

    // Without a project, nothing to show.
    context.SetProject(nullptr);
    home.Rebuild();
    CHECK(home.Scenes().IsEmpty());
    CHECK(home.SettingRows().IsEmpty());
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
