// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the Assets panel and the asset picker over a scratch project: the browser's
// group tree indents each level's row by the tree's own ContentInset, so the text steps with the
// chevrons at every depth; both views of assets switch between a list and a grid through the same
// List / Grid icon toggles (a radio pair), and the picker keeps its own choice per project.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.ui;
import foundation.settings;
import pipeline.importer; // ImportOptions: a type to make instances of
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace content = foundation::content;
namespace ui = foundation::ui;

TEST_CASE("AssetsView: the group tree indents every level by the tree's ContentInset")
{
    const String dir(u8"assets-view-indent");
    std::error_code ec;
    std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Indent").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(project);

    // Content / A / B / C: four levels, depths 0 to 3.
    content::Group* a = project->SourceDb().RootGroup()->CreateGroup(u8"A");
    REQUIRE(a != nullptr);
    content::Group* b = a->CreateGroup(u8"B");
    REQUIRE(b != nullptr);
    REQUIRE(b->CreateGroup(u8"C") != nullptr);

    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    EditorCookService cook;
    {
        RefPtr<app::AssetsView> view = MakeRef<app::AssetsView>(DefaultAllocator(), context, cook);
        view->Rebuild();
        ui::TreeView* tree = view->GroupTree();
        REQUIRE(tree != nullptr);
        ui::ITreeAdapter* adapter = tree->TreeAdapter;
        REQUIRE(adapter != nullptr);

        // Walk down the first child at each level, binding a row for each node as the tree does.
        i32 node = adapter->GetChildId(-1, 0);
        for (i32 depth = 0; depth < 4; ++depth)
        {
            CAPTURE(depth);
            REQUIRE(node >= 0);
            REQUIRE(adapter->GetDepth(node) == depth);
            RefPtr<ui::View> row = adapter->CreateView(adapter->GetItemViewType(node));
            REQUIRE(row);
            adapter->BindView(row.Get(), node, depth, true);
            auto* label = static_cast<ui::EditableLabel*>(row.Get());
            CHECK(label->TextOffsetX.Value() == doctest::Approx(tree->ContentInset(depth)));
            node = adapter->GetChildCount(node) > 0 ? adapter->GetChildId(node, 0) : -1;
        }
    }
    context.SetProject(nullptr);
    project = nullptr;
    std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
}

namespace
{
    // A scratch project with one asset in the root group, open in an editor context that has a
    // project editor settings store (where the view modes are remembered).
    struct PickerBench
    {
        String dir;
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::settings::Settings store{DefaultAllocator()};
        Guid asset;

        explicit PickerBench(StringView leaf) : dir(leaf)
        {
            std::error_code ec;
            std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Views").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(project);
            content::Instance* instance = project->SourceDb().RootGroup()->CreateInstance(
                u8"Crate", pipeline::ImportOptions::StaticType());
            REQUIRE(instance != nullptr);
            asset = instance->Id();
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
        }
        ~PickerBench()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project = nullptr;
            std::error_code ec;
            std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
        }
    };
}

