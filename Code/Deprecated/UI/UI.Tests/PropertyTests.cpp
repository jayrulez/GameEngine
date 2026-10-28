// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/PropertyTests.bf (faithful; Beef `Value` property -> Value()/
// SetValue(), Beef delegate -> core::Function, scope Property -> stack Property).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;

using namespace foundation::ui;
using namespace foundation::core;

TEST_CASE("property: InitialValue")
{
    Property<f32> prop{42.0f};
    CHECK(prop.Value() == 42.0f);
}

TEST_CASE("property: DefaultValue")
{
    Property<i32> prop;
    CHECK(prop.Value() == 0);
}

TEST_CASE("property: SetValue_FiresChanged")
{
    Property<f32> prop{0.0f};
    f32 received = -1.0f;
    prop.Changed.Add(Function<void(f32)>{[&](f32 val) { received = val; }});
    prop.SetValue(100.0f);
    CHECK(received == 100.0f);
}

TEST_CASE("property: SetValue_SameValue_DoesNotFire")
{
    Property<i32> prop{42};
    i32 fireCount = 0;
    prop.Changed.Add(Function<void(i32)>{[&](i32) { ++fireCount; }});
    prop.SetValue(42); // same value
    CHECK(fireCount == 0);
}

TEST_CASE("property: SetValue_DifferentValue_Fires")
{
    Property<i32> prop{0};
    i32 fireCount = 0;
    prop.Changed.Add(Function<void(i32)>{[&](i32) { ++fireCount; }});
    prop.SetValue(1);
    prop.SetValue(2);
    prop.SetValue(3);
    CHECK(fireCount == 3);
}

TEST_CASE("property: SetSilent_DoesNotFire")
{
    Property<f32> prop{0.0f};
    i32 fireCount = 0;
    prop.Changed.Add(Function<void(f32)>{[&](f32) { ++fireCount; }});
    prop.SetSilent(100.0f);
    CHECK(prop.Value() == 100.0f);
    CHECK(fireCount == 0);
}

TEST_CASE("property: BindTo_OneWay")
{
    Property<f32> source{0.0f};
    Property<f32> target{0.0f};
    source.BindTo(target);

    source.SetValue(50.0f);
    CHECK(target.Value() == 50.0f);

    // Reverse should not propagate back.
    target.SetValue(99.0f);
    CHECK(source.Value() == 50.0f);
}

TEST_CASE("property: BindTwoWay_BothDirections")
{
    Property<i32> a{0};
    Property<i32> b{0};
    a.BindTwoWay(b);

    a.SetValue(10);
    CHECK(b.Value() == 10);

    b.SetValue(20);
    CHECK(a.Value() == 20);
}

TEST_CASE("property: BindTwoWay_LoopGuard")
{
    Property<i32> a{0};
    Property<i32> b{0};
    a.BindTwoWay(b);

    a.SetValue(42); // must not infinite loop
    CHECK(a.Value() == 42);
    CHECK(b.Value() == 42);
}

TEST_CASE("property: Bool_Property")
{
    Property<bool> prop{false};
    bool received = false;
    prop.Changed.Add(Function<void(bool)>{[&](bool val) { received = val; }});
    prop.SetValue(true);
    CHECK(received == true);
    CHECK(prop.Value() == true);
}
