// Reflection track P1: FontAsset's reflected surface + the FontBakeMode enum. Verifies the
// authored scalar/enum/string fields enumerate with attributes, round-trip through get/set, and
// that the bake-mode enum resolves named values. The `sizes` ramp is intentionally unreflected.
#include <doctest/doctest.h>
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"
#include <initializer_list>
import draconic.core;
import draconic.fonts.editor;

using namespace draconic::core;

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
    draconic::fonts::RegisterFontAsset();
    const TypeInfo& type = draconic::fonts::FontAsset::StaticType();

    CHECK(CEq(type.name, "FontAsset"));
    // family + mode + dfSize + first/lastCodepoint + atlasWidth/Height = 7 (sizes deferred).
    CHECK(PropertyCount(type) == 7u);
    for (const char* name : {"family", "mode", "dfSize", "firstCodepoint", "lastCodepoint",
                             "atlasWidth", "atlasHeight"})
    {
        CHECK_MESSAGE(FindProperty(type, name) != nullptr, name);
    }
    CHECK(FindProperty(type, "sizes") == nullptr); // Array<f32> not reflected in P1

    // dfSize is DistanceField-only (a visibleWhen the generic page evaluates).
    const PropertyInfo* dfSize = FindProperty(type, "dfSize");
    REQUIRE(dfSize != nullptr);
    const Attribute* vis = FindAttribute(*dfSize, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    CHECK(vis->value.Get<String>().AsView() == StringView(u8"mode=1"));
}

TEST_CASE("reflection-p1: FontAsset string/scalar properties round-trip")
{
    draconic::fonts::RegisterFontAsset();
    const TypeInfo& type = draconic::fonts::FontAsset::StaticType();

    draconic::fonts::FontAsset asset;
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
    draconic::fonts::RegisterFontAsset();

    const TypeInfo& mode = TypeOf<draconic::fonts::FontBakeMode>();
    CHECK(IsEnum(mode));
    CHECK(EnumeratorCount(mode) == 2u);
    CHECK(CEq(EnumValueName(mode, 0), "RasterRamp"));
    CHECK(CEq(EnumValueName(mode, 1), "DistanceField"));

    const PropertyInfo* modeProp =
        FindProperty(draconic::fonts::FontAsset::StaticType(), "mode");
    REQUIRE(modeProp != nullptr);
    REQUIRE(modeProp->type != nullptr);
    CHECK(IsEnum(*modeProp->type));
}
