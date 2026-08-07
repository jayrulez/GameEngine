// Ported from Sedulous.UI.Tests/src/UIContextTests.bf (faithful; RefPtr views, `===` -> pointer ==).
// All managers (Input/Focus/DragDrop/Animation/Shortcut/Tooltip) are owned by-value on UIContext, so
// Managers_CreatedByDefault just checks the accessors return non-null (they point at the value members).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<RootView> MakeRoot()
{
    return foundation::MakeRef<RootView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<TestView> MakeTestView()
{
    return foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 50.0f, 30.0f);
}
static foundation::RefPtr<TestGroup> MakeTestGroup()
{
    return foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
}

TEST_CASE("uicontext: AddRootView_RegistersAndSetsActive")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    ctx.AddRootView(root.Get());

    CHECK(ctx.RootViewCount() == 1u);
    CHECK(ctx.ActiveInputRoot() == root.Get());
    CHECK(root->Context == &ctx);
}

TEST_CASE("uicontext: AddRootView_FirstBecomesActive")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root1 = MakeRoot();
    foundation::RefPtr<RootView> root2 = MakeRoot();

    ctx.AddRootView(root1.Get());
    ctx.AddRootView(root2.Get());

    CHECK(ctx.ActiveInputRoot() == root1.Get());
}

TEST_CASE("uicontext: RemoveRootView_UpdatesActive")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root1 = MakeRoot();
    foundation::RefPtr<RootView> root2 = MakeRoot();

    ctx.AddRootView(root1.Get());
    ctx.AddRootView(root2.Get());
    ctx.RemoveRootView(root1.Get());

    CHECK(ctx.RootViewCount() == 1u);
    CHECK(ctx.ActiveInputRoot() == root2.Get());
}

TEST_CASE("uicontext: RemoveRootView_ClearsContext")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    ctx.AddRootView(root.Get());
    ctx.RemoveRootView(root.Get());

    CHECK(root->Context == nullptr);
    CHECK(ctx.RootViewCount() == 0u);
    CHECK(ctx.ActiveInputRoot() == nullptr);
}

TEST_CASE("uicontext: Register_ViewLookupByIdWorks")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());

    CHECK(ctx.GetViewById(child->Id) == child.Get());
}

TEST_CASE("uicontext: Register_TypedLookup")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());

    CHECK(ctx.GetViewById<TestView>(child->Id) == child.Get());
}

TEST_CASE("uicontext: Unregister_LookupReturnsNull")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());
    const ViewId id = child->Id;

    root->RemoveView(child.Get(), true);

    CHECK(ctx.GetViewById(id) == nullptr);
}

TEST_CASE("uicontext: AttachView_RegistersSubtree")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    foundation::RefPtr<TestView> child = MakeTestView();
    group->AddView(child.Get()); // build subtree before attaching
    root->AddView(group.Get());  // attach to root - registers both

    CHECK(ctx.GetViewById(group->Id) == group.Get());
    CHECK(ctx.GetViewById(child->Id) == child.Get());
    CHECK(child->Context == &ctx);
}

TEST_CASE("uicontext: DetachView_UnregistersSubtree")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    foundation::RefPtr<TestView> child = MakeTestView();
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
    UIContext ctx;
    ctx.BeginFrame(0.016f);
    CHECK(ctx.DeltaTime() == doctest::Approx(0.016f));
    CHECK(ctx.TotalTime() == doctest::Approx(0.016f));

    ctx.BeginFrame(0.016f);
    CHECK(ctx.TotalTime() == doctest::Approx(0.032f));
}

TEST_CASE("uicontext: DpiScale_DefaultsTo1")
{
    UIContext ctx;
    CHECK(ctx.DpiScale() == 1.0f);
}

TEST_CASE("uicontext: DpiScale_FromActiveRoot")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    root->DpiScale = 2.0f;
    ctx.AddRootView(root.Get());

    CHECK(ctx.DpiScale() == 2.0f);
}

TEST_CASE("uicontext: Managers_CreatedByDefault")
{
    UIContext ctx;
    CHECK(ctx.GetInputManager() != nullptr);
    CHECK(ctx.GetFocusManager() != nullptr);
    CHECK(ctx.DragDrop() != nullptr);
    CHECK(ctx.Animations() != nullptr);
    CHECK(ctx.GetShortcuts() != nullptr);
    CHECK(ctx.Tooltips() != nullptr);
}
