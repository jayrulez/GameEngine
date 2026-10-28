// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::ModelImporter - the model manifest's reflection: its display name (the editor shows it
// as the asset's kind). Kept out of the interface, as REFLECT_* bodies are.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module modelimporter;

import foundation.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(ModelManifestAsset, "rtti::pipeline::modelimporter")
    {
        builder.Attribute("displayName", String(u8"Model"));
    }
}
