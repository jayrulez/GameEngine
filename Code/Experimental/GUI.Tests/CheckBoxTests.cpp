// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - CheckBox tests: toggle on click, programmatic set, change callback, and
// tag for CSS.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
}

TEST_CASE("checkbox: defaults")
{
    auto cb = Make<CheckBox>();
    CHECK_FALSE(cb->IsChecked());
    CHECK(cb->GetTag() == core::StringView(u8"checkbox"));
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
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{100.0f, 100.0f});
    auto cb = Make<CheckBox>();
    cb->SetSize(core::Float2{24.0f, 24.0f});
    root->AddChild(cb.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(core::Float2{12.0f, 12.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 12.0f}, MouseButton::Left);
    CHECK(cb->IsChecked());

    d->InjectMouseDown(core::Float2{12.0f, 12.0f}, MouseButton::Left);
    d->InjectMouseUp(core::Float2{12.0f, 12.0f}, MouseButton::Left);
    CHECK_FALSE(cb->IsChecked());
}

TEST_CASE("checkbox: draws box outline, plus a check when checked")
{
    auto cb = Make<CheckBox>();
    cb->SetSize(core::Float2{24.0f, 24.0f});

    foundation::vg::VGContext ctxUnchecked;
    DrawContext dcU{ctxUnchecked};
    cb->Draw(dcU);
    const core::usize unchecked = ctxUnchecked.GetBatch().vertices.Size();
    CHECK(unchecked > 0); // outline

    cb->SetChecked(true);
    foundation::vg::VGContext ctxChecked;
    DrawContext dcC{ctxChecked};
    cb->Draw(dcC);
    CHECK(ctxChecked.GetBatch().vertices.Size() > unchecked); // outline + check fill
}
