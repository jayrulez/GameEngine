// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the Project page over its markup (Data/Editor/ProjectHome.xml). Covers the
// project's name, the scenes (assets of the default scene setting's type) as links that open them,
// every reflected setting with its value (an asset shown by its name), the actions of the Game and
// Project menus as buttons, and a rebuild when the settings change.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.scene.resource;
import foundation.gui;
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

    RefPtr<foundation::gui::Node> LoadHome()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(),
                                     StringView(app::kProjectHomeViewPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        foundation::gui::RegisterGuiElements();
        static const foundation::gui::ElementSet elements = foundation::gui::ElementSet::All();
        foundation::gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* FindId(foundation::gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<foundation::gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
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

    RefPtr<foundation::gui::Node> content = LoadHome();
    REQUIRE(content.Get() != nullptr);
    app::ProjectHomeView home(DefaultAllocator(), context, nullptr, content);
    Array<Guid> opened;
    home.SetOnOpenAsset([&opened](const Guid& id) { opened.PushBack(id); });
    CHECK(FindId<foundation::gui::Label>(*content, u8"home-name")->GetText() == StringView(u8"Harbour"));
    auto* openDefault = FindId<foundation::gui::Button>(*content, u8"home-open-default");
    REQUIRE(openDefault != nullptr);
    CHECK(openDefault->GetText() == StringView(u8"Open Docks"));
    CHECK(openDefault->HandleActivate());
    REQUIRE(opened.Size() == 1u);
    CHECK(opened[0] == docks->Id());
    // Each scene is a link that opens it.
    foundation::gui::Node* scenes = FindId<foundation::gui::Node>(*content, u8"home-scenes");
    REQUIRE(scenes != nullptr);
    REQUIRE(scenes->ChildCount() == 2u);
    auto* first = Cast<foundation::gui::Button>(scenes->GetChildAt(0));
    REQUIRE(first != nullptr);
    CHECK(first->GetText() == StringView(u8"Arrival"));
    CHECK(first->HandleActivate());
    CHECK(opened.Back() == arrival->Id());
    // The Game menu's actions as buttons that run them.
    foundation::gui::Node* game = FindId<foundation::gui::Node>(*content, u8"home-game-actions");
    REQUIRE(game != nullptr);
    REQUIRE(game->ChildCount() == 1u);
    CHECK(Cast<foundation::gui::Button>(game->GetChildAt(0))->HandleActivate());
    CHECK(plays == 1);

    // Every scene, in the asset tree's order (by name).
    REQUIRE(home.Scenes().Size() == 2u);
    CHECK(home.Scenes()[0] == arrival->Id());
    CHECK(home.Scenes()[1] == docks->Id());

    // Every labelled setting: an asset shows its name, and an unset one shows empty text.
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
