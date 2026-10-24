// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - TreeModel + TreeView tests: hierarchical navigation (RowCount(parent)/Index/
// ParentIndex/HasChildren), and a TreeView that flattens visible nodes, expands/collapses (arrow
// click + keyboard), and remaps selection by node id across re-flattening.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;

using namespace foundation::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // Tree:  A [A1 [A1x], A2],  B
    struct Tree
    {
        TreeModel model;
        core::i32 a, b, a1, a2, a1x;
        Tree()
        {
            a = model.AddNode(TreeModel::kRoot, SV(u8"A"));
            b = model.AddNode(TreeModel::kRoot, SV(u8"B"));
            a1 = model.AddNode(a, SV(u8"A1"));
            a2 = model.AddNode(a, SV(u8"A2"));
            a1x = model.AddNode(a1, SV(u8"A1x"));
        }
    };

    core::RefPtr<TreeView> MountTree(core::RefPtr<SceneRoot>& root, IModel* model)
    {
        root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        root->SetSize(core::Float2{400.0f, 400.0f});
        auto tree = Make<TreeView>();
        tree->SetSize(core::Float2{260.0f, 300.0f}); // tall enough to realize all visible rows
        tree->SetRowHeight(20.0f);
        tree->SetIndentWidth(16.0f);
        root->AddChild(tree.Get());
        tree->SetModel(model);
        (void)root->UpdateLayout();
        return tree;
    }
}

TEST_CASE("tree-model: hierarchy navigation")
{
    Tree t;
    CHECK(t.model.RowCount() == 2);                          // two roots
    const ModelIndex ia = t.model.Index(0, 0, ModelIndex{}); // A
    CHECK(t.model.Data(ia).AsString() == SV(u8"A"));
    CHECK(t.model.HasChildren(ia));
    CHECK(t.model.RowCount(ia) == 2); // A1, A2

    const ModelIndex ia1 = t.model.Index(0, 0, ia); // A1
    CHECK(t.model.Data(ia1).AsString() == SV(u8"A1"));
    CHECK(t.model.RowCount(ia1) == 1);     // A1x
    CHECK(t.model.ParentIndex(ia1) == ia); // A1's parent is A

    const ModelIndex ib = t.model.Index(1, 0, ModelIndex{}); // B
    CHECK_FALSE(t.model.HasChildren(ib));
    CHECK_FALSE(t.model.ParentIndex(ia).IsValid()); // A is a root
}

TEST_CASE("tree-view: collapsed shows only roots; expanding reveals children")
{
    Tree t;
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);

    CHECK(tree->VisibleRowCount() == 2); // A, B (collapsed)

    tree->ExpandItem(0); // expand A -> A, A1, A2, B
    CHECK(tree->VisibleRowCount() == 4);
    CHECK(tree->IsItemExpanded(0));
    CHECK(tree->ItemDepth(1) == 1); // A1 is a child

    tree->ExpandItem(1); // expand A1 -> A, A1, A1x, A2, B
    CHECK(tree->VisibleRowCount() == 5);
    CHECK(tree->ItemDepth(2) == 2); // A1x is a grandchild

    tree->CollapseItem(0); // collapse A -> A, B
    CHECK(tree->VisibleRowCount() == 2);
}

TEST_CASE("tree-view: clicking the arrow toggles expansion")
{
    Tree t;
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);
    EventDispatcher* d = root->GetEventDispatcher();

    // Root A's arrow: row 0 (y in [0,20]), depth 0 -> arrow at x in [0,16].
    d->InjectMouseDown(core::Float2{8.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{8.0f, 10.0f}, MouseButton::Left);
    CHECK(tree->VisibleRowCount() == 4); // expanded
    CHECK(tree->IsItemExpanded(0));

    d->InjectMouseDown(core::Float2{8.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{8.0f, 10.0f}, MouseButton::Left);
    CHECK(tree->VisibleRowCount() == 2); // collapsed again
}

TEST_CASE("tree-view: selection is remapped by node id across expand/collapse")
{
    Tree t;
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);

    tree->ExpandItem(0);      // flat: A(0), A1(1), A2(2), B(3)
    tree->SetSelectedItem(3); // select B
    const core::i64 selId = tree->GetSelectedIndex().InternalId;

    tree->CollapseItem(0);                               // flat: A(0), B(1) - B moved
    CHECK(tree->GetSelectedItem() == 1);                 // remapped to B's new row
    CHECK(tree->GetSelectedIndex().InternalId == selId); // same node
}

