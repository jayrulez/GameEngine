// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MaterialEditorPage tests (headless): which row a property gets is a pure function of its
// declared type, covered here; the page itself needs a live application host.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.materials;
import editor.materials;

using namespace foundation::core;
namespace materials = foundation::materials;
using editor::MaterialPropertyRow;
using editor::MaterialPropertyRowFor;

TEST_CASE("material page: only a declared colour gets a colour picker")
{
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Color) ==
          MaterialPropertyRow::Color);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::ColorHdr) ==
          MaterialPropertyRow::ColorWithIntensity);
    // A Float4 that is not declared a colour is four numbers, not a swatch.
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float4) ==
          MaterialPropertyRow::Numbers4);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float) ==
          MaterialPropertyRow::Number);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Texture2D) ==
          MaterialPropertyRow::Texture);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Sampler) ==
          MaterialPropertyRow::None);
}

// Every value a shader's properties block can declare has a row of its own size: a custom shader's
// float2, float3 and int are edited as what they are, not as four floats.
TEST_CASE("material page: a shader's float2, float3 and int get rows of their own")
{
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float2) ==
          MaterialPropertyRow::Numbers2);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Float3) ==
          MaterialPropertyRow::Numbers3);
    CHECK(MaterialPropertyRowFor(materials::MaterialPropertyType::Int) ==
          MaterialPropertyRow::Integer);

}

TEST_CASE("material page: the builtin PBR template's colours land on colour rows")
{
    RefPtr<materials::Material> pbr = materials::BuiltinMaterialTemplate(u8"forward");
    REQUIRE(pbr);
    CHECK(MaterialPropertyRowFor(pbr->FindProperty(u8"BaseColor")->type) ==
          MaterialPropertyRow::Color);
    CHECK(MaterialPropertyRowFor(pbr->FindProperty(u8"EmissiveColor")->type) ==
          MaterialPropertyRow::ColorWithIntensity);
}
