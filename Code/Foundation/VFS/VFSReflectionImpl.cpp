// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VFS - reflection implementation unit: SourcePath's reflected surface.
//
// Kept out of the :source_path interface partition because REFLECT_* bodies make GCC emit a gcm
// cluster. SourcePath's value is private, so it reflects as read accessors and a StringView
// constructor rather than member properties.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.vfs;

import foundation.core;

using namespace foundation::core;

namespace foundation::vfs
{
    REFLECT_VALUE(SourcePath, "rtti::vfs")
    {
        builder.Constructor<StringView>()
            .Method<&SourcePath::View>("View")
            .Method<&SourcePath::IsEmpty>("IsEmpty")
            .Method<&SourcePath::FileName>("FileName")
            .Method<&SourcePath::Stem>("Stem")
            .Method<&SourcePath::Extension>("Extension")
            .Method<&SourcePath::Directory>("Directory");
    }

    void RegisterVFSReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_SourcePath();
            return true;
        }();
        (void)once;
    }
}
