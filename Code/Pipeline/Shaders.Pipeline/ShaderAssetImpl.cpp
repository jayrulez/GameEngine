// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Shaders - reflection implementation unit: ShaderAsset's reflected surface.
//
// Kept out of the ShaderAsset.cppm interface (REFLECT_MEMBERS bodies make GCC emit a gcm cluster).
// ShaderAsset::StaticType() gains its authored string properties (name, entry points) here. There
// are no enums, so the reflection comes with StaticType() and needs no registrar.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module shaders.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;
using namespace foundation::shaders;

namespace pipeline{
    REFLECT_MEMBERS(ShaderAsset, "rtti::pipeline::shaders")
    {
        builder.Attribute("displayName", String(u8"Shader"))
            .Attribute("category", String(u8"Rendering"))
            .Property<&ShaderAsset::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .PropAttribute("description", String(u8"Logical name materials reference"))
            .Property<&ShaderAsset::vertexEntryPoint>("vertexEntryPoint")
            .PropAttribute("displayName", String(u8"Vertex Entry Point"))
            .PropAttribute("description", String(u8"The vertex stage's function in the file"))
            .Property<&ShaderAsset::fragmentEntryPoint>("fragmentEntryPoint")
            .PropAttribute("displayName", String(u8"Fragment Entry Point"))
            .PropAttribute("description", String(u8"The fragment stage's function in the file"));
    }
}
