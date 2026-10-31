// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Input - reflection implementation unit: InputMapAsset's reflected surface.
//
// InputMapAsset wraps an InputMap by value; InputMap is a nested list of lists (sets hold actions,
// actions hold bindings). It is exposed as a nested property (TypeBuilder::Nested) so tooling and
// scripts can traverse the whole tree in place through container reflection, without marshalling
// it by value. The input editor page is hand-written; this reflection is for scripts, not a
// generated inspector. Kept out of the interface, as REFLECT_MEMBERS bodies are for GCC.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module input.pipeline;

import foundation.core;
import pipeline.core;
import foundation.input; // InputMap (the nested reflected type)

using namespace foundation::core;
using namespace foundation::input;

namespace pipeline{
    REFLECT_MEMBERS(InputMapAsset, "rtti::pipeline::input")
    {
        builder.Attribute("displayName", String(u8"Input Map"))
            .Attribute("category", String(u8"Input"))
            .Nested<&InputMapAsset::m_map>("map")
            .PropAttribute("displayName", String(u8"Map"));
    }
}
