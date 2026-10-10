// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the Assets panel over a scratch project: its group tree indents each
// level's row by the tree's own ContentInset, so the text steps with the chevrons at every depth.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import foundation.ui;
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
