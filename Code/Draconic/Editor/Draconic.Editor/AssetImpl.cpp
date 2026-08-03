// Draconic::Editor - reflection implementation unit: the Asset base's reflected surface.
//
// Kept OUT of the Asset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster; see
// gcc-module-interface-hygiene). Asset::StaticType() gains its fileName property here, so every
// concrete asset inherits it through the base chain (FindProperty walks bases). RegisterAssetReflection
// also registers SourcePath's reflection (fileName's type). Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.editor;

import draconic.core;
import draconic.vfs;

using namespace draconic::core;

namespace draconic::editor
{
    DRACONIC_REFLECT(Asset, "draconic::editor")
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
            draconic::vfs::RegisterVFSReflection(); // SourcePath (fileName's type)
            GlobalTypeRegistry().Register(Asset::StaticType(), TypeDomain(u8"Editor"));
            return true;
        }();
        (void)once;
    }
}
