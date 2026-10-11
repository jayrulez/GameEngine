// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the project launcher: the recent projects as a list or a grid (remembered),
// each by its picture (the thumbnail in its editor folder, an image chosen for it, or a placeholder
// with its initial), and each project's menu.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.settings;
import foundation.image;
import foundation.image.io;
import foundation.ui;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

namespace
{
    // The tooltip's texts: its labels (name, detail, path), then its row of buttons.
    void CollectTexts(const foundation::ui::View& tip, Array<String>& labels, Array<String>& buttons)
    {
        const auto& view = static_cast<const foundation::ui::ViewGroup&>(tip);
        REQUIRE(view.ChildCount() == 4u);
        for (usize i = 0; i < 3; ++i)
        {
            labels.PushBack(String(static_cast<const foundation::ui::Label*>(view.GetChildAt(i))->Text.Value()));
        }
        const auto& row = static_cast<const foundation::ui::ViewGroup&>(*view.GetChildAt(3));
        for (usize i = 0; i < row.ChildCount(); ++i)
        {
            buttons.PushBack(String(static_cast<const foundation::ui::Button*>(row.GetChildAt(i))->Text.Value()));
        }
    }

    struct Launcher
    {
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_launcher");
        String first = PathJoin(root.AsView(), u8"First");
        String second = PathJoin(root.AsView(), u8"Second");
        String gone = PathJoin(root.AsView(), u8"Gone");
        foundation::settings::Settings store{DefaultAllocator()};
        UniquePtr<ProjectManagerController> controller;
        UniquePtr<app::ProjectManagerView> view;
        u32 saves = 0;

        Launcher()
        {
            RegisterProjectRegistryTypes();
            (void)RemoveDirectoryRecursive(root.AsView());
            REQUIRE(CreateDirectories(root.AsView()));
            REQUIRE(EditorProject::Create(DefaultAllocator(), first.AsView(), u8"First").IsOk());
            REQUIRE(EditorProject::Create(DefaultAllocator(), second.AsView(), u8"Second").IsOk());
            TouchRecentProject(store, gone.AsView(), u8"Gone", u8"0.1.0"); // no project there
            TouchRecentProject(store, second.AsView(), u8"Second", u8"0.1.0");
            TouchRecentProject(store, first.AsView(), u8"First", u8"0.1.0");
            controller = MakeUnique<ProjectManagerController>(DefaultAllocator(), store);
            view = MakeUnique<app::ProjectManagerView>(DefaultAllocator(), DefaultAllocator());
            view->OnStoreChanged = [this]() { ++saves; };
            view->Build(*controller, nullptr, nullptr, 1280, 800);
        }
        ~Launcher()
        {
            view.Reset();
            (void)RemoveDirectoryRecursive(root.AsView());
        }

        /// A small RGBA picture written as `name` under the scratch root.
        String WritePicture(StringView name)
        {
            foundation::image::Image picture(16, 9, foundation::image::PixelFormat::RGBA8);
            const String path = PathJoin(root.AsView(), name);
            REQUIRE(foundation::image::io::SaveImage(picture, path.AsView(), foundation::image::io::ImageFileFormat::PNG)
                        .IsOk());
            return path;
        }
    };
}

TEST_CASE("launcher: the projects as a list or a grid, the choice remembered")
{
    Launcher l;
    CHECK_FALSE(l.view->IsGridView());
    CHECK(l.view->CardCount() == 3u);
    CHECK(l.view->ThumbnailCount() == 0u); // no pictures yet: placeholders

    l.view->SetGridView(true);
    CHECK(l.view->IsGridView());
    CHECK(l.view->CardCount() == 3u);
    CHECK(l.saves == 1u);
    CHECK(l.controller->Entries().gridView); // kept with the recent projects
    l.view->SetGridView(true);
    CHECK(l.saves == 1u); // no change, nothing to save
}

