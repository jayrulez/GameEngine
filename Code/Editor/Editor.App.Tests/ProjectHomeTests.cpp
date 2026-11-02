// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the Project page over its markup (Data/Editor/ProjectHome.xml). Covers the
// project's name and summary (location, engine version, the settings marked `summary` with an asset
// shown by its name, the window), its buttons, the scenes (assets of the default scene setting's
// type) as cards that open them with the default one marked, a project with no scenes, and a rebuild
// when the settings change.
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
}

TEST_CASE("project page: the summary, the buttons, and the scenes as cards that open them")
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
    RefPtr<foundation::gui::Node> content = LoadHome();
    REQUIRE(content.Get() != nullptr);
    app::ProjectHomeView home(DefaultAllocator(), context, nullptr, content);
    Array<Guid> opened;
    String shown;
    i32 settingsOpened = 0;
    i32 scenesMade = 0;
    home.SetOnOpenAsset([&opened](const Guid& id) { opened.PushBack(id); });
    home.SetOnShowFolder([&shown](StringView folder) { shown = String(folder); });
    home.SetOnProjectSettings([&settingsOpened]() { ++settingsOpened; });
    home.SetOnNewScene([&scenesMade]() { ++scenesMade; });
    CHECK(FindId<foundation::gui::Label>(*content, u8"home-name")->GetText() == StringView(u8"Harbour"));

    // The summary: where it is, its engine, the settings marked for it, and the window; no others.
    Span<const String> rows = home.SummaryRows();
    REQUIRE(rows.Size() == 7u);
    CHECK(rows[0].AsView().StartsWith(u8"Location: "));
    CHECK(rows[1] == Format(u8"Engine version: {}", project->Settings().engineVersion));
    CHECK(rows[2] == StringView(u8"Default scene: Docks"));
    CHECK(rows[3] == StringView(u8"Startup script: (none)"));
    CHECK(rows[4] == StringView(u8"Input map: (none)"));
    CHECK(rows[5].AsView().StartsWith(u8"UI font: "));
    CHECK(rows[6] == StringView(u8"Window: 1280 × 720, windowed"));
    CHECK_FALSE(Has(rows, u8"Default bus layout: (built-in)"));

    // Its buttons.
    CHECK(FindId<foundation::gui::Button>(*content, u8"home-show-folder")->HandleActivate());
    CHECK(shown == project->Directory());
    CHECK(FindId<foundation::gui::Button>(*content, u8"home-settings")->HandleActivate());
    CHECK(settingsOpened == 1);
    auto* newScene = FindId<foundation::gui::Button>(*content, u8"home-new-scene");
    REQUIRE(newScene != nullptr);
    CHECK(newScene->IsDisplayed());
    CHECK(newScene->HandleActivate());
    CHECK(scenesMade == 1);
    CHECK_FALSE(FindId<foundation::gui::UIWidget>(*content, u8"home-no-scenes")->IsDisplayed());

    // A card per scene, in the asset tree's order (by name); each opens its scene, and the default
    // one is marked. With no thumbnail service, each shows the scene type's icon.
    REQUIRE(home.Scenes().Size() == 2u);
    CHECK(home.Scenes()[0] == arrival->Id());
    CHECK(home.Scenes()[1] == docks->Id());
    CHECK(FindId<foundation::gui::Label>(*content, u8"home-scene-count")->GetText() == StringView(u8"2"));
    foundation::gui::Node* grid = FindId<foundation::gui::Node>(*content, u8"home-scenes");
    REQUIRE(grid != nullptr);
    REQUIRE(grid->ChildCount() == 2u);
    auto* first = Cast<foundation::gui::Button>(grid->GetChildAt(0));
    REQUIRE(first != nullptr);
    CHECK(first->HandleActivate());
    REQUIRE(opened.Size() == 1u);
    CHECK(opened[0] == arrival->Id());
    const auto marked = [](foundation::gui::Node& card)
    {
        foundation::gui::Node& picture = *card.GetChildAt(0);
        for (usize i = 0; i < picture.ChildCount(); ++i)
            if (auto* label = Cast<foundation::gui::Label>(picture.GetChildAt(i)); label != nullptr && label->GetText() == StringView(u8"Default"))
                return true;
        return false;
    };
    CHECK_FALSE(marked(*grid->GetChildAt(0)));
    CHECK(marked(*grid->GetChildAt(1)));
    CHECK(home.ThumbnailsShown() == 0u);

    // A change to the settings shows at the next refresh, once marked.
    project->Settings().defaultSceneId = arrival->Id();
    home.Refresh();
    CHECK(Has(home.SummaryRows(), u8"Default scene: Docks"));
    home.MarkChanged();
    home.Refresh();
    CHECK(Has(home.SummaryRows(), u8"Default scene: Arrival"));
    CHECK(marked(*grid->GetChildAt(0)));

    // Without a project, nothing to show.
    context.SetProject(nullptr);
    home.Rebuild();
    CHECK(home.Scenes().IsEmpty());
    CHECK(home.SummaryRows().IsEmpty());
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("project page: a project with no scenes shows a note with New Scene and the first-project page")
{
    const StringView dir = u8"scratch_project_home_empty";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Harbour").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    RefPtr<foundation::gui::Node> content = LoadHome();
    REQUIRE(content.Get() != nullptr);
    app::ProjectHomeView home(DefaultAllocator(), context, nullptr, content);
    i32 scenesMade = 0;
    String docs;
    home.SetOnNewScene([&scenesMade]() { ++scenesMade; });
    home.SetOnOpenDocs([&docs](StringView page) { docs = String(page); });

    CHECK(home.Scenes().IsEmpty());
    CHECK(Has(home.SummaryRows(), u8"Default scene: (none)"));
    CHECK(FindId<foundation::gui::Label>(*content, u8"home-scene-count")->GetText() == StringView(u8"0"));
    CHECK(FindId<foundation::gui::UIWidget>(*content, u8"home-no-scenes")->IsDisplayed());
    CHECK_FALSE(FindId<foundation::gui::UIWidget>(*content, u8"home-scenes")->IsDisplayed());
    CHECK_FALSE(FindId<foundation::gui::UIWidget>(*content, u8"home-new-scene")->IsDisplayed()); // the note has its own
    CHECK(FindId<foundation::gui::Button>(*content, u8"home-first-scene")->HandleActivate());
    CHECK(scenesMade == 1);
    CHECK(FindId<foundation::gui::Button>(*content, u8"home-first-docs")->HandleActivate());
    CHECK(docs == StringView(app::kProjectHomeFirstProjectPage));

    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