TEST_CASE("ViewModeToggles: two icon toggles with their words as tooltips, one always checked")
{
    app::EditorIcons& icons = app::EditorIcons::Get();
    icons.Initialize();
    {
        RefPtr<app::ViewModeToggles> toggles = MakeRef<app::ViewModeToggles>(DefaultAllocator());
        ui::ToggleButton* list = toggles->ListToggle();
        ui::ToggleButton* grid = toggles->GridToggle();
        CHECK(list->TooltipText == StringView(u8"List"));
        CHECK(grid->TooltipText == StringView(u8"Grid"));
        // Icons, not words: each toggle's content is a drawable view showing its icon.
        auto* listGlyph = Cast<ui::DrawableView>(list->Content());
        auto* gridGlyph = Cast<ui::DrawableView>(grid->Content());
        REQUIRE(listGlyph != nullptr);
        REQUIRE(gridGlyph != nullptr);
        CHECK(listGlyph->Drawable.Get() == icons.viewList.Get());
        CHECK(gridGlyph->Drawable.Get() == icons.viewGrid.Get());

        i32 calls = 0;
        bool last = false;
        toggles->OnModeChanged = [&](bool g) { ++calls; last = g; };
        CHECK(list->IsChecked.Value());
        CHECK_FALSE(grid->IsChecked.Value());

        // Checking the other one switches, unchecking its partner, and reports it once.
        grid->IsChecked.SetValue(true);
        CHECK(toggles->GridMode());
        CHECK_FALSE(list->IsChecked.Value());
        CHECK(calls == 1);
        CHECK(last);

        // Unchecking the checked one (a click on it) leaves it checked, with no report.
        grid->IsChecked.SetValue(false);
        CHECK(grid->IsChecked.Value());
        CHECK(toggles->GridMode());
        CHECK(calls == 1);

        // SetGridMode is the owner's: it moves the check without reporting back.
        toggles->SetGridMode(false);
        CHECK(list->IsChecked.Value());
        CHECK_FALSE(grid->IsChecked.Value());
        CHECK(calls == 1);
    }
    icons.Shutdown();
}

TEST_CASE("AssetsView: the Grid toggle shows the grid and remembers it for the project")
{
    PickerBench bench(u8"assets-view-modes");
    EditorCookService cook;
    RefPtr<app::AssetsView> view = MakeRef<app::AssetsView>(DefaultAllocator(), bench.context, cook);
    CHECK_FALSE(view->IsGridMode());
    view->ViewToggles()->GridToggle()->IsChecked.SetValue(true);
    CHECK(view->IsGridMode());
    const app::EditorAssetBrowserSettings* section =
        bench.store.Find<app::EditorAssetBrowserSettings>();
    REQUIRE(section != nullptr);
    CHECK(section->gridMode);
    CHECK_FALSE(section->pickerGridMode); // the picker's choice is its own
}

TEST_CASE("AssetPickerDialog: a grid of the same rows, picked from, its mode kept per project")
{
    PickerBench bench(u8"asset-picker-grid");
    Guid picked;
    {
        RefPtr<app::AssetPickerDialog> picker =
            MakeRef<app::AssetPickerDialog>(DefaultAllocator(), bench.context, Array<String>{});
        CHECK_FALSE(picker->IsGridMode());
        CHECK(picker->RowList()->Visibility == ui::VisibilityValue::Visible);
        CHECK(picker->RowGrid()->Visibility == ui::VisibilityValue::Gone);
        REQUIRE(picker->Rows().Size() == 1);
        CHECK(picker->Rows()[0] == bench.asset);

        picker->ViewToggles()->GridToggle()->IsChecked.SetValue(true);
        CHECK(picker->IsGridMode());
        CHECK(picker->RowList()->Visibility == ui::VisibilityValue::Gone);
        CHECK(picker->RowGrid()->Visibility == ui::VisibilityValue::Visible);
        const app::EditorAssetBrowserSettings* section =
            bench.store.Find<app::EditorAssetBrowserSettings>();
        REQUIRE(section != nullptr);
        CHECK(section->pickerGridMode);
        CHECK_FALSE(section->gridMode); // the browser's choice is its own

        // A double-click on a tile picks its asset, as on a list row.
        picker->OnPicked = [&](const Guid& id) { picked = id; };
        picker->RowGrid()->OnItemClicked.Invoke(0, 2, 0.0f, 0.0f);
    }
    CHECK(picked == bench.asset);

    // The next picker opens in the grid.
    RefPtr<app::AssetPickerDialog> again =
        MakeRef<app::AssetPickerDialog>(DefaultAllocator(), bench.context, Array<String>{});
    CHECK(again->IsGridMode());
    CHECK(again->ViewToggles()->GridToggle()->IsChecked.Value());
}
