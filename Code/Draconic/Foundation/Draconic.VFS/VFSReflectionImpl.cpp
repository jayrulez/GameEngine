// Draconic::VFS - reflection implementation unit: SourcePath's reflected surface.
//
// Kept OUT of the :source_path interface partition (DRACONIC_REFLECT_* bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). SourcePath's value is private, so it reflects as
// read accessors + a StringView constructor (the accessor-gated-state convention) rather than
// member properties. Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.vfs;

import draconic.core;

using namespace draconic::core;

namespace draconic::vfs
{
    DRACONIC_REFLECT_VALUE(SourcePath, "draconic::vfs")
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
            DraconicRegisterValue_SourcePath();
            return true;
        }();
        (void)once;
    }
}
