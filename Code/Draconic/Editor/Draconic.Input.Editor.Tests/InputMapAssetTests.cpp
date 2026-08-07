// draconic.input.editor tests: InputMapAsset's reflected surface - a Nested `map` property whose
// InputMap tree (sets -> actions -> bindings) is traversable via reflection (scriptability). The
// input editor page itself stays bespoke; this proves the asset is reflection-visible.

#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.editor;
import draconic.input;
import draconic.input.editor;

using namespace draconic::foundation;
using namespace draconic::input;

TEST_CASE("input editor: InputMapAsset exposes its InputMap as a Nested, traversable property")
{
    RegisterInputMapAsset(); // registers the asset + the whole InputMap tree reflection

    const PropertyInfo* mapProp = FindProperty(InputMapAsset::StaticType(), "map");
    REQUIRE(mapProp != nullptr);
    CHECK(IsNested(*mapProp));
    CHECK(mapProp->type == &TypeOf<InputMap>());

    InputMapAsset asset;
    asset.SeedDefaultContent(); // one "Gameplay" set with Move/Look/Jump/Fire
    Instance assetInst = Instance::From(&asset);

    // Recurse: asset.map -> InputMap.sets (container) -> the ActionSet -> actions (container).
    void* mapAddr = mapProp->address(assetInst);
    REQUIRE(mapAddr != nullptr);
    const Instance mapInst(mapAddr, mapProp->type);

    const PropertyInfo* setsProp = FindProperty(*mapProp->type, "sets");
    REQUIRE(setsProp != nullptr);
    REQUIRE(IsContainer(*setsProp->type));
    const Instance setsInst(setsProp->address(mapInst), setsProp->type);
    REQUIRE(ContainerSize(*setsProp->type->container, setsInst) == 1u);

    Variant setElem = ContainerGetAt(*setsProp->type->container, setsInst, 0);
    const Instance setInst(setElem.ValuePointer(), setElem.Type());
    const PropertyInfo* nameProp = FindProperty(*setElem.Type(), "name");
    REQUIRE(nameProp != nullptr);
    CHECK(GetProperty(*nameProp, setInst).Get<String>() == StringView(u8"Gameplay"));

    const PropertyInfo* actionsProp = FindProperty(*setElem.Type(), "actions");
    REQUIRE(actionsProp != nullptr);
    CHECK(IsNested(*actionsProp));
    REQUIRE(IsContainer(*actionsProp->type));
    const Instance actionsInst(actionsProp->address(setInst), actionsProp->type);
    CHECK(ContainerSize(*actionsProp->type->container, actionsInst) == 4u); // Move/Look/Jump/Fire
}
