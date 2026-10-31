// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Materials.Resource - reflection implementation unit: MaterialSource's reflected surface.
//
// Kept out of the MaterialResource.cppm interface, because REFLECT_MEMBERS bodies there make GCC
// emit a gcm cluster. The class declares its identity via RTTI_OBJECT in the interface; this unit
// defines MaterialSource::StaticType() with its properties and data version, so tooling that
// recurses into it (a MaterialAsset's nested `source`) sees the authored scalar surface. The
// render-state fields are reflected enums (RegisterMaterialsTypeReflection). The parallel cooked
// arrays (propertyNames and the rest, textureSlots, textureIds) are cook output, not authored
// field by field, so they are not reflected.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.materials.resource;

import foundation.core;

using namespace foundation::core;

namespace foundation::materials
{
    REFLECT_MEMBERS(MaterialSource, "rtti::materials")
    {
        // 3: the property arrays use full names (propertyNames...). 4: the effect groups
        // (`effects`); a version-3 payload still reads, without them, and its PBR wind lanes
        // migrate into a Wind group (MaterialSource::MigrateWindLanes).
        builder.DataVersion(4)
            .ReadsDataVersionsFrom(3)
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
