// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Physics - reflection implementation unit: physics asset reflected surface.
//
// Kept out of the PhysicsAsset.cppm interface, because REFLECT_MEMBERS bodies there make GCC emit
// a gcm cluster. The classes declare identity with RTTI_OBJECT in the interface; this unit defines
// their StaticType() with properties and tooling attributes, and the CollisionCookKind enum
// reflection.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module physics.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;
using namespace foundation::physics;

namespace pipeline{
    REFLECT_ENUM(CollisionCookKind, "rtti::pipeline::physics")
    {
        builder.Value("ConvexHull", CollisionCookKind::ConvexHull);
        builder.Value("TriangleMesh", CollisionCookKind::TriangleMesh);
    }

    REFLECT_MEMBERS(CollisionShapeAsset, "rtti::pipeline::physics")
    {
        builder.Attribute("displayName", String(u8"Collision Shape"))
            .Attribute("category", String(u8"Physics"))
            .Property<&CollisionShapeAsset::sourceMesh>("sourceMesh")
            .PropAttribute("displayName", String(u8"Source Mesh"))
            .PropAttribute("referenceType", String(u8"StaticMeshSource")) // what a mesh cooks to, static or skinned
            .Property<&CollisionShapeAsset::cook>("cook")
            .PropAttribute("displayName", String(u8"Cook Mode"))
            .Property<&CollisionShapeAsset::hullTolerance>("hullTolerance")
            .PropAttribute("displayName", String(u8"Hull Tolerance"))
            .PropAttribute("range", Float4{0.0f, 0.1f, 0.0001f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"cook=0")); // convex hull only
    }

    REFLECT_MEMBERS(PhysicalMaterialAsset, "rtti::pipeline::physics")
    {
        builder.Attribute("displayName", String(u8"Physical Material"))
            .Attribute("category", String(u8"Physics"))
            .Property<&PhysicalMaterialAsset::friction>("friction")
            .PropAttribute("range", Float4{0.0f, 2.0f, 0.01f, 0.0f})
            .Property<&PhysicalMaterialAsset::restitution>("restitution")
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&PhysicalMaterialAsset::density>("density")
            .PropAttribute("displayName", String(u8"Density (kg/m^3)"))
            .PropAttribute("range", Float4{0.0f, 20000.0f, 1.0f, 0.0f});
    }

    void RegisterPhysicsAssetReflection()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_CollisionCookKind();
            return true;
        }();
        (void)once;
    }
}
