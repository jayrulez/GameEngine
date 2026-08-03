// Reflection track P1: ImageAsset's reflected surface (its colorSpace enum property).
#include <doctest/doctest.h>
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"
import draconic.core;
import draconic.image;
import draconic.image.editor;

using namespace draconic::core;

TEST_CASE("reflection-p1: ImageAsset exposes colorSpace as a reflected enum")
{
    draconic::image::RegisterImageAsset();
    const TypeInfo& type = draconic::image::ImageAsset::StaticType();

    CHECK(PropertyCount(type) == 1u);
    const PropertyInfo* cs = FindProperty(type, "colorSpace");
    REQUIRE(cs != nullptr);
    REQUIRE(cs->type != nullptr);
    CHECK(IsEnum(*cs->type));

    // Round-trip through the address escape-hatch (enum Variants aren't constructible at runtime).
    draconic::image::ImageAsset asset;
    Instance inst = Instance::From(&asset);
    REQUIRE(cs->address != nullptr);
    auto* field = static_cast<draconic::image::ImageColorSpace*>(cs->address(inst));
    REQUIRE(field != nullptr);
    *field = draconic::image::ImageColorSpace::Linear;
    CHECK(asset.colorSpace == draconic::image::ImageColorSpace::Linear);
}
