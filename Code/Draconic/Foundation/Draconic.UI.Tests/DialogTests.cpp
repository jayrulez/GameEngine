// Ported from Sedulous.UI.Tests/src/DialogTests.bf (faithful). Beef nullable String -> non-empty check;
// `scope Dialog` / `new Dialog` -> RefPtr (RAII, ownsView:false so the test controls lifetime); Event
// delegate captures -> lambda captures; ctx.MutationQueue.Drain() -> ctx.MutationQueueRef().Drain().
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<Dialog> MakeDialog(StringView title)
{
    return foundation::MakeRef<Dialog>(foundation::DefaultAllocator(), title);
}

TEST_CASE("dialog: Dialog_TitleProperty")
{
    auto dlg = MakeDialog(u8"Hello");
    CHECK(!dlg->Title.IsEmpty());
    CHECK(dlg->Title == u8"Hello");
}

TEST_CASE("dialog: Dialog_DefaultResult")
{
    auto dlg = MakeDialog(u8"Test");
    CHECK(dlg->Result == DialogResult::None);
}

TEST_CASE("dialog: Alert_Factory")
{
    auto dlg = Dialog::Alert(u8"Title", u8"Message");
    CHECK(!dlg->Title.IsEmpty());
    CHECK(dlg->Title == u8"Title");
}

TEST_CASE("dialog: Confirm_Factory")
{
    auto dlg = Dialog::Confirm(u8"Confirm", u8"Are you sure?");
    CHECK(!dlg->Title.IsEmpty());
    CHECK(dlg->Title == u8"Confirm");
}

TEST_CASE("dialog: Close_FiresOnClosed")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto dlg = MakeDialog(u8"Test");
    dlg->AddButton(u8"OK", DialogResult::OK);

    bool closed = false;
    DialogResult closedResult = DialogResult::None;
    dlg->OnClosed.Add(Event<void(Dialog*, DialogResult)>::Handler{
        [&closed, &closedResult](Dialog*, DialogResult r)
        {
            closed = true;
            closedResult = r;
        }});

    dlg->Show(&ctx, false); // ownsView=false so we control deletion

    CHECK(root->GetPopupLayer()->PopupCount() == 1);

    dlg->Close(DialogResult::OK);
    ctx.MutationQueueRef().Drain();

    CHECK(closed);
    CHECK(closedResult == DialogResult::OK);
    CHECK(root->GetPopupLayer()->PopupCount() == 0);
}

TEST_CASE("dialog: Show_CreatesModalPopup")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto dlg = MakeDialog(u8"Modal Test");
    dlg->Show(&ctx, false);

    CHECK(root->GetPopupLayer()->PopupCount() == 1);
    CHECK(root->GetPopupLayer()->HasModalPopup());

    dlg->Close(DialogResult::Cancel);
    ctx.MutationQueueRef().Drain();

    CHECK(root->GetPopupLayer()->PopupCount() == 0);
    CHECK(!root->GetPopupLayer()->HasModalPopup());
}

TEST_CASE("dialog: NoneButton_IsCallerManaged")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto dlg = MakeDialog(u8"Test");
    Button* custom = dlg->AddButton(u8"Validate", DialogResult::None);

    bool closed = false;
    dlg->OnClosed.Add(Event<void(Dialog*, DialogResult)>::Handler{[&closed](Dialog*, DialogResult)
                                                                  { closed = true; }});

    dlg->Show(&ctx, false);
    CHECK(root->GetPopupLayer()->PopupCount() == 1);

    // A None button does NOT auto-close - the caller's handler decides (validation flows).
    custom->OnClick.Invoke(custom);
    ctx.MutationQueueRef().Drain();
    CHECK(!closed);
    CHECK(root->GetPopupLayer()->PopupCount() == 1);

    dlg->Close(DialogResult::OK);
    ctx.MutationQueueRef().Drain();
    CHECK(closed);
    CHECK(root->GetPopupLayer()->PopupCount() == 0);
}
