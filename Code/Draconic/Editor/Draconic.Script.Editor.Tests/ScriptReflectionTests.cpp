// Reflection track P1: ScriptClassAsset's reflected surface (its language string).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.script.editor;

using namespace draconic::foundation;

TEST_CASE("reflection-p1: ScriptClassAsset exposes language and round-trips")
{
    draconic::script::RegisterScriptAssets();
    const TypeInfo& type = draconic::script::ScriptClassAsset::StaticType();

    CHECK(PropertyCount(type) == 1u);
    const PropertyInfo* lang = FindProperty(type, "language");
    REQUIRE(lang != nullptr);
    CHECK(FindAttribute(*lang, u8"displayName") != nullptr);

    draconic::script::ScriptClassAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*lang, inst, Variant::From(String(u8"angelscript"))).IsOk());
    CHECK(GetProperty(*lang, inst).Get<String>().AsView() == StringView(u8"angelscript"));
    CHECK(asset.language.AsView() == StringView(u8"angelscript"));
}