TEST_CASE("tree-view: keyboard Right expands and Left collapses the selection")
{
    Tree t;
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);
    EventDispatcher* d = root->GetEventDispatcher();
    tree->RequestFocus();

    tree->SetSelectedItem(0); // select A
    const auto key = [](KeyCode k) { return static_cast<core::u32>(k); };
    d->InjectKeyDown(key(KeyCode::Right)); // expand A
    CHECK(tree->VisibleRowCount() == 4);
    CHECK(tree->IsItemExpanded(0));
    d->InjectKeyDown(key(KeyCode::Left)); // collapse A
    CHECK(tree->VisibleRowCount() == 2);
    CHECK_FALSE(tree->IsItemExpanded(0));
}
TEST_CASE("tree-view: in right to left Left expands and Right collapses, toward the way it is indented")
{
    Tree t;
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);
    root->SetLayoutDirection(Node::LayoutDirection::RightToLeft);
    EventDispatcher* d = root->GetEventDispatcher();
    tree->RequestFocus();

    tree->SetSelectedItem(0);
    const auto key = [](KeyCode k) { return static_cast<core::u32>(k); };
    d->InjectKeyDown(key(KeyCode::Left)); // toward the inline end: expand
    CHECK(tree->IsItemExpanded(0));
    d->InjectKeyDown(key(KeyCode::Right)); // toward the start: collapse
    CHECK_FALSE(tree->IsItemExpanded(0));
}


TEST_CASE("tree-view: a sheet sets its arrow's column and its indent")
{
    Tree t;
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);
    StyleManager styles;
    styles.SetStyleSheet(CSSParser::Parse(SV(u8"treeview { arrow-size: calc(5px * 2); indent-width: 8px; }")));
    styles.ApplyTree(*root.Get());
    (void)root->UpdateLayout();
    CHECK(tree->GetArrowSize() == doctest::Approx(10.0f));
    CHECK(tree->GetIndentWidth() == doctest::Approx(8.0f));
    EventDispatcher* d = root->GetEventDispatcher();
    // Past the 10 px arrow (inside the default 16): the row, not its arrow.
    d->InjectMouseDown(core::Float2{13.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{13.0f, 10.0f}, MouseButton::Left);
    CHECK_FALSE(tree->IsItemExpanded(0));
    d->InjectMouseDown(core::Float2{5.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{5.0f, 10.0f}, MouseButton::Left);
    CHECK(tree->IsItemExpanded(0));
}

TEST_CASE("tree-view: a row shows the model's icon, with its class, between the arrow and the text")
{
    Tree t;
    t.model.SetIcon(t.a, SV(u8"Icons/folder.svg"), SV(u8"cat-rendering"));
    core::RefPtr<SceneRoot> root;
    auto tree = MountTree(root, &t.model);
    tree->SetIconSize(14.0f);
    (void)root->UpdateLayout();

    const auto* withIcon = core::Cast<TreeRowContent>(tree->RowForItem(0)->GetContent()); // A
    const auto* without = core::Cast<TreeRowContent>(tree->RowForItem(1)->GetContent());  // B
    REQUIRE(withIcon != nullptr);
    REQUIRE(without != nullptr);
    REQUIRE(withIcon->GetIcon() != nullptr);
    CHECK(withIcon->GetIcon()->GetSource() == core::String(u8"Icons/folder.svg"));
    CHECK(withIcon->GetIcon()->HasClass(SV(u8"cat-rendering")));
    CHECK(withIcon->GetIcon()->GetSize().x == doctest::Approx(14.0f));
    // The icon after the arrow's column, the text after the icon; a row with none starts its text earlier.
    CHECK(withIcon->GetIcon()->GetPosition().x >= withIcon->GetArrow()->GetPosition().x + withIcon->GetArrow()->GetSize().x);
    CHECK(withIcon->GetLabel()->GetPosition().x >= withIcon->GetIcon()->GetPosition().x + 14.0f);
    CHECK(without->GetLabel()->GetPosition().x < withIcon->GetLabel()->GetPosition().x);

    // The icon goes when the model takes it away, and the text moves back.
    t.model.SetIcon(t.a, SV(u8""));
    (void)root->UpdateLayout();
    const auto* cleared = core::Cast<TreeRowContent>(tree->RowForItem(0)->GetContent());
    CHECK((cleared->GetIcon() == nullptr || !cleared->GetIcon()->IsDisplayed()));
    CHECK(cleared->GetLabel()->GetPosition().x == doctest::Approx(without->GetLabel()->GetPosition().x));
}

TEST_CASE("tree-view: markup's items take an icon and its class")
{
    RegisterGuiElements();
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 400.0f});
    MarkupLoader loader(*root->GetElementSet(), core::DefaultAllocator());
    auto loaded = loader.LoadFromString(SV(u8R"(<TreeView width="300" height="300" row-height="20">
        <TreeItem text="Sun" icon="Icons/light.svg" icon-class="cat-rendering"/>
        <TreeItem text="Group"/>
    </TreeView>)"));
    auto* tree = core::Cast<TreeView>(loaded.Get());
    REQUIRE(tree != nullptr);
    root->AddChild(tree);
    (void)root->UpdateLayout();
    const auto* sun = core::Cast<TreeRowContent>(tree->RowForItem(0)->GetContent());
    REQUIRE(sun != nullptr);
    REQUIRE(sun->GetIcon() != nullptr);
    CHECK(sun->GetIcon()->GetSource() == core::String(u8"Icons/light.svg"));
    CHECK(sun->GetIcon()->HasClass(SV(u8"cat-rendering")));
    CHECK(core::Cast<TreeRowContent>(tree->RowForItem(1)->GetContent())->GetIcon() == nullptr);
}
