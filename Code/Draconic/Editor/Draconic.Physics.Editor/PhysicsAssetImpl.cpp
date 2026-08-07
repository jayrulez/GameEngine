// Draconic::PhysicsEditor - reflection implementation unit: physics asset reflected surface.
//
// Kept OUT of the PhysicsAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). The classes declare identity via DRACONIC_OBJECT in
// the interface; this unit defines their StaticType() WITH properties + tooling attributes, plus
// the CollisionCookKind enum reflection. Reflection track P1.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.physics.editor;

import draconic.foundation;
import draconic.editor;

using namespace draconic::foundation;

namespace draconic::physics
{
    DRACONIC_REFLECT_ENUM(CollisionCookKind, "draconic::physics")
    {
        builder.Value("ConvexHull", CollisionCookKind::ConvexHull);
        builder.Value("TriangleMesh", CollisionCookKind::TriangleMesh);
    }

    DRACONIC_REFLECT(CollisionShapeAsset, "draconic::physics")
    {
        builder.Attribute("displayName", String(u8"Collision Shape"))
            .Attribute("category", String(u8"Physics"))
            .Property<&CollisionShapeAsset::sourceMesh>("sourceMesh")
            .PropAttribute("displayName", String(u8"Source Mesh"))
            .Property<&CollisionShapeAsset::cook>("cook")
            .PropAttribute("displayName", String(u8"Cook Mode"))
            .Property<&CollisionShapeAsset::hullTolerance>("hullTolerance")
            .PropAttribute("displayName", String(u8"Hull Tolerance"))
            .PropAttribute("range", Float4{0.0f, 0.1f, 0.0001f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"cook=0")); // convex hull only
    }

    DRACONIC_REFLECT(PhysicalMaterialAsset, "draconic::physics")
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
            DraconicRegisterEnum_CollisionCookKind();
            return true;
        }();
        (void)once;
    }
}
