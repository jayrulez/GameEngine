// Reflection track P1: UIDocumentAsset (markup) + UIThemeAsset (stylesheet) reflected surfaces.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.ui.editor;

using namespace draconic::foundation;

TEST_CASE("reflection-p1: UIDocumentAsset exposes markup and round-trips")
{
    draconic::ui::RegisterUIAssets();
    const TypeInfo& type = draconic::ui::UIDocumentAsset::StaticType();

    CHECK(PropertyCount(type) == 1u);
    const PropertyInfo* markup = FindProperty(type, "markup");
    REQUIRE(markup != nullptr);

    draconic::ui::UIDocumentAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*markup, inst, Variant::From(String(u8"<panel/>"))).IsOk());
    CHECK(GetProperty(*markup, inst).Get<String>().AsView() == StringView(u8"<panel/>"));
    CHECK(asset.markup.AsView() == StringView(u8"<panel/>"));
}

TEST_CASE("reflection-p1: UIThemeAsset exposes stylesheet and round-trips")
{
    draconic::ui::RegisterUIAssets();
    const TypeInfo& type = draconic::ui::UIThemeAsset::StaticType();

    CHECK(PropertyCount(type) == 1u);
    const PropertyInfo* sheet = FindProperty(type, "stylesheet");
    REQUIRE(sheet != nullptr);

    draconic::ui::UIThemeAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*sheet, inst, Variant::From(String(u8"panel{}"))).IsOk());
    CHECK(asset.stylesheet.AsView() == StringView(u8"panel{}"));
}
