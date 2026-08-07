#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.model;

using namespace draconic::foundation;
using namespace draconic::model;

TEST_CASE("model: core data types - names round-trip as wide strings")
{
    ModelMaterial mat;
    mat.setName(u8"steel");
    CHECK(mat.name() == u8"steel");

    Model model;
    CHECK(model.meshes().Size() == 0u);
}
