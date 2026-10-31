// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Core - reflection implementation unit: the Asset base's reflected surface.
//
// Kept out of the Asset.cppm interface, because REFLECT_MEMBERS bodies there make GCC emit a gcm cluster.
// Asset::StaticType() gains its fileName property here, so every concrete asset inherits it
// through the base chain (FindProperty walks bases). RegisterAssetReflection also registers
// SourcePath's reflection (fileName's type).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module pipeline.core;

import foundation.core;
import foundation.vfs;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(Asset, "rtti::pipeline::asset")
    {
        builder.Property<&Asset::fileName>("fileName")
            .PropAttribute("displayName", String(u8"Source File"))
            .PropAttribute("description",
                           String(u8"Source file, relative to the sources mount (empty = embedded)"));
    }

    void RegisterAssetReflection()
    {
        static const bool once = []()
        {
            foundation::vfs::RegisterVFSReflection(); // SourcePath (fileName's type)
            GlobalTypeRegistry().Register(Asset::StaticType(), TypeDomain(u8"Pipeline"));
            return true;
        }();
        (void)once;
    }
}
