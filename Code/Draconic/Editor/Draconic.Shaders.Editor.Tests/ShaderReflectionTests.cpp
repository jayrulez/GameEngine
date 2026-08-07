// Reflection track P1: ShaderAsset's reflected surface (name + fragmentFile strings).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.shaders.editor;

using namespace draconic::foundation;

TEST_CASE("reflection-p1: ShaderAsset exposes name + fragmentFile with labels, and round-trips")
{
    draconic::shaders::RegisterShaderAsset();
    const TypeInfo& type = draconic::shaders::ShaderAsset::StaticType();

    CHECK(PropertyCount(type) == 2u);
    const PropertyInfo* name = FindProperty(type, "name");
    const PropertyInfo* frag = FindProperty(type, "fragmentFile");
    REQUIRE(name != nullptr);
    REQUIRE(frag != nullptr);
    CHECK(FindAttribute(*frag, u8"displayName") != nullptr);

    draconic::shaders::ShaderAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*name, inst, Variant::From(String(u8"Lit"))).IsOk());
    CHECK(asset.name.AsView() == StringView(u8"Lit"));
    CHECK(SetProperty(*frag, inst, Variant::From(String(u8"lit.frag.hlsl"))).IsOk());
    CHECK(GetProperty(*frag, inst).Get<String>().AsView() == StringView(u8"lit.frag.hlsl"));
}
