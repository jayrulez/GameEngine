// Draconic GUI - CheckBox tests: toggle on click, programmatic set, change callback, and
// tag for CSS.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }
}

TEST_CASE("checkbox: defaults")
{
    auto cb = Make<CheckBox>();
    CHECK_FALSE(cb->IsChecked());
    CHECK(cb->GetTag() == foundation::StringView(u8"checkbox"));
}

TEST_CASE("checkbox: toggle and set fire the change callback")
{
    auto cb = Make<CheckBox>();
    int changes = 0;
    bool last = false;
    cb->SetOnCheckedChanged(
        [&](bool checked)
        {
            ++changes;
            last = checked;
        });

    cb->Toggle();
    CHECK(cb->IsChecked());
    CHECK(changes == 1);
    CHECK(last == true);

    cb->Toggle();
    CHECK_FALSE(cb->IsChecked());
    CHECK(changes == 2);
    CHECK(last == false);

    cb->SetChecked(true);
    CHECK(changes == 3);
    cb->SetChecked(true); // no change -> no callback
    CHECK(changes == 3);
}

TEST_CASE("checkbox: click through the dispatcher toggles it")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{100.0f, 100.0f});
    auto cb = Make<CheckBox>();
    cb->SetSize(foundation::Float2{24.0f, 24.0f});
    root->AddChild(cb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{12.0f, 12.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{12.0f, 12.0f}, MouseButton::Left);
    CHECK(cb->IsChecked());

    d->InjectMouseDown(foundation::Float2{12.0f, 12.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{12.0f, 12.0f}, MouseButton::Left);
    CHECK_FALSE(cb->IsChecked());
}

TEST_CASE("checkbox: draws box outline, plus a check when checked")
{
    auto cb = Make<CheckBox>();
    cb->SetSize(foundation::Float2{24.0f, 24.0f});

    draconic::vg::VGContext ctxUnchecked;
    DrawContext dcU{ctxUnchecked};
    cb->Draw(dcU);
    const foundation::usize unchecked = ctxUnchecked.GetBatch().vertices.Size();
    CHECK(unchecked > 0); // outline

    cb->SetChecked(true);
    draconic::vg::VGContext ctxChecked;
    DrawContext dcC{ctxChecked};
    cb->Draw(dcC);
    CHECK(ctxChecked.GetBatch().vertices.Size() > unchecked); // outline + check fill
}
