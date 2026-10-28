// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Render.Pipeline - the render profile assets' reflection: their display names, which the editor
// shows as the asset's kind (as File > New's creators name them). Kept out of the interface, as
// REFLECT_* bodies are.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module render.pipeline;

import foundation.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(EnvironmentProfileAsset, "rtti::pipeline::render")
    {
        builder.Attribute("displayName", String(u8"Environment Profile"));
    }

    REFLECT_MEMBERS(PostProcessProfileAsset, "rtti::pipeline::render")
    {
        builder.Attribute("displayName", String(u8"Post Process Profile"));
    }
}
