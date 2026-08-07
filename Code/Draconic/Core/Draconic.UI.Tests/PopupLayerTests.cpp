// Ported from Sedulous.UI.Tests/src/PopupLayerTests.bf (faithful). Beef property `root.PopupLayer` ->
// root->GetPopupLayer() (our PopupLayer is created lazily on first access rather than in the ctor - a
// module-cycle divergence - so the "is last child" tests capture it into a local first, mirroring Beef's
// eager ctor creation). Popups are MakeRef'd and passed by .Get(); `new`+`delete` -> RefPtr (RAII).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    foundation::RefPtr<RootView> MakeRoot() { return foundation::MakeRef<RootView>(foundation::DefaultAllocator()); }
    foundation::RefPtr<TestView> MakeView(f32 w = 50.0f, f32 h = 30.0f)
    {
        return foundation::MakeRef<TestView>(foundation::DefaultAllocator(), w, h);
    }

    class TestPopupOwner final : public IPopupOwner
    {
    public:
        bool* Notified;
        View** ClosedPopup;
        TestPopupOwner(bool* notified, View** closedPopup)
            : Notified(notified), ClosedPopup(closedPopup)
        {
        }
        void OnPopupClosed(View* popup) override
        {
            *Notified = true;
            *ClosedPopup = popup;
        }
        // Not a View - cascade-close treats null as "outside any popup" (unused by these tests).
        [[nodiscard]] View* OwnerView() override { return nullptr; }
    };
}

TEST_CASE("popup-layer: RootView_HasPopupLayer")
{
    auto root = MakeRoot();
    CHECK(root->GetPopupLayer() != nullptr);
}

TEST_CASE("popup-layer: RootView_PopupLayerIsLastChild")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    PopupLayer* pl = root->GetPopupLayer();
    root->AddView(MakeView().Get());
    CHECK(root->GetChildAt(root->ChildCount() - 1) == pl);
}

TEST_CASE("popup-layer: RootView_PopupLayerStaysLast")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    PopupLayer* pl = root->GetPopupLayer();
    root->AddView(MakeView().Get());
    root->AddView(MakeView().Get());
    root->AddView(MakeView().Get());
    CHECK(root->GetChildAt(root->ChildCount() - 1) == pl);
}

TEST_CASE("popup-layer: ShowPopup_IncreasesCount")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    CHECK(root->GetPopupLayer()->PopupCount() == 1u);
}

TEST_CASE("popup-layer: ClosePopup_DecreasesCount")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    CHECK(root->GetPopupLayer()->PopupCount() == 0u);
}

TEST_CASE("popup-layer: ShowPopup_OwnedView_UnregisteredOnClose")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    const ViewId popupId = popup->Id;
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    // Detached -> no longer in the registry (Beef also deletes it; RefPtr keeps the test's local alive).
    CHECK(ctx.GetViewById(popupId) == nullptr);
}

TEST_CASE("popup-layer: ShowPopup_NotOwned_NotDeletedOnClose")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, false);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    CHECK(popup->Id.IsValid()); // still alive (test's RefPtr owns it)
}

TEST_CASE("popup-layer: ShowPopup_NotifiesOwner")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());

    bool notified = false;
    View* closedPopup = nullptr;
    TestPopupOwner owner(&notified, &closedPopup);
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), &owner, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());
    CHECK(notified);
}

TEST_CASE("popup-layer: Modal_HasModalPopup")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, true, true);
    CHECK(root->GetPopupLayer()->HasModalPopup());
    CHECK(root->GetPopupLayer()->TopmostModalPopup() == popup.Get());
}

TEST_CASE("popup-layer: Modal_HitTestBlocksBackground")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 800, 600);
    LayoutPass(ctx, root.Get());
    auto bg = MakeView(800, 600);
    root->AddView(bg.Get());
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, true, true);
    LayoutPass(ctx, root.Get());

    View* hit = root->HitTest(Float2{700, 500}); // outside the popup, over bg
    CHECK(hit != bg.Get());
}

TEST_CASE("popup-layer: HandleClickOutside_ClosesCloseOnClick")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    LayoutPass(ctx, root.Get());
    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    CHECK(root->GetPopupLayer()->PopupCount() == 1u);
    root->GetPopupLayer()->HandleClickOutside(nullptr, 0); // outside any popup, LMB
    CHECK(root->GetPopupLayer()->PopupCount() == 0u);
}

TEST_CASE("popup-layer: ShowPopup_PushesFocus")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    auto view = MakeView();
    view->IsFocusable = true;
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());

    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);

    CHECK(ctx.GetFocusManager()->FocusedView() == nullptr);
    CHECK(ctx.GetFocusManager()->FocusStackDepth() == 1u);
}

TEST_CASE("popup-layer: ClosePopup_PopsFocus")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    root->ViewportSize = Float2{800, 600};
    auto view = MakeView();
    view->IsFocusable = true;
    root->AddView(view.Get());
    ctx.GetFocusManager()->SetFocus(view.Get());

    auto popup = MakeView(100, 50);
    root->GetPopupLayer()->ShowPopup(popup.Get(), nullptr, 10, 10, true, false, true);
    root->GetPopupLayer()->ClosePopup(popup.Get());

    CHECK(ctx.GetFocusManager()->FocusedView() == view.Get());
}
