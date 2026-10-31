// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Materials - reflection implementation unit: MaterialAsset's reflected surface.
//
// Kept out of the MaterialAsset.cppm interface, because REFLECT_MEMBERS bodies there make GCC emit
// a gcm cluster. MaterialAsset wraps a MaterialSource by value, and MaterialSource derives Object
// (RefCounted deletes its copy constructor), so it cannot marshal through a Variant. It is exposed
// as a nested property (TypeBuilder::Nested): tooling reaches the member in place through its
// address and recurses into MaterialSource's own reflected properties.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module materials.pipeline;

import foundation.core;
import pipeline.core;
import foundation.materials.resource; // MaterialSource (the nested reflected type)

using namespace foundation::core;
using namespace foundation::materials;

namespace pipeline{
    REFLECT_MEMBERS(MaterialAsset, "rtti::pipeline::materials")
    {
        builder.Attribute("displayName", String(u8"Material"))
            .Attribute("category", String(u8"Materials"))
            .Nested<&MaterialAsset::source>("source")
            .PropAttribute("displayName", String(u8"Source"));
    }
}
