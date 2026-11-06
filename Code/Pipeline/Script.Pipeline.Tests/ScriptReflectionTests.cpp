// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ScriptClassAsset's reflected surface (its language and the class it plays).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import script.pipeline;

using namespace foundation::core;

TEST_CASE("reflection-p1: ScriptClassAsset exposes language and class, and round-trips")
{
    pipeline::RegisterScriptAssets();
    const TypeInfo& type = pipeline::ScriptClassAsset::StaticType();

    CHECK(PropertyCount(type) == 2u);
    const PropertyInfo* lang = FindProperty(type, "language");
    REQUIRE(lang != nullptr);
    CHECK(FindAttribute(*lang, u8"displayName") != nullptr);

    pipeline::ScriptClassAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*lang, inst, Variant::From(String(u8"angelscript"))).IsOk());
    CHECK(GetProperty(*lang, inst).Get<String>().AsView() == StringView(u8"angelscript"));
    CHECK(asset.language.AsView() == StringView(u8"angelscript"));

    const PropertyInfo* cls = FindProperty(type, "className");
    REQUIRE(cls != nullptr);
    CHECK(FindAttribute(*cls, u8"displayName") != nullptr);
    CHECK(SetProperty(*cls, inst, Variant::From(String(u8"Battle"))).IsOk());
    CHECK(asset.className.AsView() == StringView(u8"Battle"));
}
