// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/UIContextTests.bf (faithful; RefPtr views, `===` -> pointer ==).
// All managers (Input/Focus/DragDrop/Animation/Shortcut/Tooltip) are owned by-value on UIContext, so
// Managers_CreatedByDefault just checks the accessors return non-null (they point at the value members).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

static core::RefPtr<RootView> MakeRoot()
{
    return core::MakeRef<RootView>(core::DefaultAllocator());
}
static core::RefPtr<TestView> MakeTestView()
{
    return core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
}
static core::RefPtr<TestGroup> MakeTestGroup()
{
    return core::MakeRef<TestGroup>(core::DefaultAllocator());
}

TEST_CASE("uicontext: AddRootView_RegistersAndSetsActive")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    ctx.AddRootView(root.Get());

    CHECK(ctx.RootViewCount() == 1u);
    CHECK(ctx.ActiveInputRoot() == root.Get());
    CHECK(root->Context == &ctx);
}

TEST_CASE("uicontext: AddRootView_FirstBecomesActive")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root1 = MakeRoot();
    core::RefPtr<RootView> root2 = MakeRoot();

    ctx.AddRootView(root1.Get());
    ctx.AddRootView(root2.Get());

    CHECK(ctx.ActiveInputRoot() == root1.Get());
}

TEST_CASE("uicontext: RemoveRootView_UpdatesActive")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root1 = MakeRoot();
    core::RefPtr<RootView> root2 = MakeRoot();

    ctx.AddRootView(root1.Get());
    ctx.AddRootView(root2.Get());
    ctx.RemoveRootView(root1.Get());

    CHECK(ctx.RootViewCount() == 1u);
    CHECK(ctx.ActiveInputRoot() == root2.Get());
}

TEST_CASE("uicontext: RemoveRootView_ClearsContext")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    ctx.AddRootView(root.Get());
    ctx.RemoveRootView(root.Get());

    CHECK(root->Context == nullptr);
    CHECK(ctx.RootViewCount() == 0u);
    CHECK(ctx.ActiveInputRoot() == nullptr);
}

TEST_CASE("uicontext: Register_ViewLookupByIdWorks")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    core::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());

    CHECK(ctx.GetViewById(child->Id) == child.Get());
}

TEST_CASE("uicontext: Register_TypedLookup")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    core::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());

    CHECK(ctx.GetViewById<TestView>(child->Id) == child.Get());
}

TEST_CASE("uicontext: Unregister_LookupReturnsNull")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    core::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());
    const ViewId id = child->Id;

    root->RemoveView(child.Get(), true);

    CHECK(ctx.GetViewById(id) == nullptr);
}

TEST_CASE("uicontext: AttachView_RegistersSubtree")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    core::RefPtr<TestGroup> group = MakeTestGroup();
    core::RefPtr<TestView> child = MakeTestView();
    group->AddView(child.Get()); // build subtree before attaching
    root->AddView(group.Get());  // attach to root - registers both

    CHECK(ctx.GetViewById(group->Id) == group.Get());
    CHECK(ctx.GetViewById(child->Id) == child.Get());
    CHECK(child->Context == &ctx);
}

TEST_CASE("uicontext: DetachView_UnregistersSubtree")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    core::RefPtr<TestGroup> group = MakeTestGroup();
    core::RefPtr<TestView> child = MakeTestView();
    root->AddView(group.Get());
    group->AddView(child.Get());
    const ViewId groupId = group->Id;
    const ViewId childId = child->Id;

    root->RemoveView(group.Get());
    CHECK(ctx.GetViewById(groupId) == nullptr);
    CHECK(ctx.GetViewById(childId) == nullptr);
}

TEST_CASE("uicontext: BeginFrame_UpdatesTime")
{
    UIContext ctx{DefaultAllocator()};
    ctx.BeginFrame(0.016f);
    CHECK(ctx.DeltaTime() == doctest::Approx(0.016f));
    CHECK(ctx.TotalTime() == doctest::Approx(0.016f));

    ctx.BeginFrame(0.016f);
    CHECK(ctx.TotalTime() == doctest::Approx(0.032f));
}

TEST_CASE("uicontext: DpiScale_DefaultsTo1")
{
    UIContext ctx{DefaultAllocator()};
    CHECK(ctx.DpiScale() == 1.0f);
}

TEST_CASE("uicontext: DpiScale_FromActiveRoot")
{
    UIContext ctx{DefaultAllocator()};
    core::RefPtr<RootView> root = MakeRoot();
    root->DpiScale = 2.0f;
    ctx.AddRootView(root.Get());

    CHECK(ctx.DpiScale() == 2.0f);
}

TEST_CASE("uicontext: Managers_CreatedByDefault")
{
    UIContext ctx{DefaultAllocator()};
    CHECK(ctx.GetInputManager() != nullptr);
    CHECK(ctx.GetFocusManager() != nullptr);
    CHECK(ctx.DragDrop() != nullptr);
    CHECK(ctx.Animations() != nullptr);
    CHECK(ctx.GetShortcuts() != nullptr);
    CHECK(ctx.Tooltips() != nullptr);
}