TEST_CASE("launcher: a project's picture is its thumbnail, an image chosen for it, or none")
{
    Launcher l;
    // A captured thumbnail in the project's editor folder shows.
    const String captured = l.WritePicture(u8"captured.png");
    REQUIRE(CreateDirectories(PathParent(ProjectThumbnailPath(l.first.AsView()).AsView())));
    foundation::image::Image picture;
    REQUIRE(foundation::image::io::LoadImage(captured.AsView(), picture).IsOk());
    REQUIRE(foundation::image::io::SaveImage(picture, ProjectThumbnailPath(l.first.AsView()).AsView(),
                                             foundation::image::io::ImageFileFormat::PNG)
                .IsOk());
    l.view->Rebuild();
    CHECK(l.view->ThumbnailCount() == 1u);

    // An image chosen for the second project becomes its thumbnail.
    const String chosen = l.WritePicture(u8"chosen.png");
    CHECK(l.view->SetThumbnailFromImage(l.second.AsView(), chosen.AsView()));
    CHECK(FileExists(ProjectThumbnailPath(l.second.AsView()).AsView()));
    CHECK(l.view->ThumbnailCount() == 2u);
    CHECK_FALSE(l.view->SetThumbnailFromImage(l.second.AsView(), u8"/nowhere/at/all.png"));
}

TEST_CASE("launcher: each project's menu holds what applies to it")
{
    Launcher l;
    Array<String> items = l.view->MenuItemsFor(l.first.AsView());
    REQUIRE(items.Size() == 5u);
    CHECK(items[0] == StringView(u8"Open"));
    CHECK(items[1] == StringView(u8"Show in Folder"));
    CHECK(items[2] == StringView(u8"Copy Path"));
    CHECK(items[3] == StringView(u8"Choose Thumbnail Image..."));
    CHECK(items[4] == StringView(u8"Remove from List"));

    // With a thumbnail, it can be cleared.
    CHECK(l.view->SetThumbnailFromImage(l.first.AsView(), l.WritePicture(u8"p.png").AsView()));
    items = l.view->MenuItemsFor(l.first.AsView());
    CHECK(items[4] == StringView(u8"Clear Thumbnail"));

    // A missing project: only what still makes sense.
    items = l.view->MenuItemsFor(l.gone.AsView());
    REQUIRE(items.Size() == 3u);
    CHECK(items[0] == StringView(u8"Show in Folder"));
    CHECK(items[2] == StringView(u8"Remove from List"));
}

TEST_CASE("launcher: a tile's tooltip names the project, its engine and its path, with its actions")
{
    Launcher l;
    l.view->SetGridView(true);
    RefPtr<foundation::ui::View> tip = l.view->TileTooltipFor(l.first.AsView());
    REQUIRE(tip.Get() != nullptr);
    Array<String> labels;
    Array<String> buttons;
    CollectTexts(*tip, labels, buttons);
    REQUIRE(labels.Size() == 3u);
    CHECK(labels[0] == StringView(u8"First"));
    CHECK(labels[1].AsView().StartsWith(u8"Engine "));
    CHECK(labels[2] == l.first.AsView());
    REQUIRE(buttons.Size() == 3u);
    CHECK(buttons[0] == StringView(u8"Open"));
    CHECK(buttons[1] == StringView(u8"Show in Folder"));
    CHECK(buttons[2] == StringView(u8"Copy Path"));

    // A missing project: no Open, and the tooltip says why.
    tip = l.view->TileTooltipFor(l.gone.AsView());
    REQUIRE(tip.Get() != nullptr);
    labels.Clear();
    buttons.Clear();
    CollectTexts(*tip, labels, buttons);
    CHECK(labels[1].AsView().StartsWith(u8"Missing"));
    REQUIRE(buttons.Size() == 2u);
    CHECK(buttons[0] == StringView(u8"Show in Folder"));

    // Not a project the launcher shows: no tooltip.
    CHECK(l.view->TileTooltipFor(u8"/nowhere").Get() == nullptr);
}
