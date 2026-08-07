// Ported from Sedulous.UI.Tests/src/ViewTests.bf (faithful; Beef `scope`/`new` -> RefPtr via MakeRef,
// `===` -> pointer ==, Vector2 -> Float2 (.x/.y), UserData Object -> void*).
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
static foundation::RefPtr<TestView> MakeTestView(f32 w = 50, f32 h = 30)
{
    return foundation::MakeRef<TestView>(foundation::DefaultAllocator(), w, h);
}
static foundation::RefPtr<TestGroup> MakeTestGroup()
{
    return foundation::MakeRef<TestGroup>(foundation::DefaultAllocator());
}

TEST_CASE("view: View_HasUniqueId")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> a = MakeTestView();
    foundation::RefPtr<TestView> b = MakeTestView();
    root->AddView(a.Get());
    root->AddView(b.Get());

    CHECK(!a->Id.Equals(b->Id));
    CHECK(a->Id.IsValid());
    CHECK(b->Id.IsValid());
}

TEST_CASE("view: View_ParentSetOnAdd")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());
    CHECK(child->Parent == root.Get());
}

TEST_CASE("view: View_ContextSetOnAttach")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());
    CHECK(child->Context == &ctx);
}

TEST_CASE("view: View_ContextClearedOnRemove")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());
    root->RemoveView(child.Get());
    CHECK(child->Context == nullptr);
    CHECK(child->Parent == nullptr);
}

TEST_CASE("view: View_RootProperty")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(group.Get());
    group->AddView(child.Get());

    CHECK(child->Root() == root.Get());
    CHECK(group->Root() == root.Get());
    CHECK(root->Root() == root.Get());
}

TEST_CASE("view: View_DefaultMeasure_ClampsToZero")
{
    foundation::RefPtr<TestView> view = MakeTestView(0, 0);
    view->Measure(BoxConstraints::Loose(100, 100));
    CHECK(view->MeasuredSize.x == 0);
    CHECK(view->MeasuredSize.y == 0);
}

TEST_CASE("view: View_Layout_SetsBounds")
{
    foundation::RefPtr<TestView> view = MakeTestView();
    view->Layout(10, 20, 100, 50);
    CHECK(view->Bounds.x == 10);
    CHECK(view->Bounds.y == 20);
    CHECK(view->Width() == 100);
    CHECK(view->Height() == 50);
}

TEST_CASE("view: View_Invalidate_MarksRedraw")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(child.Get());

    CHECK(child->NeedsRedraw());
    child->ClearRedrawFlag();
    CHECK(!child->NeedsRedraw());

    child->Invalidate();
    CHECK(child->NeedsRedraw());
    CHECK(ctx.NeedsRedraw());
}

TEST_CASE("view: View_Visibility_GoneSkipsMeasure")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> child = MakeTestView(100, 50);
    child->Visibility = Visibility::Gone;
    root->AddView(child.Get());

    LayoutPass(ctx, root.Get());
    // Gone views should not affect parent measurement (no crash / no assertion).
}

TEST_CASE("view: View_UserData_SetAndGet")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestView> view = MakeTestView();
    root->AddView(view.Get());

    String testObj(u8"hello");
    view->SetUserData(u8"key", &testObj);
    void* retrieved = view->GetUserData(u8"key");
    CHECK(retrieved == &testObj);
}

TEST_CASE("view: View_UserData_NullWhenNotSet")
{
    foundation::RefPtr<TestView> view = MakeTestView();
    CHECK(view->GetUserData(u8"missing") == nullptr);
}

TEST_CASE("view: View_UserData_TypedRetrieval")
{
    foundation::RefPtr<TestView> view = MakeTestView();
    String str(u8"test");
    view->SetUserData(u8"str", &str);
    String* typed = view->GetUserData<String>(u8"str");
    CHECK(typed == &str);
}

TEST_CASE("view: View_LocalToScreen_NestedViews")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(group.Get());
    group->AddView(child.Get());

    group->Layout(10, 20, 100, 100);
    child->Layout(5, 5, 50, 30);

    const Float2 screen = child->LocalToScreen(Float2{0, 0});
    CHECK(screen.x == doctest::Approx(15));
    CHECK(screen.y == doctest::Approx(25));
}

TEST_CASE("view: View_ScreenToLocal_NestedViews")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(group.Get());
    group->AddView(child.Get());

    group->Layout(10, 20, 100, 100);
    child->Layout(5, 5, 50, 30);

    const Float2 local = child->ScreenToLocal(Float2{15, 25});
    CHECK(local.x == doctest::Approx(0));
    CHECK(local.y == doctest::Approx(0));
}

TEST_CASE("view: View_IsEffectivelyEnabled_WalksParents")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(group.Get());
    group->AddView(child.Get());

    CHECK(child->IsEffectivelyEnabled());

    group->IsEnabled = false;
    CHECK(!child->IsEffectivelyEnabled());
    CHECK(!group->IsEffectivelyEnabled());
}

TEST_CASE("view: View_GetControlState_Disabled")
{
    foundation::RefPtr<TestView> view = MakeTestView();
    view->IsEnabled = false;
    CHECK(HasFlag(view->GetControlState(), ControlState::Disabled));
}

TEST_CASE("view: View_EffectiveCursor_InheritsFromParent")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    group->Cursor = CursorType::Hand;
    foundation::RefPtr<TestView> child = MakeTestView();
    root->AddView(group.Get());
    group->AddView(child.Get());

    CHECK(child->EffectiveCursor(Float2{0, 0}) == CursorType::Hand);
    CHECK(child->Cursor == CursorType::Default);
}

TEST_CASE("view: View_EffectiveCursor_ChildOverridesParent")
{
    UIContext ctx;
    foundation::RefPtr<RootView> root = MakeRoot();
    Init(ctx, root.Get());

    foundation::RefPtr<TestGroup> group = MakeTestGroup();
    group->Cursor = CursorType::Hand;
    foundation::RefPtr<TestView> child = MakeTestView();
    child->Cursor = CursorType::IBeam;
    root->AddView(group.Get());
    group->AddView(child.Get());

    CHECK(child->EffectiveCursor(Float2{0, 0}) == CursorType::IBeam);
}
