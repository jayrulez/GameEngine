// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::VG - reflection implementation unit: the vector image asset's reflected surface.
//
// Kept out of the VGAsset.cppm interface (REFLECT_MEMBERS bodies make GCC emit a gcm cluster).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module vg.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(VectorImageAsset, "rtti::pipeline::vg")
    {
        builder.Attribute("displayName", String(u8"Vector Image"))
            .Attribute("category", String(u8"Vector Graphics"));
    }
}
