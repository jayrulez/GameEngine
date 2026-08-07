// Draconic::MaterialResource - reflection implementation unit: MaterialSource's reflected surface.
//
// Kept OUT of the MaterialResource.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). The class declares its identity via DRACONIC_OBJECT
// in the interface; this unit defines MaterialSource::StaticType() WITH properties + the data
// version, so tooling that recurses into it (a MaterialAsset's nested `source`) sees the authored
// scalar surface. The render-state fields stay u8 for now (their enum-name retype is a later step);
// the parallel cooked arrays (propNames/... , textureSlots/...) are internal cook output, not
// per-field authored, so they are intentionally not reflected. Reflection track P1.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.materials.resource;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::materials
{
    DRACONIC_REFLECT(MaterialSource, "draconic::materials")
    {
        builder.DataVersion(2) // matches the prior DRACONIC_DEFINE_OBJECT_VERSIONED(2)
            .Property<&MaterialSource::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .Property<&MaterialSource::shaderId>("shaderId")
            .PropAttribute("displayName", String(u8"Shader"))
            .Property<&MaterialSource::shaderName>("shaderName")
            .PropAttribute("displayName", String(u8"Shader Name"))
            .PropAttribute("description",
                           String(u8"Builtin shader name fallback when Shader is unset"))
            .Property<&MaterialSource::shaderFlags>("shaderFlags")
            .PropAttribute("displayName", String(u8"Shader Flags"))
            .Property<&MaterialSource::blendMode>("blendMode")
            .PropAttribute("displayName", String(u8"Blend Mode"))
            .Property<&MaterialSource::depthMode>("depthMode")
            .PropAttribute("displayName", String(u8"Depth Mode"))
            .Property<&MaterialSource::cullMode>("cullMode")
            .PropAttribute("displayName", String(u8"Cull Mode"))
            .Property<&MaterialSource::vertexLayout>("vertexLayout")
            .PropAttribute("displayName", String(u8"Vertex Layout"))
            .Property<&MaterialSource::samplerU>("samplerU")
            .PropAttribute("displayName", String(u8"Sampler U"))
            .Property<&MaterialSource::samplerV>("samplerV")
            .PropAttribute("displayName", String(u8"Sampler V"));
    }
}
