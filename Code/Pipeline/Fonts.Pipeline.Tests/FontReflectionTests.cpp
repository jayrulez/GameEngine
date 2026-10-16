// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// FontAsset's reflected surface + the FontBakeMode enum. Verifies the
// authored scalar/enum/string fields enumerate with attributes, round-trip through get/set, and
// that the bake-mode enum resolves named values. The `sizes` ramp is intentionally unreflected.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>
import foundation.core;
import fonts.pipeline;

using namespace foundation::core;
using namespace pipeline;

namespace
{
    bool CEq(const char* a, const char* b)
    {
        if (a == nullptr || b == nullptr)
        {
            return a == b;
        }
        while (*a != '\0' && *b != '\0')
        {
            if (*a != *b)
            {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }
}

TEST_CASE("reflection-p1: FontAsset exposes its flat authored fields with attributes")
{
    pipeline::RegisterFontAsset();
    const TypeInfo& type = pipeline::FontAsset::StaticType();

    CHECK(CEq(type.name, "FontAsset"));
    // family + mode + distanceFieldSize/Range + first/lastCodepoint + atlasWidth/Height = 8 (sizes is
    // unreflected).
    CHECK(PropertyCount(type) == 8u);
    for (const char* name : {"family", "mode", "distanceFieldSize", "distanceFieldRange", "firstCodepoint",
                             "lastCodepoint", "atlasWidth", "atlasHeight"})
    {
        CHECK_MESSAGE(FindProperty(type, name) != nullptr, name);
    }
    CHECK(FindProperty(type, "sizes") == nullptr); // Array<f32> not reflected

    // distanceFieldSize is DistanceField-only (a visibleWhen the generic page evaluates).
    const PropertyInfo* distanceFieldSize = FindProperty(type, "distanceFieldSize");
    REQUIRE(distanceFieldSize != nullptr);
    const Attribute* vis = FindAttribute(*distanceFieldSize, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    CHECK(vis->value.Get<String>().AsView() == StringView(u8"mode=1"));
}

TEST_CASE("reflection-p1: FontAsset string/scalar properties round-trip")
{
    pipeline::RegisterFontAsset();
    const TypeInfo& type = pipeline::FontAsset::StaticType();

    pipeline::FontAsset asset;
    Instance inst = Instance::From(&asset);

    const PropertyInfo* family = FindProperty(type, "family");
    REQUIRE(family != nullptr);
    CHECK(SetProperty(*family, inst, Variant::From(String(u8"Inter"))).IsOk());
    CHECK(GetProperty(*family, inst).Get<String>().AsView() == StringView(u8"Inter"));
    CHECK(asset.family.AsView() == StringView(u8"Inter"));

    const PropertyInfo* first = FindProperty(type, "firstCodepoint");
    REQUIRE(first != nullptr);
    CHECK(SetProperty(*first, inst, Variant::From<i32>(65)).IsOk());
    CHECK(asset.firstCodepoint == 65);

    const PropertyInfo* aw = FindProperty(type, "atlasWidth");
    REQUIRE(aw != nullptr);
    CHECK(SetProperty(*aw, inst, Variant::From<u32>(2048u)).IsOk());
    CHECK(asset.atlasWidth == 2048u);
}

TEST_CASE("reflection-p1: FontBakeMode enum resolves named values")
{
    pipeline::RegisterFontAsset();

    const TypeInfo& mode = TypeOf<pipeline::FontBakeMode>();
    CHECK(IsEnum(mode));
    CHECK(EnumeratorCount(mode) == 2u);
    CHECK(CEq(EnumValueName(mode, 0), "RasterRamp"));
    CHECK(CEq(EnumValueName(mode, 1), "DistanceField"));

    const PropertyInfo* modeProp =
        FindProperty(pipeline::FontAsset::StaticType(), "mode");
    REQUIRE(modeProp != nullptr);
    REQUIRE(modeProp->type != nullptr);
    CHECK(IsEnum(*modeProp->type));
}
